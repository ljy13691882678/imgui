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
// 通过普通文件 mmap 读取 APK 写入的帧数据
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

    // 获取最新帧数据，返回指向帧数据的指针，如果无新帧返回 nullptr。
    // 头部与帧数据都改用 pread 直接读取文件（绕过 mmap TLB 缓存一致性问题），
    // 确保看到另一进程通过 write() 写入的最新序号与数据。
    const uint8_t* readFrame() {
        if (!m_valid) return nullptr;

        // 用 pread 从文件读取最新头（不用 mmap），保证跨进程可见性
        ShmFrameHeader freshHdr;
        if (::pread(m_fd, &freshHdr, sizeof(freshHdr), 0) != (ssize_t)sizeof(freshHdr))
            return nullptr;
        if (freshHdr.magic != SHM_MAGIC) return nullptr;

        uint32_t seq = freshHdr.lastSeq;
        if (seq == m_lastSeq) return nullptr;
        m_lastSeq = seq;
        m_freshHdr = freshHdr;
        m_freshValid = true;

        int sizePerFrame = (int)freshHdr.sizePerFrame;
        if (sizePerFrame <= 0 || sizePerFrame > 64 * 1024 * 1024) return nullptr;
        // 帧尺寸若变化（如 rowStride 对齐），自动扩展文件，避免拒绝所有帧
        if (SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT > m_totalSize) {
            ::ftruncate(m_fd, SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT);
            m_totalSize = SHM_HEADER_SIZE + sizePerFrame * SHM_BUFFER_COUNT;
        }
        int bufIdx = (seq % SHM_BUFFER_COUNT) * sizePerFrame;

        // 帧数据也走 pread 到暂存缓冲，避免 mmap 读到另一进程刚 write() 的脏页
        if (m_stage.size() < (size_t)sizePerFrame) m_stage.resize(sizePerFrame);
        if (::pread(m_fd, m_stage.data(), sizePerFrame, SHM_HEADER_SIZE + bufIdx)
            != (ssize_t)sizePerFrame)
            return nullptr;

        m_frameCount++;
        return m_stage.data();
    }

    uint64_t frameCount() const { return m_frameCount; }

    // 读取文件当前的帧序号（用 pread 绕过 mmap 缓存一致性问题）。
    // 若 APK 持续写帧，该值会不断增大；用于面板诊断“帧源是否在产帧”。
    uint32_t readSeq() const {
        if (!m_valid || m_fd < 0) return 0;
        ShmFrameHeader hdr;
        if (::pread(m_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return 0;
        if (hdr.magic != SHM_MAGIC) return 0;
        return hdr.lastSeq;
    }

    static ShmFrameReader* open(const char* filePath, int waitMs = 10000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        int fd = -1;

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

        // 读取头信息
        ShmFrameHeader hdr;
        if (pread(fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
            fprintf(stderr, "ShmFrameReader: failed to read header\n");
            delete reader;
            return nullptr;
        }

        if (hdr.magic != SHM_MAGIC) {
            fprintf(stderr, "ShmFrameReader: bad magic 0x%x\n", hdr.magic);
            delete reader;
            return nullptr;
        }

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

        printf("ShmFrameReader: opened %s (%dx%d)\n", filePath,
               hdr.width, hdr.height);
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
    uint64_t m_frameCount = 0;
    int m_totalSize = 0;
    ShmFrameHeader* m_hdr = nullptr;
    uint8_t* m_frameBase = nullptr;
    ShmFrameHeader m_freshHdr{};
    bool m_freshValid = false;
    std::vector<uint8_t> m_stage;   // 帧数据暂存缓冲（pread 读取）
};