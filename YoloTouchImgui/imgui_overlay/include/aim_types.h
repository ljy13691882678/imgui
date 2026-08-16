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

    // 居中裁剪尺寸索引（CROP_OPTIONS 数组下标）
    int   cropIndex = 1;           // 默认 416（CROP_OPTIONS[1]）
    // 检测框描边粗细（像素）
    int   boxThickness = 2;
    bool  showBoxLabels = true;
    // 在悬浮窗上绘制裁剪区域边框（让用户看到推理输入范围）
    bool  showCropBox = true;
    // 推理帧率上限（FPS，0=不限制；可选 60/90/120/144）
    int   fpsLimit = 0;

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
// 布局：12×uint32(48B) + 4×uint64(32B) = 80B
// 帧数据 = 屏幕中心裁剪出的正方形区域（居中准星区域），边长 = cropSize。
// width/height 保持全屏尺寸（供坐标回映射/绘制），sizePerFrame = cropSize^2*4。
#pragma pack(push, 1)
struct ShmFrameHeader {
    uint32_t magic;          // 0xA1B2C3D4
    uint32_t width;          // 全屏宽
    uint32_t height;         // 全屏高
    uint32_t rowStride;      // 裁剪后每行字节数 = cropSize * pixelStride
    uint32_t pixelStride;
    uint32_t rotation;
    uint32_t sizePerFrame;   // 裁剪后单帧字节数 = cropSize^2 * 4
    uint32_t lastSeq;        // 最近写入序号
    uint32_t cropSize;       // 当前实际裁剪边长
    uint32_t cropOffsetX;    // 裁剪区域左上角（全屏坐标）
    uint32_t cropOffsetY;
    uint32_t cropRequest;    // 面板请求的裁剪边长（C++ 写入，APK 读取后应用）
    // APK 侧写帧诊断计数（imgui 侧只读，用于判断“APK 是否在持续写帧”）
    uint64_t writeAttempts;  // writeFrame 被调用次数（已取到非空帧）
    uint64_t writeSuccesses; // 写帧成功次数
    uint64_t acquireNulls;   // acquireLatestImage 返回 null 次数
    uint64_t writeFails;     // 写帧异常次数
};
#pragma pack(pop)

constexpr uint32_t SHM_MAGIC = 0xA1B2C3D4u;
constexpr int      SHM_HEADER_SIZE = 80;
constexpr int      SHM_BUFFER_COUNT = 2;   // 双缓冲
// 可选的居中裁剪边长（面板下拉选择，均 ≥ 模型输入 256，避免放大失真）
constexpr int      CROP_OPTIONS[] = {620, 416, 320, 256};
constexpr int      CROP_DEFAULT = 416;
