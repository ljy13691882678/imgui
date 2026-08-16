#pragma once

#include <vector>
#include <mutex>
#include <cstdint>

// 检测目标（归一化坐标，相对屏幕）
struct AimTarget {
    float x1, y1, x2, y2;   // 归一化 [0,1]
    float score = 0.0f;
    int   classId = 0;
    float cx = 0.0f;        // 中心点（归一化）
    float cy = 0.0f;
    float vx = 0.0f;        // 速度（预测用）
    float vy = 0.0f;
    int   trackId = -1;     // 跟踪 ID
    int   lostCount = 0;    // 丢失帧数
};

// 自瞄配置
struct AimConfig {
    bool  enabled = true;
    bool  showBoxes = true;
    bool  showFps = true;
    bool  triggerEnabled = false;

    float confidence = 0.45f;
    float nmsIoU = 0.45f;

    // 自瞄参数
    bool  aimEnabled = true;
    float deadZone = 0.02f;        // 归一化死区（中心距离小于此不移动）
    float smoothX = 0.35f;         // 平滑系数 0~1
    float smoothY = 0.35f;
    float aimSpeed = 1.0f;         // 速度增益
    float predictGain = 0.05f;     // 预判增益（秒）
    int   aimLockTimeMs = 300;     // 锁定时间

    // 触发参数
    float triggerSensitivity = 0.5f;
    bool  triggerHold = false;

    // 目标选择
    int   selectMode = 0;          // 0=最近中心 1=最大框 2=最接近准星
};

// 共享内存帧头（与 APK 侧严格一致）
#pragma pack(push, 1)
struct ShmFrameHeader {
    uint32_t magic;          // 0xA1B2C3D4
    uint32_t width;
    uint32_t height;
    uint32_t rowStride;
    uint32_t pixelStride;
    uint32_t rotation;
    uint32_t sizePerFrame;   // 单帧字节数
    uint32_t lastSeq;        // 最近写入序号
    uint8_t  reserved[32];   // 头部共 64 字节
};
#pragma pack(pop)

constexpr uint32_t SHM_MAGIC = 0xA1B2C3D4u;
constexpr int      SHM_HEADER_SIZE = 64;
constexpr int      SHM_BUFFER_COUNT = 2;   // 双缓冲
