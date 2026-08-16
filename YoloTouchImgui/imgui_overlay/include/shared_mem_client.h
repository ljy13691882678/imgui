#pragma once

#include "aim_types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
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

    // 获取最新帧数据，返回指向帧数据的指针，通过 outSize 返回帧大小
    // 如果无新帧返回 nullptr
    const uint8_t* readFrame() {
        if (!m_valid) return nullptr;
        uint32_t seq = m_hdr->lastSeq;
        int bufIdx = (seq % SHM_BUFFER_COUNT) * m_hdr->sizePerFrame;
        const uint8_t* frameData = m_frameBase + bufIdx;

        // 检查序号是否变化
        if (seq == m_lastSeq) return nullptr;
        m_lastSeq = seq;
        m_frameCount++;
        return frameData;
    }

    uint64_t frameCount() const { return m_frameCount; }

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
};