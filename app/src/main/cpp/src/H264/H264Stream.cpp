#include "H264/H264Stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

// AMediaCodec 输出颜色格式常量（与 AMediaFormat 兼容）
#define IMG_FMT_RGBA8888       0x11   // COLOR_Format32bitRGBA8888
#define IMG_FMT_ARGB8888       0x10   // COLOR_Format32bitARGB8888
#define IMG_FMT_YUV420_PLANAR  0x13   // I420
#define IMG_FMT_YUV420_SEMI    0x15   // NV12
#define IMG_FMT_RGBA_FLEXIBLE  0x7F000001

// 一块解码缓冲的读取大小（H264 流从管道持续流入）
static const int kReadChunk = 65536;

H264Stream::H264Stream() = default;
H264Stream::~H264Stream() { Stop(); }

bool H264Stream::Running() const { return running_; }

bool H264Stream::Start(int width, int height, int bitrate) {
    if (running_) return true;
    outW_ = width;
    outH_ = height;
    bitrate_ = bitrate;
    lastError_.clear();

    if (!StartScreenRecord()) return false;

    running_ = true;
    std::thread([this] { DecodeLoop(); }).detach();
    printf("[h264] stream started %dx%d\n", outW_, outH_);
    return true;
}

void H264Stream::Stop() {
    if (running_) {
        running_ = false;
        StopScreenRecord();   // 结束子进程，管道读端返回 EOF，解码线程据此退出
    }
    // 短暂等待解码线程完成资源清理（codec 由解码线程自行释放）
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// 通过 root 拉起 screenrecord，输出 H264 原始码流到 stdout（文件名 "-"）
bool H264Stream::StartScreenRecord() {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "screenrecord --size %dx%d --bit-rate %d --output-format=h264 --time-limit 180 -",
             outW_, outH_, bitrate_);
    procCmd_ = cmd;

    int fds[2];
    if (pipe(fds) != 0) {
        lastError_ = "pipe() failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        lastError_ = "fork() failed";
        return false;
    }
    if (pid == 0) {
        // 子进程：把 stdout 接到管道写端后 exec screenrecord
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl("/system/bin/sh", "sh", "-c", procCmd_.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    procFd_ = fds[0];
    procPid_ = pid;
    return true;
}

void H264Stream::StopScreenRecord() {
    if (procPid_ > 0) {
        kill(procPid_, SIGTERM);   // 终止 screenrecord，让管道 EOF
        int st = 0;
        waitpid(procPid_, &st, 0);
        procPid_ = -1;
    }
    if (procFd_ >= 0) {
        close(procFd_);
        procFd_ = -1;
    }
}

bool H264Stream::GrabFrame(std::vector<uint8_t> &rgba, int &w, int &h) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (latest_.empty()) return false;
    rgba = latest_;
    w = latestW_;
    h = latestH_;
    return true;
}

// ---- 解码线程 ----
void H264Stream::DecodeLoop() {
    AMediaCodec *codec = nullptr;
    AMediaFormat *fmt = AMediaFormat_new();
    int64_t tsUs = 0;   // 自增时间戳（单位 us）
    bool codecStarted = false;
    long frameCount = 0;
    bool firstFrame = true;

    while (running_) {
        if (!codec) {
            codec = AMediaCodec_createDecoderByType("video/avc");
            if (!codec) {
                lastError_ = "createDecoderByType(avc) failed";
                break;
            }
            AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, outW_);
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, outH_);
            // 请求 RGBA 输出，避免额外颜色转换
            AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, IMG_FMT_RGBA_FLEXIBLE);
            media_status_t st = AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0);
            if (st != AMEDIA_OK) {
                lastError_ = "AMediaCodec_configure failed";
                AMediaCodec_delete(codec);
                codec = nullptr;
                break;
            }
            if (AMediaCodec_start(codec) != AMEDIA_OK) {
                lastError_ = "AMediaCodec_start failed";
                AMediaCodec_delete(codec);
                codec = nullptr;
                break;
            }
            codecStarted = true;
            codec_ = codec;
            codecStarted_ = true;
        }

        if (procFd_ < 0) {
            lastError_ = "screenrecord pipe closed";
            break;
        }

        // 读一小段 H264 送入解码器
        std::vector<uint8_t> chunk(kReadChunk);
        ssize_t n = read(procFd_, chunk.data(), chunk.size());
        if (n <= 0) {
            // 管道 EOF：screenrecord 已结束
            if (running_) {
                // 若已有帧可继续供，先退出采集（screenrecord 受 time-limit 限制需重启，
                // 此处简单起见直接结束，由上层重新 Start 即可）
                lastError_ = "screenrecord EOF";
            }
            break;
        }

        // 送入输入缓冲
        ssize_t inIdx = AMediaCodec_dequeueInputBuffer(codec, 10000);
        if (inIdx >= 0) {
            size_t sz = 0;
            uint8_t *buf = AMediaCodec_getInputBuffer(codec, inIdx, &sz);
            if (buf && sz > 0) {
                size_t copy = (size_t)n < sz ? (size_t)n : sz;
                memcpy(buf, chunk.data(), copy);
                AMediaCodec_queueInputBuffer(codec, inIdx, 0, copy, tsUs, 0);
                tsUs += 16000;   // ~60fps 采样间隔
            } else {
                // 缓冲不可用：以 0 长度入队跳过该缓冲
                AMediaCodec_queueInputBuffer(codec, inIdx, 0, 0, tsUs, 0);
            }
        }

        // 取输出
        AMediaCodecBufferInfo binfo;
        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec, &binfo, 0);
        if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
            AMediaFormat_delete(of);
            if (w > 0 && h > 0) { outW_ = w; outH_ = h; }
        } else if (outIdx >= 0) {
            if (binfo.size > 0) {
                size_t sz = 0;
                uint8_t *buf = AMediaCodec_getOutputBuffer(codec, outIdx, &sz);
                if (buf && sz >= (size_t)binfo.size) {
                    std::vector<uint8_t> rgba((size_t)outW_ * outH_ * 4);
                    int32_t fmt_out = CurrentOutputColorFormat(codec);
                    ConvertOutput(buf, (size_t)binfo.size, outW_, outH_, rgba.data(), fmt_out);
                    {
                        std::lock_guard<std::mutex> lk(frameMutex_);
                        latest_ = std::move(rgba);
                        latestW_ = outW_;
                        latestH_ = outH_;
                    }
                    frameCount++;
                    if (firstFrame) {
                        firstFrame = false;
                        printf("[h264] first frame decoded %dx%d\n", outW_, outH_);
                    } else if ((frameCount % 60) == 0) {
                        printf("[h264] decoded %ld frames\n", frameCount);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, outIdx, false);
        }
        // 有限睡眠，避免独占 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (codec) {
        if (codecStarted) AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        codec = nullptr;
        codecStarted = false;
    }
    AMediaFormat_delete(fmt);
    codec_ = nullptr;
    codecStarted_ = false;

    // 结束前清掉尚未消费的帧，标记停止
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        latest_.clear();
    }
    printf("[h264] decode loop exit, err=%s\n", lastError_.empty() ? "none" : lastError_.c_str());
    running_ = false;
}

