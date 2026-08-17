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

// 自瞄控制器输出（归一化增量，相对屏幕；deltaX/deltaY 为屏幕占比）
struct AimOutput {
    bool   active = false;
    float  deltaX = 0.0f;   // 归一化移动量（相对屏幕）
    float  deltaY = 0.0f;
    float  targetX = 0.0f;
    float  targetY = 0.0f;
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
    int   cropIndex = 6;           // 默认 416（CROP_OPTIONS[6]）
    // 检测框描边粗细（像素）
    int   boxThickness = 2;
    bool  showBoxLabels = true;
    // 检测框跟随模式：true=直接跟随检测位置（无预测，响应最快）；false=速度预测跟随（更平滑但有延迟）
    bool  boxDirectFollow = true;
    // 检测框显示预判（秒）：仅在 boxDirectFollow=false 时生效
    float boxPredictTime = 0.05f;
    // 在悬浮窗上绘制裁剪区域边框（让用户看到推理输入范围）
    bool  showCropBox = true;
    // 推理帧率上限（FPS，0=不限制；可选 60/90/120/144）
    int   fpsLimit = 0;

    // 自瞄参数
    bool  aimEnabled = true;
    float deadZone = 0.02f;        // 归一化死区（中心距离小于此不移动）
    float smoothX = 0.5f;          // 平滑系数 0~1
    float smoothY = 0.5f;
    float aimSpeed = 1.0f;         // 速度增益
    float predictGain = 0.05f;     // 预判增益（秒）
    int   aimLockTimeMs = 300;     // 锁定时间
    // 自瞄锁定部位：0=中心 1=头部 2=身体（按目标框计算瞄准点，头部/身体按类别名 head/body 识别）
    int   aimPart = 0;
    // 自瞄瞄准点微调（归一化，相对屏幕）：在锁定部位计算出的瞄准点上再叠加偏移，
    // 用于微调锁点位置（如锁头时略向下，避免顶到头顶 / 打偏）
    float aimOffsetX = 0.0f;       // X 偏移（正=右）
    float aimOffsetY = 0.0f;       // Y 偏移（正=下）
    // 瞄准点时间平滑（EMA，0~1）：同一跟踪目标对瞄准点做指数移动平均，
    // 压掉检测框抖动/拖视角反馈振荡（越大越平滑，0=关闭）
    float aimPointSmooth = 0.75f;
    // 自瞄算法：0=PID 1=贝塞尔
    int   aimMode = 0;
    // PID 参数（executeAimingPid 移植）
    float pidKp = 0.30f;          // 比例系数
    float pidKi = 0.02f;          // 积分系数
    float pidKd = 0.08f;          // 微分系数
    float pidSamplePeriodMs = 8.0f; // PID 采样周期（固定值，ms）
    // 贝塞尔参数（executeAimingBezier 移植，slow-fast-slow smoothstep 计时）
    float bezierDuration = 30.0f; // 贝塞尔移动时长基准（ms 系数，最终随距离调整）
    // 收敛阈值（像素）：|误差| 小于该值认为已对准，停止拖拽（PID/贝塞尔共用）
    float convergeThresh = 10.0f;
    // 输出移动平滑（PID/贝塞尔共用，0~0.95，越大越平滑）
    float aimMoveSmooth = 0.35f;
    // 自瞄类别锁定：-1=所有启用类别；>=0=仅锁定该类（模型类别索引，面板按类别名选择）
    int   aimClass = -1;

    // 触控区（自瞄拖拽注入区域，归一化 [0,1]）：虚拟手指只在该区域内拖动视角。
    // 游戏通常只在特定区域响应拖屏转向，此区域应与游戏的转向/瞄准区域对齐。
    float touchZoneL = 0.40f, touchZoneT = 0.10f, touchZoneR = 1.0f, touchZoneB = 0.90f;
    // 触发区（玩家物理手指在此区域内时，扳机暂停自动开火，避免与手动开火冲突）
    float fireZoneL = 0.65f, fireZoneT = 0.65f, fireZoneR = 1.0f, fireZoneB = 1.0f;
    // 在悬浮窗上可视化触控区/触发区当前位置（半透明矩形 + 文字，调节时可直观看到）
    bool  showZones = true;

    // 从屏幕中上方向推理框画辅助连线（便于观察目标位置与准星偏移）
    bool  showAimLines = true;

    // 自瞄回正速度（归一化每帧最大瞄准点移动距离）：限制从当前位置到目标位置的
    // 每帧移动量，防止目标在裁剪框边缘时准星一帧甩飞（0=不限制）
    float aimApproachSpeed = 0.0f; // 0=关闭限制

    // 自瞄触发区：需要物理手指在此区域内才触发自瞄，配合按钮开关
    bool  aimTriggerZoneEnabled = false; // 开启后需点击触发区才自瞄
    float aimTriggerZoneL = 0.0f, aimTriggerZoneT = 0.0f;
    float aimTriggerZoneR = 0.5f, aimTriggerZoneB = 1.0f;
    // 倍镜区（开镜区）：需要物理手指在此区域内才触发自瞄，配合按钮开关
    bool  adsZoneEnabled = false;
    float adsZoneL = 0.0f, adsZoneT = 0.0f;
    float adsZoneR = 0.5f, adsZoneB = 1.0f;

    // 区域编辑模式：在悬浮窗上直接拖拽控制点调整区域大小和位置
    // 0=关闭 1=触控区 2=触发区(扳机暂停/开火区) 3=自瞄触发区 4=倍镜区
    int   zoneEditTarget = 0;

    // 触发参数
    float triggerSensitivity = 0.5f;
    bool  triggerHold = false;
    int   triggerCooldownMs = 150; // 点射模式两次开火最小间隔（毫秒）
    int   triggerDelayMin = 50;    // 扳机随机延迟最小值（ms）
    int   triggerDelayMax = 150;   // 扳机随机延迟最大值（ms）

    // 压枪（独立功能：检测物理手指按在“开枪区”时视角自动下拉补偿后坐力，不依赖扳机）
    bool  recoilEnabled = false;   // 压枪总开关
    int   recoilStartMs = 250;     // 压枪开始时间：开枪键按住后延迟多久开始下拉（ms）
    int   recoilStrength = 300;    // 压枪力度(uinput模式)：下拉速度（px/s），0=关闭
    float recoilDegPerSec = 60.0f; // 压枪力度(陀螺仪模式)：下拉角速度（°/s）

    // 内核陀螺仪（TimeDriver）模式：勾选后自瞄转向/压枪走内核陀螺仪 hook（pitch/yaw 角度注入），
    // 屏蔽 uinput 触摸注入及初始化，扳机禁用；取消勾选回到纯 uinput 模式。
    bool  gyroAim = false;              // 内核陀螺仪模式开关
    float gyroSens = 0.01f;             // 陀螺仪灵敏度（屏幕像素增量 → 角度系数）
    float gyroMaxDeg = 15.0f;           // 自瞄单帧最大注入角度（°）
    bool  gyroInvertPitch = false;      // 反转 Pitch（上下）
    bool  gyroInvertYaw = false;        // 反转 Yaw（左右）

    // 内核触摸（TimeDriver 触摸注入，替代 uinput）
    // 勾选后自瞄/压枪/扳机的虚拟触摸走 TimeDriver 内核驱动注入，
    // 而非 uinput。物理触摸不受影响（Touch_Init 后立即 Touch_Disable）。
    bool  kernelTouch = false;

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
constexpr int      CROP_OPTIONS[] = {0, 1260, 1080, 960, 720, 620, 416, 320, 256};
constexpr int      CROP_DEFAULT = 416;
