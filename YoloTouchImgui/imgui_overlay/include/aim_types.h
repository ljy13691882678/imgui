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
    int   cropIndex = 4;           // 默认 416（CROP_OPTIONS[4]）
    // 检测框描边粗细（像素）
    int   boxThickness = 2;
    bool  showBoxLabels = true;
    // 检测框显示预判（秒）：用目标速度把框前移，补偿“捕获→绘制”延迟，快速转动不掉框
    float boxPredictTime = 0.02f;
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
    // 自瞄锁定部位：0=中心 1=头部 2=身体（按目标框计算瞄准点，头部/身体按类别名 head/body 识别）
    int   aimPart = 0;
    // 自瞄拖拽灵敏度（拖动视角式）：归一化增量 × 屏幕 × dragSens → 每帧拖拽像素
    float dragSens = 0.5f;
    // 自瞄单帧最大拖拽像素（防止瞬移/抖动）
    int   aimMaxStepPx = 40;
    // 自瞄类别锁定：-1=所有启用类别；>=0=仅锁定该类（模型类别索引，面板按类别名选择）
    int   aimClass = -1;

    // 触控区（自瞄拖拽注入区域，归一化 [0,1]）：虚拟手指只在该区域内拖动视角。
    // 游戏通常只在特定区域响应拖屏转向，此区域应与游戏的转向/瞄准区域对齐。
    float touchZoneL = 0.40f, touchZoneT = 0.10f, touchZoneR = 1.0f, touchZoneB = 0.90f;
    // 触发区（玩家物理手指在此区域内时，扳机暂停自动开火，避免与手动开火冲突）
    float fireZoneL = 0.65f, fireZoneT = 0.65f, fireZoneR = 1.0f, fireZoneB = 1.0f;

    // 触发参数
    float triggerSensitivity = 0.5f;
    bool  triggerHold = false;
    int   triggerCooldownMs = 150; // 点射模式两次开火最小间隔（毫秒）

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
// 可选的居中裁剪边长（面板下拉选择，0=全屏不裁剪，其他 ≥ 模型输入 256，避免放大失真）
// 与 APK 侧 CaptureService.CROP_OPTIONS 严格一致
constexpr int      CROP_OPTIONS[] = {0, 960, 720, 620, 416, 320, 256};
constexpr int      CROP_DEFAULT = 416;