int32_t H264Stream::CurrentOutputColorFormat(void *codec) {
    AMediaFormat *of = AMediaCodec_getOutputFormat((AMediaCodec *)codec);
    int32_t fmt = IMG_FMT_RGBA8888;
    AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &fmt);
    AMediaFormat_delete(of);
    return fmt;
}

// 将解码输出转换为 RGBA8。支持 RGBA/ARGB 与常见 YUV420 平面/半平面。
void H264Stream::ConvertOutput(const uint8_t *src, size_t bytes, int w, int h,
                               uint8_t *dst, int32_t fmt) {
    const size_t need = (size_t)w * h * 4;
    if (bytes < need) return;

    switch (fmt) {
        case IMG_FMT_RGBA_FLEXIBLE:
        case IMG_FMT_RGBA8888: {
            memcpy(dst, src, need);
            return;
        }
        case IMG_FMT_ARGB8888: {
            for (int i = 0; i < w * h; i++) {
                dst[i * 4 + 0] = src[i * 4 + 1];
                dst[i * 4 + 1] = src[i * 4 + 2];
                dst[i * 4 + 2] = src[i * 4 + 3];
                dst[i * 4 + 3] = 255;
            }
            return;
        }
        case IMG_FMT_YUV420_PLANAR: { // I420: YYYY UU VV
            const uint8_t *Y = src;
            const uint8_t *U = src + (size_t)w * h;
            const uint8_t *V = U + ((size_t)w * h) / 4;
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    int yi = j * w + i;
                    int uv = (j / 2) * (w / 2) + (i / 2);
                    int y = Y[yi], u = U[uv] - 128, v = V[uv] - 128;
                    int r = (298 * y + 409 * v + 128) >> 8;
                    int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
                    int b = (298 * y + 516 * u + 128) >> 8;
                    dst[yi * 4 + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
                    dst[yi * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
                    dst[yi * 4 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
                    dst[yi * 4 + 3] = 255;
                }
            }
            return;
        }
        case IMG_FMT_YUV420_SEMI: { // NV12: YYYY UVUV
            const uint8_t *Y = src;
            const uint8_t *UV = src + (size_t)w * h;
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < w; i++) {
                    int yi = j * w + i;
                    int uv = (j / 2) * w + (i & ~1);
                    int y = Y[yi], u = UV[uv] - 128, v = UV[uv + 1] - 128;
                    int r = (298 * y + 409 * v + 128) >> 8;
                    int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
                    int b = (298 * y + 516 * u + 128) >> 8;
                    dst[yi * 4 + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
                    dst[yi * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
                    dst[yi * 4 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
                    dst[yi * 4 + 3] = 255;
                }
            }
            return;
        }
        default: {
            memset(dst, 90, need);   // 未知格式置灰，避免崩溃
            return;
        }
    }
}