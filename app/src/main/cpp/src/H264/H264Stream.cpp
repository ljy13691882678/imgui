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
#include <media/NdkImage.h>

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
                // 用官方 AImage 接口读取解码输出，正确处理 RGBA / YUV420 的 stride 与像素步长
                AImage *img = nullptr;
                if (AMediaCodec_getOutputImage(codec, outIdx, &img) == AMEDIA_OK && img) {
                    int32_t iw = 0, ih = 0;
                    AImage_getWidth(img, &iw);
                    AImage_getHeight(img, &ih);
                    if (iw > 0 && ih > 0) {
                        std::vector<uint8_t> rgba((size_t)iw * ih * 4);
                        if (ImageToRGBA(img, iw, ih, rgba.data())) {
                            {
                                std::lock_guard<std::mutex> lk(frameMutex_);
                                latest_ = std::move(rgba);
                                latestW_ = iw;
                                latestH_ = ih;
                            }
                            frameCount++;
                            if (firstFrame) {
                                firstFrame = false;
                                printf("[h264] first frame decoded %dx%d\n", iw, ih);
                            } else if ((frameCount % 60) == 0) {
                                printf("[h264] decoded %ld frames\n", frameCount);
                            }
                        }
                    }
                    AImage_delete(img);
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

// 用 AImage 平面访问接口把解码输出转换为 RGBA8。
// 通过 RowStride/PixelStride 正确处理实际的缓冲排列（含对齐 padding），
// 兼容 RGBA_8888 单平面与 YUV_420_888 三平面两种常见输出。
bool H264Stream::ImageToRGBA(AImage *img, int w, int h, uint8_t *dst) {
    int32_t fmt = AImage_getFormat(img);

    if (fmt == AIMAGE_FORMAT_RGBA_8888) {
        uint8_t *data = nullptr;
        int len = 0;
        if (AImage_getPlaneData(img, 0, &data, &len) != AMEDIA_OK || !data) return false;
        int32_t rowStride = AImage_getPlaneRowStride(img, 0);
        int32_t pixStride = AImage_getPlanePixelStride(img, 0);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = data + (size_t)y * rowStride;
            uint8_t *d = dst + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                d[x * 4 + 0] = src[x * pixStride + 0];
                d[x * 4 + 1] = src[x * pixStride + 1];
                d[x * 4 + 2] = src[x * pixStride + 2];
                d[x * 4 + 3] = 255;
            }
        }
        return true;
    }

    if (fmt == AIMAGE_FORMAT_YUV_420_888) {
        uint8_t *Y = nullptr, *U = nullptr, *V = nullptr;
        int ylen = 0, ulen = 0, vlen = 0;
        AImage_getPlaneData(img, 0, &Y, &ylen);
        AImage_getPlaneData(img, 1, &U, &ulen);
        AImage_getPlaneData(img, 2, &V, &vlen);
        if (!Y || !U || !V) return false;
        int32_t yRs = AImage_getPlaneRowStride(img, 0);
        int32_t uRs = AImage_getPlaneRowStride(img, 1);
        int32_t vRs = AImage_getPlaneRowStride(img, 2);
        int32_t uPs = AImage_getPlanePixelStride(img, 1);
        int32_t vPs = AImage_getPlanePixelStride(img, 2);
        for (int j = 0; j < h; j++) {
            const uint8_t *yp = Y + (size_t)j * yRs;
            const uint8_t *up = U + (size_t)(j / 2) * uRs;
            const uint8_t *vp = V + (size_t)(j / 2) * vRs;
            uint8_t *d = dst + (size_t)j * w * 4;
            for (int i = 0; i < w; i++) {
                int y = yp[i];
                int u = up[(i / 2) * uPs] - 128;
                int v = vp[(i / 2) * vPs] - 128;
                int r = (298 * y + 409 * v + 128) >> 8;
                int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
                int b = (298 * y + 516 * u + 128) >> 8;
                d[i * 4 + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
                d[i * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
                d[i * 4 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
                d[i * 4 + 3] = 255;
            }
        }
        return true;
    }

    return false;
}