#pragma once

#include "aim_types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

// 共享内存帧读取器（imgui 侧）
// 通过普通文件 mmap + pread 读取 APK 写入的帧数据。
// 头部与帧数据都用 pread 直接读文件（绕过 mmap TLB 缓存一致性问题），
// 并对“撕裂头”（APK 正在写头部 64 字节时被读到半截）做重试防护。
class ShmFrameReader {
public:
    ~ShmFrameReader() {
        if (m_mapped && m_mapped != MAP_FAILED) {
            munmap(m_mapped, m_totalSize);
        }
        if (m_fd >= 0) ::close(m_fd);
    }

    bool valid() const { return m_valid; }

    const ShmFrameHeader* header() const { return m_hdr; }

    // 最近一次 readFrame() 取到的头部（pread 读取，与返回的帧数据同一次快照）。
    // 宽度/高度/rowStride 等以它为准，避免用 mmap 的旧 header（rowStride 可能是
    // APK initShm 写入的错误占位值）。
    const ShmFrameHeader* freshHeader() const {
        return m_freshValid ? &m_freshHdr : m_hdr;
    }

    // 读取最新帧数据，返回指向帧数据的指针，如果无新帧返回 nullptr。
    // 头部与帧数据都改用 pread 直接读取文件（绕过 mmap TLB 缓存一致性问题），
    // 确保看到另一进程通过 write() 写入的最新序号与数据。
    const uint8_t* readFrame() {
        if (!m_valid) return nullptr;

        // 读头部（重试：APK 可能在写 64 字节头部时被读到半截，产生撕裂头）
        ShmFrameHeader freshHdr{};
        bool hdrOk = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (::pread(m_fd, &freshHdr, sizeof(freshHdr), 0) != (ssize_t)sizeof(freshHdr)) {
                if (attempt == 3) { m_headerFail++; return nullptr; }
                continue;
            }
            // magic + 关键字段合法性校验；不合法说明头部被撕裂，重试
            if (freshHdr.magic == SHM_MAGIC &&
                freshHdr.width > 0 && freshHdr.height > 0 &&
                freshHdr.sizePerFrame > 0 &&
                freshHdr.sizePerFrame <= 64 * 1024 * 1024) {
                hdrOk = true;
                break;
            }
            if (attempt == 3) { m_headerFail++; return nullptr; }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (!hdrOk) return nullptr;

        uint32_t seq = freshHdr.lastSeq;
        if (seq == m_lastSeq) { m_noNew++; return nullptr; }
        m_lastSeq = seq;
        m_freshHdr = freshHdr;
        m_freshValid = true;

        int sizePerFrame = (int)freshHdr.sizePerFrame;
        // 帧尺寸若变化（如 rowStride 对齐），自动扩展文件，避免拒绝所有帧
        if (SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT > m_totalSize) {
            ::ftruncate(m_fd, SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT);
            m_totalSize = SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT;
        }
        int bufIdx = (seq % SHM_BUFFER_COUNT) * sizePerFrame;

        // 帧数据也走 pread 到暂存缓冲（重试：跨进程写文件可能读到部分数据）
        if (m_stage.size() < (size_t)sizePerFrame) m_stage.resize(sizePerFrame);
        bool dataOk = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (::pread(m_fd, m_stage.data(), sizePerFrame, SHM_HEADER_SIZE + bufIdx)
                == (ssize_t)sizePerFrame) {
                dataOk = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (!dataOk) { m_dataFail++; return nullptr; }

        m_readOk++;
        return m_stage.data();
    }

    uint64_t frameCount() const { return m_readOk; }

    // 读取文件当前的帧序号（用 pread 绕过 mmap 缓存一致性问题）。
    // 若 APK 持续写帧，该值会不断增大；用于面板诊断“帧源是否在产帧”。
    uint32_t readSeq() const {
        if (!m_valid || m_fd < 0) return 0;
        ShmFrameHeader hdr;
        if (::pread(m_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return 0;
        if (hdr.magic != SHM_MAGIC) return 0;
        return hdr.lastSeq;
    }

    // 读取 APK 侧写帧诊断计数（写进头部，imgui 侧只读）。
    // 用于面板直接判断“APK 是否在持续写帧”：writeAttempts 不增长 = APK 没取到帧。
    ShmFrameHeader readHeader() const {
        ShmFrameHeader hdr{};
        if (m_valid && m_fd >= 0 &&
            ::pread(m_fd, &hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr)) {
            return hdr;
        }
        return hdr;
    }
    uint64_t writeAttempts() const { return readHeader().writeAttempts; }
    uint64_t writeSuccesses() const { return readHeader().writeSuccesses; }
    uint64_t acquireNulls() const { return readHeader().acquireNulls; }
    uint64_t writeFails() const { return readHeader().writeFails; }

    // 返回当前帧的裁剪几何信息（来自最近一次 readFrame 的 freshHeader）
    struct CropInfo {
        int size = 0;      // 裁剪边长
        int offX = 0;      // 左上角 x（全屏坐标）
        int offY = 0;      // 左上角 y（全屏坐标）
        int fullW = 0;     // 全屏宽
        int fullH = 0;     // 全屏高
        int rotation = 0;  // 设备当前旋转（0/2=竖屏，1/3=横屏）
    };
    CropInfo cropInfo() const {
        CropInfo c;
        const ShmFrameHeader* h = freshHeader();
        if (!h) return c;
        c.size = (int)h->cropSize;
        c.offX = (int)h->cropOffsetX;
        c.offY = (int)h->cropOffsetY;
        c.fullW = (int)h->width;
        c.fullH = (int)h->height;
        c.rotation = (int)h->rotation;
        return c;
    }

    // 请求 APK 切换裁剪边长（写入头部 cropRequest，APK 每 500ms 读取并应用）
    void requestCrop(int size) {
        if (!m_valid || m_fd < 0) return;
        ShmFrameHeader hdr;
        if (::pread(m_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return;
        hdr.cropRequest = (uint32_t)size;
        // 只覆写 cropRequest 字段（offset 44，4 字节），避免与写帧竞争
        ::pwrite(m_fd, &hdr.cropRequest, sizeof(hdr.cropRequest), 44);
    }

    // 读取统计（供面板诊断“帧源/读取是否正常”）
    uint64_t readOkCount() const { return m_readOk; }
    uint64_t noNewCount() const { return m_noNew; }
    uint64_t headerFailCount() const { return m_headerFail; }
    uint64_t dataFailCount() const { return m_dataFail; }

    // 简短诊断字符串（面板显示用）
    std::string diag() const {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "seq=%u ok=%llu nonew=%llu hdrFail=%llu dataFail=%llu "
                 "| APK写帧: att=%llu ok=%llu null=%llu fail=%llu",
                 readSeq(),
                 (unsigned long long)m_readOk, (unsigned long long)m_noNew,
                 (unsigned long long)m_headerFail, (unsigned long long)m_dataFail,
                 (unsigned long long)writeAttempts(), (unsigned long long)writeSuccesses(),
                 (unsigned long long)acquireNulls(), (unsigned long long)writeFails());
        return buf;
    }

    static ShmFrameReader* open(const char* filePath, int waitMs = 10000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        int fd = -1;

        // 1) 等文件出现
        while (true) {
            fd = ::open(filePath, O_RDWR);
            if (fd >= 0) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                fprintf(stderr, "ShmFrameReader: '%s' not ready within %d ms\n", filePath, waitMs);
                return nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        auto* reader = new ShmFrameReader();
        reader->m_fd = fd;

        // 2) 读头信息；APK 可能在写头部时被读到半截（撕裂头），
        //    对 bad magic / 非法字段做重试，而不是直接放弃
        ShmFrameHeader hdr{};
        bool hdrOk = false;
        while (true) {
            bool readOk = pread(fd, &hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr);
            if (readOk && hdr.magic == SHM_MAGIC &&
                hdr.width > 0 && hdr.height > 0 &&
                hdr.sizePerFrame > 0 && hdr.sizePerFrame <= 64 * 1024 * 1024) {
                hdrOk = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                fprintf(stderr, "ShmFrameReader: bad/torn header in '%s' (magic=0x%x)\n",
                        filePath, hdr.magic);
                delete reader;
                return nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!hdrOk) { delete reader; return nullptr; }

        int totalFrameSize = hdr.sizePerFrame * SHM_BUFFER_COUNT;
        reader->m_totalSize = SHM_HEADER_SIZE + totalFrameSize;

        // 确保文件足够大
        struct stat st;
        if (fstat(fd, &st) == 0 && (size_t)st.st_size < (size_t)reader->m_totalSize) {
            ftruncate(fd, reader->m_totalSize);
        }

        reader->m_mapped = mmap(nullptr, reader->m_totalSize, PROT_READ,
                                MAP_SHARED, fd, 0);
        if (reader->m_mapped == MAP_FAILED) {
            fprintf(stderr, "ShmFrameReader: mmap failed: %s\n", strerror(errno));
            delete reader;
            return nullptr;
        }

        reader->m_hdr = (ShmFrameHeader*)reader->m_mapped;
        reader->m_frameBase = (uint8_t*)reader->m_mapped + SHM_HEADER_SIZE;
        reader->m_valid = true;

        printf("ShmFrameReader: opened %s (%dx%d sizePerFrame=%u)\n", filePath,
               hdr.width, hdr.height, hdr.sizePerFrame);
        return reader;
    }

private:
    ShmFrameReader() = default;
    ShmFrameReader(const ShmFrameReader&) = delete;
    ShmFrameReader& operator=(const ShmFrameReader&) = delete;

    int m_fd = -1;
    void* m_mapped = nullptr;
    bool m_valid = false;
    uint32_t m_lastSeq = 0;
    uint64_t m_readOk = 0;      // 成功读到的帧数
    uint64_t m_noNew = 0;       // 无新帧（序号未变）次数
    uint64_t m_headerFail = 0;  // 头部读取/校验失败次数
    uint64_t m_dataFail = 0;    // 帧数据读取失败次数
    int m_totalSize = 0;
    ShmFrameHeader* m_hdr = nullptr;
    uint8_t* m_frameBase = nullptr;
    ShmFrameHeader m_freshHdr{};
    bool m_freshValid = false;
    std::vector<uint8_t> m_stage;   // 帧数据暂存缓冲（pread 读取）
};
