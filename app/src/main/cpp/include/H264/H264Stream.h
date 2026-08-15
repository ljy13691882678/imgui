#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <string>

// H264 流式采集：screenrecord 输出 H264 视频流，经 AMediaCodec 硬件解码为 RGBA。
// 相比逐帧 screencap，帧率可提升到 30~60 FPS。
class H264Stream {
public:
    H264Stream();
    ~H264Stream();

    // 启动采集。width/height 为输出尺寸（screenrecord 会按该尺寸缩放全屏）。
    // bitrate 单位 bps。返回是否成功启动。
    bool Start(int width, int height, int bitrate);

    // 停止采集并回收资源
    void Stop();

    bool Running() const;

    // 取最新一帧 RGBA；有新帧返回 true 并拷贝到 rgba(大小为 w*h*4)。
    // 无新帧且仍在运行返回 false。
    bool GrabFrame(std::vector<uint8_t> &rgba, int &w, int &h);

    // 最近一次错误描述
    const char *LastError() const { return lastError_.c_str(); }

private:
    void DecodeLoop();                       // 解码线程主循环
    bool StartScreenRecord();                // 拉起 screenrecord 子进程
    void StopScreenRecord();
    int32_t CurrentOutputColorFormat(void *codec);   // 查询解码输出颜色格式
    static void ConvertOutput(const uint8_t *src, size_t bytes, int w, int h,
                              uint8_t *dst, int32_t fmt);

    // 内部状态
    volatile bool running_ = false;
    std::mutex    frameMutex_;
    std::vector<uint8_t> latest_;            // 最新 RGBA 帧
    int           latestW_ = 0;
    int           latestH_ = 0;
    std::string   lastError_;

    int           outW_ = 0;                 // 输出/解码尺寸
    int           outH_ = 0;
    int           bitrate_ = 0;

    // screenrecord 子进程
    int           procFd_ = -1;             // 读取 H264 码流的管道读端
    int           procPid_ = -1;            // screenrecord 子进程 pid
    std::string   procCmd_;                 // 便于重启

    // 解码器（仅解码线程访问）
    void         *codec_ = nullptr;          // AMediaCodec*
    bool          codecStarted_ = false;
};