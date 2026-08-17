#include "Android_draw/draw.h"

#include "aim_types.h"
#include "kalman_tracker.h"
#include "aim_controller.h"
#include "aim_modes.h"
#include "trigger_controller.h"
#include "shared_mem_client.h"
#include "inference/inference_engine.h"
#include "inference/litert_engine.h"
#include "injection/touch_core.h"
#include "injection/time_driver_wrap.h"
#include "auth/t3auth.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <dirent.h>
#include <unistd.h>
#include <sys/time.h>

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
bool main_thread_flag = true;
int abs_ScreenX = 0;
int abs_ScreenY = 0;

// 当前推理引擎（main 创建，推理线程只读调用）
static InferenceEngine* g_engine = nullptr;
static std::atomic<bool> g_engineReady{false};
// 引擎读写锁：推理线程读锁调用 detect，模型切换写锁重建引擎
static std::shared_mutex g_engineMutex;

// 可用模型列表（从模型目录扫描）
static std::vector<std::string> g_modelList;
static int g_modelIndex = 0;

// 录屏帧尺寸（从共享内存头读取，供绘制坐标缩放）
static int g_shmWidth = 0;
static int g_shmHeight = 0;
static int g_rotation = 0;

// 控制配置
static AimConfig g_cfg;

// 触摸注入
static bool g_touchReady = false;
// uinput 注入设备就绪（touch_inject_ready 且设备扫描成功）时才可做触摸注入。
// 陀螺仪模式下 uinput 设备【保持初始化】供屏幕触控，仅自瞄/压枪注入走内核陀螺仪。
static bool injectReady() { return g_touchReady && touch_inject_ready(); }

// 内核触摸模式：TimeDriver 已连接且勾选了内核触摸
static bool kernelTouchMode() {
    return g_cfg.kernelTouch && kdrv_connected();
}

// 统一触摸注入：根据当前模式选择 uinput 或内核驱动
static bool canInjectTouch() {
    return injectReady() || kernelTouchMode();
}

static void inject_down(int slot, int id, int x, int y) {
    if (kernelTouchMode()) kdrv_touch_down(id, x, y);
    else touch_down(slot, id, x, y);
}
static void inject_move(int slot, int x, int y) {
    if (kernelTouchMode()) kdrv_touch_move(slot, x, y);
    else touch_move(slot, x, y);
}
static void inject_up(int slot) {
    if (kernelTouchMode()) kdrv_touch_up(slot);
    else touch_up(slot);
}

// 共享内存
static ShmFrameReader* g_shm = nullptr;
static std::atomic<bool> g_shmReady{false};

// 推理结果（跨线程）
static std::mutex g_detMutex;
static std::vector<Detection> g_detections;
static std::vector<AimTarget> g_tracks;
static std::atomic<uint64_t> g_inferFps{0};
static std::atomic<uint64_t> g_srcFps{0};      // APK 帧源写帧速率（帧/s），区分“帧源到顶”和“推理慢”
static std::atomic<uint64_t> g_frameCount{0};
// 最近一帧处理耗时（ms，含推理），供面板/日志诊断 QNN 是否过慢
static std::atomic<long long> g_lastFrameMs{0};
// 最近一次 processFrame 完成时刻（ms），用于主循环诊断推理是否卡死
static std::atomic<long long> g_lastFrameDoneMs{0};
// 最近一帧推理完成的时间戳（ms），用于绘制时精确补偿推理延迟
static std::atomic<long long> g_lastFrameTimestamp{0};
// 平滑后的推理延迟（ms，整数，用 atomic<int> 保证跨线程安全）
// 用于 UI 显示和预测补偿
static std::atomic<int> g_inferLatencyMs{0};

static bool g_aimActive = false;
static float g_aimX = 0.5f, g_aimY = 0.5f;

// 陀螺仪平滑注入：推理线程只计算目标，主线程负责平滑插值注入
// 这样即使推理帧率低（如50ms/帧），陀螺仪注入仍然平滑（1ms/帧插值）
static std::mutex g_gyroMutex;
static float g_gyroTargetPitch = 0.0f;   // 推理线程写入的目标 pitch
static float g_gyroTargetYaw   = 0.0f;   // 推理线程写入的目标 yaw
static float g_gyroSmoothPitch = 0.0f;   // 主线程平滑后的实际注入 pitch
static float g_gyroSmoothYaw   = 0.0f;   // 主线程平滑后的实际注入 yaw
static float g_gyroLastTargetPitch = 0.0f; // 上一帧目标 pitch（用于变化率限制）
static float g_gyroLastTargetYaw   = 0.0f; // 上一帧目标 yaw（用于变化率限制）
static bool  g_gyroTargetActive = false;  // 推理线程是否有有效目标
static bool  g_gyroTargetDirReversed = false; // 目标方向反转标记（主线程快速收敛）

// T3 卡密验证状态（防破解：验证通过后才启用推理/自瞄/注入）
static std::atomic<bool> g_t3Verified{false};   // 是否已通过卡密验证
static std::atomic<bool> g_t3LoggingIn{false};  // 正在登录（避免重复提交）
static std::string       g_t3Card;              // 验证通过的卡密
static std::string       g_t3Statecode;         // 心跳需要的 statecode
static std::mutex        g_t3MsgMutex;          // 保护 g_t3Message
static std::string       g_t3Message;           // 登录状态提示（成功/失败原因）
static char              g_t3InputBuf[128] = {0}; // 悬浮窗输入框内容

// 配置文件路径（工作目录下）+ 上次保存副本（用于自动保存差异检测）
static std::string g_cfgFile = "yolotouch_cfg.bin";
static AimConfig g_cfgLastSaved;

// 保存配置到文件（魔数 + 版本 + 结构体二进制）
static void saveConfig() {
    FILE* f = fopen(g_cfgFile.c_str(), "wb");
    if (!f) return;
    const char magic[4] = {'Y', 'T', 'C', 'T'};
    uint32_t version = 1;
    fwrite(magic, 1, 4, f);
    fwrite(&version, 4, 1, f);
    fwrite(&g_cfg, 1, sizeof(g_cfg), f);
    fclose(f);
}

// 从文件加载配置（版本/大小不符时静默忽略，保留默认值）
static bool loadConfig() {
    FILE* f = fopen(g_cfgFile.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {0};
    uint32_t version = 0;
    bool ok = false;
    if (fread(magic, 1, 4, f) == 4 && fread(&version, 4, 1, f) == 1 &&
        memcmp(magic, "YTCT", 4) == 0 && version == 1) {
        AimConfig cfg;
        if (fread(&cfg, 1, sizeof(cfg), f) == sizeof(cfg)) {
            g_cfg = cfg;
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

// 类别显示过滤：classEnabled[i]==false 时该类别的框不显示、不参与自瞄/扳机
static std::vector<bool> g_classEnabled;

// 根据当前引擎的类别数同步 g_classEnabled（引擎初始化/切换模型后调用）
static void syncClassConfig() {
    int n = g_engine ? g_engine->getNumClasses() : 0;
    if (n > 0 && (int)g_classEnabled.size() != n)
        g_classEnabled.assign((size_t)n, true);
    // 类别锁定索引越界（切换模型后类别数变化）时回退到"全部"
    if (n > 0 && g_cfg.aimClass >= n) g_cfg.aimClass = -1;
}

// 控制面板折叠：true 时只显示一个小状态框
static bool g_panelCollapsed = false;

// 区域编辑拖拽状态
static struct {
    bool dragging = false;
    int dragPoint = -1; // -1=移动整个区域, 0-7=控制点(0-3角,4-7边中点)
    float dragStartMX = 0.0f, dragStartMY = 0.0f;
    float origL = 0.0f, origT = 0.0f, origR = 0.0f, origB = 0.0f;
} g_zoneDrag;

// 自瞄/触发控制器
static KalmanTracker g_tracker;
static AimController g_aim;        // 原版（拖拽+平滑）
static PidAimController g_pidAim;  // PID（移植自 YoloTouchHelp）
static BezierAimController g_bezierAim; // 贝塞尔（移植自 YoloTouchHelp）
static TriggerController g_trigger;

// 自瞄虚拟手指状态（拖动视角式：手指保持按下，逐帧按增量移动，目标进入死区时抬起）
static bool   g_aimFingerDown = false;
static float  g_aimFingerX = 0.0f, g_aimFingerY = 0.0f;
// 扳机虚拟手指状态（按住模式需保持按下，并在目标离开/停用时抬起）
static bool   g_triggerDown = false;
// 压枪状态：物理手指按在开枪区时计时 + 下拉补偿
static bool      g_recoilFiring = false;   // 开枪键按住中（用于开始时间计时）
static long long g_recoilStartMs = 0;      // 本次按住开枪键的起始时刻

// 瞄准点时间平滑（EMA）：抑制检测框抖动传导，尤其锁头部/身体时 Y 轴上下甩
static float  g_aimSmoothX = -1.0f, g_aimSmoothY = -1.0f;
static int    g_aimSmoothTrack = -1;

// 根据锁定部位计算瞄准点（归一化坐标）。
// part: 0=中心 1=头部 2=身体。头部/身体按模型类别名 head/body 识别：
//   - 锁头部：head 类框直接用框中心，全身框用上部 12% 高度处
//   - 锁身体：body 类框用框中心，其他框用中上部（胸口）45% 高度处
static void computeAimPoint(const AimTarget& t, int part, float* ax, float* ay) {
    float x = t.cx;
    float y = t.cy;
    float h = t.y2 - t.y1;
    if (part == 1 || part == 2) {
        const char* cn = g_engine ? g_engine->getClassName(t.classId) : nullptr;
        bool isHead = cn && (strstr(cn, "head") || strstr(cn, "Head"));
        bool isBody = cn && (strstr(cn, "body") || strstr(cn, "Body"));
        if (part == 1) {
            y = t.y1 + h * (isHead ? 0.5f : 0.12f);
        } else {
            y = t.y1 + h * (isBody ? 0.5f : 0.45f);
        }
    }
    *ax = x;
    *ay = y;
}

// 释放自瞄/扳机的虚拟手指（目标丢失/功能停用时调用，避免手指卡在屏幕上）
static void releaseAimFingers() {
    if (g_aimFingerDown) {
        if (g_touchReady || kernelTouchMode()) inject_up(TOUCH_VIRTUAL_SLOT);
        g_aimFingerDown = false;
    }
    if (g_triggerDown) {
        if (g_touchReady || kernelTouchMode()) inject_up(TOUCH_TRIGGER_SLOT);
        g_triggerDown = false;
    }
    // 陀螺仪模式下也停止注入，避免目标丢失/功能停用后视角仍被持续拖拽
    if (g_cfg.gyroAim) {
        std::lock_guard<std::mutex> lock(g_gyroMutex);
        g_gyroTargetActive = false;
        g_gyroSmoothPitch = 0.0f;
        g_gyroSmoothYaw   = 0.0f;
        g_gyroLastTargetPitch = 0.0f;
        g_gyroLastTargetYaw   = 0.0f;
        g_gyroTargetDirReversed = false;
        touch_gyro_stop();
    }
}

// 切换内核陀螺仪模式：
// 勾选 → 屏蔽 uinput（销毁注入设备，不初始化），连接驱动并初始化陀螺仪 hook；
// 取消 → 关闭陀螺仪 hook 并恢复 uinput 注入。
// 注意：陀螺仪模式下仅自瞄/压枪由陀螺仪注入，uinput 设备【保持初始化】，
// 这样游戏屏幕仍可被注入触摸（仅 uinput 未初始化才会导致屏幕不可触控）。
static void applyGyroMode(bool enabled) {
    if (enabled) {
        bool gyroOk = touch_kernel_gyro_init();
        if (gyroOk && touch_inject_ready()) {
            releaseAimFingers();
        }
        printf("applyGyroMode(true) driver_connected=%d inject_ready=%d\n",
               (int)touch_kernel_connected(), (int)touch_inject_ready());
    } else {
        touch_gyro_stop();
        touch_gyro_disable();
        if (!touch_inject_ready()) touch_inject_init();
    }
}

// 切换内核触摸模式：
// 勾选 → 初始化 TimeDriver 内核触摸设备（Touch_Init + Touch_Disable 不阻断物理触摸）
// 取消 → 清理内核触摸设备
// 内核触摸模式与 uinput 模式互斥：勾选后注入走 TimeDriver，不再走 uinput
static void applyKernelTouchMode(bool enabled) {
    if (enabled) {
        if (!kdrv_connected()) kdrv_init();
        if (kdrv_connected()) {
            int w = g_shm ? (int)g_shm->header()->width : 1080;
            int h = g_shm ? (int)g_shm->header()->height : 2400;
            int orient = g_rotation;
            bool ok = kdrv_touch_init(w, h, orient);
            printf("applyKernelTouchMode(true) driver_connected=%d touch_inited=%d\n",
                   (int)kdrv_connected(), (int)ok);
        } else {
            printf("applyKernelTouchMode(true) FAILED: driver not connected\n");
        }
    } else {
        kdrv_touch_up(TOUCH_VIRTUAL_SLOT);
        kdrv_touch_up(TOUCH_TRIGGER_SLOT);
        kdrv_touch_cleanup();
        printf("applyKernelTouchMode(false) cleanup done\n");
    }
}

// 检查物理手指是否按下并位于指定区域（归一化坐标）。无手指/未按下返回 false。
// 用于自瞄触发区/倍镜区门控：只有手指点在对应区域内才允许自瞄。
// 检查是否有任何手指在指定区域内（支持多点触控）
static bool isFingerInZone(float zL, float zT, float zR, float zB,
                           int scrW, int scrH) {
    if (!g_touchReady) return false;
    int l = (int)(zL * scrW);
    int t = (int)(zT * scrH);
    int r = (int)(zR * scrW);
    int b = (int)(zB * scrH);
    // 遍历所有手指，只要有一个在区域内就返回 true
    return touch_is_any_finger_in_zone(l, t, r, b);
}

// ---------------------------------------------------------------------------
// 时间工具
// ---------------------------------------------------------------------------
static long long getTimeNowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static long long getTimeNowUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ---------------------------------------------------------------------------
// 模型管理
// ---------------------------------------------------------------------------
// 扫描目录下所有 .tflite 模型
static void scanModels(const char* dir) {
    g_modelList.clear();
    if (!dir || !dir[0]) return;
    DIR* d = opendir(dir);
    if (!d) {
        printf("model dir not found: %s\n", dir);
        return;
    }
    std::vector<std::string> names;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() > 7 && n.compare(n.size() - 7, 7, ".tflite") == 0)
            names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) g_modelList.push_back(std::string(dir) + "/" + n);
    printf("found %zu model(s) in %s\n", g_modelList.size(), dir);
}

// 重新加载指定索引的模型（写锁重建引擎）
static bool loadModel(int idx) {
    if (idx < 0 || (size_t)idx >= g_modelList.size()) return false;
    std::unique_lock<std::shared_mutex> lock(g_engineMutex);
    if (!g_engine) return false;
    bool ok = g_engine->init(g_modelList[idx].c_str());
    if (ok) {
        g_engine->setConfidence(g_cfg.confidence);
        syncClassConfig();
        g_modelIndex = idx;
        printf("switched model -> %s (backend=%s)\n",
               g_modelList[idx].c_str(), g_engine->getBackendType().c_str());
    } else {
        fprintf(stderr, "load model failed: %s\n", g_modelList[idx].c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 单帧处理：推理 + 跟踪 + 自瞄 + 触发 + 注入
// ---------------------------------------------------------------------------
static void processFrame(const uint8_t* frame, const ShmFrameHeader* h) {
    const int w = (int)h->width;      // 全屏宽
    const int hh = (int)h->height;    // 全屏高
    if (w <= 0 || hh <= 0) return;
    if (!g_engine) return;

    // 共享内存里的帧是居中裁剪出的准星区域（cropSize×cropSize）。
    // detect 语义：src 指向裁剪区域，region=裁剪区域，offset=裁剪区域在全屏中的
    // 左上角，screen=全屏。这样检测框坐标会回映射到全屏归一化坐标。
    int cropSize = (int)h->cropSize;
    int cropOffX = (int)h->cropOffsetX;
    int cropOffY = (int)h->cropOffsetY;
    int regionW = cropSize, regionH = cropSize;
    if (cropSize <= 0) {
        // 全屏模式：共享内存帧为整屏（rowStride 为真实 stride），推理区域=整屏
        cropOffX = 0;
        cropOffY = 0;
        regionW = w;
        regionH = hh;
    } else if (cropSize > w || cropSize > hh) {
        // 兜底：尺寸非法时退化为最大居中正方形
        cropSize = std::min(w, hh);
        regionW = cropSize;
        regionH = cropSize;
    }

    // 读锁保护引擎（切换模型时会取写锁等待）
    std::shared_lock<std::shared_mutex> engLock(g_engineMutex);
    if (!g_engine) return;

    auto dets = g_engine->detect(
        const_cast<uint8_t*>(frame), cropOffX, cropOffY, regionW, regionH,
        w, hh,
        (int)h->rowStride, (int)h->pixelStride);

    // 类别过滤：仅保留启用显示的类别（同时影响自瞄/扳机目标选择）
    if (!g_classEnabled.empty()) {
        std::vector<Detection> filtered;
        filtered.reserve(dets.size());
        for (const auto& d : dets) {
            int cid = (int)d.classId;
            if (cid >= 0 && cid < (int)g_classEnabled.size() && g_classEnabled[cid])
                filtered.push_back(d);
        }
        dets = std::move(filtered);
    }

    // 更新检测结果（供绘制）
    {
        std::lock_guard<std::mutex> lock(g_detMutex);
        g_detections = dets;
    }
    g_frameCount++;

    if (!g_cfg.enabled) return;

    // 转为 AimTarget 并跟踪
    long long detectTimeUs = getTimeNowUs();
    std::vector<AimTarget> targets;
    for (const auto& d : dets) {
        AimTarget t;
        t.x1 = d.x1; t.y1 = d.y1; t.x2 = d.x2; t.y2 = d.y2;
        t.score = d.score; t.classId = (int)d.classId;
        t.cx = (d.x1 + d.x2) * 0.5f;
        t.cy = (d.y1 + d.y2) * 0.5f;
        t.timestamp = detectTimeUs;
        targets.push_back(t);
    }

    static long long lastTrackMs = 0;
    long long now = getTimeNowMs();
    float dt = lastTrackMs ? (now - lastTrackMs) / 1000.0f : 0.016f;
    lastTrackMs = now;
    g_tracker.update(targets, dt);
    auto tracks = g_tracker.activeTargets();

    {
        std::lock_guard<std::mutex> lock(g_detMutex);
        g_tracks = tracks;
    }

    if (tracks.empty()) {
        // 目标全部丢失：释放自瞄/扳机虚拟手指，避免卡在屏幕上
        g_aimActive = false;
        releaseAimFingers();
        return;
    }

    // 选择目标（硬锁定：使用检测位置）
    const AimTarget* pick = nullptr;
    float bestScore = -1.0f;
    float screenCx = 0.5f, screenCy = 0.5f;
    for (const auto& t : tracks) {
        if (g_cfg.aimClass >= 0 && (int)t.classId != g_cfg.aimClass) continue;

        // 硬锁定：直接使用检测位置做选择
        float pcx = t.cx;
        float pcy = t.cy;

        float score;
        float dx = pcx - screenCx;
        float dy = pcy - screenCy;
        switch (g_cfg.selectMode) {
        case 1: score = (t.x2 - t.x1) * (t.y2 - t.y1); break;
        case 2: score = -std::sqrt(dx*dx + dy*dy); break;
        default: score = -std::sqrt(dx*dx + dy*dy); break;
        }
        if (score > bestScore) { bestScore = score; pick = &t; }
    }
    if (!pick) {
        g_aimActive = false;
        releaseAimFingers();
        return;
    }

    // 按锁定部位调整瞄准点（硬锁定：直接使用检测位置）
    AimTarget aimTarget = *pick;
    {
        // 直接使用检测位置，不做预测偏移
        float ax, ay;
        computeAimPoint(*pick, g_cfg.aimPart, &ax, &ay);
        ax += g_cfg.aimOffsetX;
        ay += g_cfg.aimOffsetY;
        // 瞄准点时间平滑（EMA）：同一跟踪目标用指数移动平均压掉检测框抖动，
        // 避免“拖视角→框移动→再拖”的反馈振荡传导到自瞄（尤其锁头/身体时 Y 轴上下甩）。
        // 切换跟踪目标时重置，避免新旧目标位置混叠。
        float alpha = g_cfg.aimPointSmooth; // 上一帧权重（0~1，越大越平滑/越迟钝）
        if (g_aimSmoothTrack != pick->trackId) {
            g_aimSmoothX = ax;
            g_aimSmoothY = ay;
            g_aimSmoothTrack = pick->trackId;
        } else if (alpha > 0.0f) {
            g_aimSmoothX = g_aimSmoothX * alpha + ax * (1.0f - alpha);
            g_aimSmoothY = g_aimSmoothY * alpha + ay * (1.0f - alpha);
            ax = g_aimSmoothX;
            ay = g_aimSmoothY;
        }
        aimTarget.cx = ax;
        aimTarget.cy = ay;
    }

    // 触控区（归一化 → 像素坐标）。自瞄虚拟手指在该区域内拖动，不超出边界。
    // 起始位置为触控区中心，模拟玩家在游戏转向/瞄准区域内的操作。
    int scrW = native_window_screen_x;
    int scrH = native_window_screen_y;
    float tzLpx = g_cfg.touchZoneL * scrW, tzTpx = g_cfg.touchZoneT * scrH;
    float tzRpx = g_cfg.touchZoneR * scrW, tzBpx = g_cfg.touchZoneB * scrH;
    float tzCx = (tzLpx + tzRpx) * 0.5f, tzCy = (tzTpx + tzBpx) * 0.5f;

    // 区域编辑模式下暂停自瞄/扳机（配置区域时避免误拖视角/误开火）
    bool zoneEditing = (g_cfg.zoneEditTarget != 0);

    // 内核陀螺仪模式：勾选后惰性连接驱动并初始化陀螺仪 hook。
    // 该模式下自瞄/压枪注入走内核陀螺仪，扳机禁用；
    // uinput 设备保持初始化，游戏屏幕仍可被注入触摸。
    if (g_cfg.gyroAim && !touch_kernel_connected()) {
        bool gyroOk = touch_kernel_gyro_init();
        // 驱动连接失败时回退保底：确保 uinput 注入可用，避免自瞄/扳机全部失效
        if (!gyroOk && g_touchReady && !touch_inject_ready())
            touch_inject_init();
    }
    const bool gyroMode = g_cfg.gyroAim && touch_kernel_connected();
    const bool kernelTouchModeOn = g_cfg.kernelTouch && kdrv_connected();

    // 自瞄触发区/倍镜区门控：开启时物理手指需点在对应区域内才允许自瞄
    bool aimGateOk = true;
    if (!zoneEditing) {
        if (g_cfg.aimTriggerZoneEnabled &&
            !isFingerInZone(g_cfg.aimTriggerZoneL, g_cfg.aimTriggerZoneT,
                            g_cfg.aimTriggerZoneR, g_cfg.aimTriggerZoneB, scrW, scrH))
            aimGateOk = false;
        if (g_cfg.adsZoneEnabled &&
            !isFingerInZone(g_cfg.adsZoneL, g_cfg.adsZoneT,
                            g_cfg.adsZoneR, g_cfg.adsZoneB, scrW, scrH))
            aimGateOk = false;
    }

    // ── 自瞄（拖动视角式）──
    // 三种算法：0=原版(拖拽+平滑) 1=PID 2=贝塞尔，均由控制器输出每帧增量，
    // 这里保持虚拟手指按下并逐帧移动，模拟人手拖屏转向；目标进入死区后抬起手指。
    // 手指被 clamp 在触控区内。
    AimOutput out;
    bool  aimActiveNow = false;
    float dpx = 0.0f, dpy = 0.0f;
    // 自瞄增量计算不依赖 uinput（只算误差增量）；陀螺仪模式下即使设备扫描/uinput 不可用
    // 也照常计算，注入交给内核陀螺仪；内核触摸模式下自瞄注入走 TimeDriver；
    // uinput 模式下仍要求 g_touchReady。
    if ((gyroMode || g_touchReady || kernelTouchModeOn) && g_cfg.aimEnabled && g_cfg.enabled && !zoneEditing && aimGateOk) {
        switch (g_cfg.aimMode) {
        case 0:
            out = g_pidAim.compute(aimTarget, g_cfg, screenCx, screenCy, dt,
                                   (float)scrW, (float)scrH);
            break;
        case 1:
            out = g_bezierAim.compute(aimTarget, g_cfg, screenCx, screenCy, dt,
                                      (float)scrW, (float)scrH);
            break;
        default:
            out = g_pidAim.compute(aimTarget, g_cfg, screenCx, screenCy, dt,
                                   (float)scrW, (float)scrH);
            break;
        }
        if (out.active) {
            dpx = out.deltaX * scrW;
            dpy = out.deltaY * scrH;
            // 自瞄回正速度限制：归一化每帧最大瞄准点移动距离
            if (g_cfg.aimApproachSpeed > 0.0f) {
                float maxPx = g_cfg.aimApproachSpeed * (float)std::min(scrW, scrH);
                float step = std::sqrt(dpx*dpx + dpy*dpy);
                if (step > maxPx) { dpx *= maxPx / step; dpy *= maxPx / step; }
            }
            aimActiveNow = true;
        }
    }

    // ── 压枪（检测物理触摸开枪键，视角自动下拉补偿后坐力）──
    // 独立功能：不依赖扳机（自动开火）。当检测到物理手指按在“开枪区域”
    // （fire zone）时启动压枪计时。
    // 开始时间：开枪键按住持续到 recoilStartMs 后开始下拉；
    // 力度：每帧下拉 recoilStrength(px/s) × dt 像素，模拟人手持续下拉压枪。
    // 开枪区只需任一功能（压枪/扳机）启用即同步到 touch_core，供硬件手指检测。
    if (g_touchReady && g_cfg.enabled &&
        (g_cfg.recoilEnabled || g_cfg.triggerEnabled)) {
        int fzL = (int)(g_cfg.fireZoneL * scrW), fzT = (int)(g_cfg.fireZoneT * scrH);
        int fzR = (int)(g_cfg.fireZoneR * scrW), fzB = (int)(g_cfg.fireZoneB * scrH);
        touch_set_fire_zone(fzL, fzT, fzR, fzB);
    }
    const bool recoilArmed = g_cfg.enabled && !zoneEditing &&
                             g_cfg.recoilEnabled &&
                             (gyroMode ? (g_cfg.recoilDegPerSec > 0.0f)
                                       : ((g_touchReady || kernelTouchModeOn) && g_cfg.recoilStrength > 0));
    if (recoilArmed && touch_is_finger_in_fire_zone()) {
        if (!g_recoilFiring) { g_recoilFiring = true; g_recoilStartMs = now; }
    } else {
        g_recoilFiring = false;
    }
    const bool recoilPulling = g_recoilFiring &&
                               (now - g_recoilStartMs >= (long long)g_cfg.recoilStartMs);

    // ── 自瞄/压枪统一输出 ──
    // 陀螺仪模式：自瞄增量换算为 pitch/yaw 角度，压枪叠加下拉角速度。
    // 关键：推理线程只计算目标，写入 g_gyroTargetPitch/Yaw，不直接注入。
    // 主线程每帧（~1ms）读取目标并平滑插值注入，实现丝滑陀螺仪手感。
    if (gyroMode) {
        if (aimActiveNow || recoilPulling) {
            // 屏幕增量（像素）→ 角度：Y 增量 → pitch（俯仰），X 增量 → yaw（偏航）
            float pitch = dpy * g_cfg.gyroSens;
            float yaw   = dpx * g_cfg.gyroSens;
            
            // 方向反转检测：当前目标方向与上帧注入方向相反
            bool dirReversed = false;
            {
                float prevPitch = g_gyroLastTargetPitch;
                float prevYaw = g_gyroLastTargetYaw;
                bool signRevPitch = (pitch > 0 && prevPitch < 0) || (pitch < 0 && prevPitch > 0);
                bool signRevYaw = (yaw > 0 && prevYaw < 0) || (yaw < 0 && prevYaw > 0);
                dirReversed = signRevPitch || signRevYaw;
            }

            if (!dirReversed) {
                // 限制变化率
                float maxChange = 3.0f / (g_cfg.gyroSens + 0.5f);
                float dp = pitch - g_gyroLastTargetPitch;
                float dy = yaw - g_gyroLastTargetYaw;
                float changeMag = std::sqrt(dp*dp + dy*dy);
                if (changeMag > maxChange) {
                    float k = maxChange / changeMag;
                    pitch = g_gyroLastTargetPitch + dp * k;
                    yaw = g_gyroLastTargetYaw + dy * k;
                }
            }
            g_gyroLastTargetPitch = pitch;
            g_gyroLastTargetYaw = yaw;
            
            // 分离自瞄和压枪的限幅：
            float maxDeg = g_cfg.gyroMaxDeg;
            float maxYawDeg = maxDeg * 1.5f;

            // 自瞄 Yaw 限幅
            if (std::fabs(yaw) > maxYawDeg) {
                yaw = (yaw > 0) ? maxYawDeg : -maxYawDeg;
            }
            // 自瞄 Pitch 限幅
            if (std::fabs(pitch) > maxDeg) {
                pitch = (pitch > 0) ? maxDeg : -maxDeg;
            }

            // 反转在压枪之前应用
            if (g_cfg.gyroInvertPitch) pitch = -pitch;
            if (g_cfg.gyroInvertYaw)   yaw = -yaw;

            // 压枪：在反转之后应用，确保方向始终往下压
            // 使用 -= 确保压枪方向不受反转设置影响
            if (recoilPulling) {
                float recoilDt = g_cfg.recoilDegPerSec * dt;
                // 移动时额外压枪补偿
                float moveSpeed = std::sqrt(dpx*dpx + dpy*dpy);
                if (moveSpeed > 50.0f) {
                    float boost = std::min(moveSpeed / 200.0f, 1.0f);
                    recoilDt *= (1.0f + boost * 0.5f);
                }
                pitch -= recoilDt;
            }
            float finalPitch = pitch;

            {
                std::lock_guard<std::mutex> lock(g_gyroMutex);
                g_gyroTargetPitch = finalPitch;
                g_gyroTargetYaw   = yaw;
                g_gyroTargetActive = true;
                g_gyroTargetDirReversed = dirReversed;
            }
            g_aimActive = aimActiveNow;
            if (aimActiveNow) {
                g_aimX = out.targetX;
                g_aimY = out.targetY;
            }
        } else {
            // 目标丢失：通知主线程停止注入
            {
                std::lock_guard<std::mutex> lock(g_gyroMutex);
                g_gyroTargetActive = false;
                g_gyroLastTargetPitch = 0.0f;
                g_gyroLastTargetYaw = 0.0f;
                g_gyroTargetDirReversed = false;
            }
            g_aimActive = false;
        }
    } else if (canInjectTouch() && (aimActiveNow || recoilPulling)) {
        // 视角手指：自瞄增量 + 压枪下拉合并移动
        // 根据当前模式选择 uinput 或内核驱动注入
        float dx = dpx;
        float dy = dpy;
        if (recoilPulling) dy += g_cfg.recoilStrength * dt;  // 压枪下拉（px/s × s）
        if (!g_aimFingerDown) {
            // 手指从触控区中心按下
            g_aimFingerX = tzCx;
            g_aimFingerY = tzCy;
            inject_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID,
                       (int)g_aimFingerX, (int)g_aimFingerY);
            g_aimFingerDown = true;
        }
        g_aimFingerX = std::clamp(g_aimFingerX + dx, tzLpx, tzRpx);
        g_aimFingerY = std::clamp(g_aimFingerY + dy, tzTpx, tzBpx);
        inject_move(TOUCH_VIRTUAL_SLOT, (int)g_aimFingerX, (int)g_aimFingerY);
        // 拖到远离触控区中心时抬手回中心再按下，模拟人手重新起指，
        // 保证目标在屏幕边缘也能持续转向（游戏累积每段拖拽的旋转量）
        float drift = std::sqrt((g_aimFingerX - tzCx) * (g_aimFingerX - tzCx) +
                                (g_aimFingerY - tzCy) * (g_aimFingerY - tzCy));
        if (drift > std::max(tzRpx - tzLpx, tzBpx - tzTpx) * 0.5f) {
            inject_up(TOUCH_VIRTUAL_SLOT);
            g_aimFingerX = tzCx;
            g_aimFingerY = tzCy;
            inject_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID,
                       (int)g_aimFingerX, (int)g_aimFingerY);
        }
        g_aimActive = aimActiveNow;
        if (aimActiveNow) {
            g_aimX = out.targetX;
            g_aimY = out.targetY;
        }
    } else {
        if (g_aimFingerDown) {
            inject_up(TOUCH_VIRTUAL_SLOT);
            g_aimFingerDown = false;
        }
        g_aimActive = false;
    }

    // ── 扳机（点射/按住可切换）──
    // 触发区（fire zone）：玩家物理手指在此区域内时，暂停自动开火，避免与手动开火冲突。
    // 先按配置同步触发区到 touch_core，再让 reader 线程做硬件手指检测。
    // 陀螺仪模式下扳机禁用（自瞄/压枪走内核陀螺仪，扳机走 uinput 会与陀螺仪冲突）。
    // 内核触摸模式下扳机可用（走 TimeDriver 内核触摸注入，不与内核陀螺仪冲突）。
    if (canInjectTouch() && (!gyroMode || kernelTouchModeOn) && g_cfg.triggerEnabled && g_cfg.enabled) {
        int fzL = (int)(g_cfg.fireZoneL * scrW), fzT = (int)(g_cfg.fireZoneT * scrH);
        int fzR = (int)(g_cfg.fireZoneR * scrW), fzB = (int)(g_cfg.fireZoneB * scrH);
        touch_set_fire_zone(fzL, fzT, fzR, fzB);

        bool fireOnce = false, hold = false, holdRelease = false;
        bool fingerDown = touch_is_finger_in_fire_zone();
        g_trigger.update(aimTarget, g_cfg, screenCx, screenCy, fingerDown,
                         fireOnce, hold, holdRelease);
        // 扳机注入落点 = 触发区中心（开火按钮位置）
        int trigX = (int)((g_cfg.fireZoneL + g_cfg.fireZoneR) * 0.5f * scrW);
        int trigY = (int)((g_cfg.fireZoneT + g_cfg.fireZoneB) * 0.5f * scrH);
        if (fireOnce) {
            if (g_triggerDown) { inject_up(TOUCH_TRIGGER_SLOT); g_triggerDown = false; }
            inject_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID, trigX, trigY);
            inject_up(TOUCH_TRIGGER_SLOT);
        } else if (hold) {
            if (!g_triggerDown) {
                inject_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID, trigX, trigY);
                g_triggerDown = true;
            }
        } else if (holdRelease || g_triggerDown) {
            inject_up(TOUCH_TRIGGER_SLOT);
            g_triggerDown = false;
        }
    } else if (g_triggerDown) {
        inject_up(TOUCH_TRIGGER_SLOT);
        g_triggerDown = false;
    }
}

// ---------------------------------------------------------------------------
// 推理线程
// ---------------------------------------------------------------------------
static std::atomic<bool> g_inferRunning{false};

static void inferenceLoop() {
    uint64_t frames = 0;
    long long lastFpsMs = getTimeNowMs();
    long long lastNoFrameLog = getTimeNowMs();
    long long lastStuckLog = getTimeNowMs();
    long long lastSrcFpsMs = getTimeNowMs();
    uint64_t lastWriteOk = 0;
    bool everGotFrame = false;
    bool wasEnabled = true;

    while (g_inferRunning.load()) {
        // 帧源速率统计：APK 写帧速率（writeSuccesses 增量/秒）。放在最前，
        // 这样即使推理被关闭也能持续刷新，用于区分“帧源只产 72 帧”和“推理慢”。
        {
            long long now = getTimeNowMs();
            if (now - lastSrcFpsMs >= 1000) {
                uint64_t ws = (g_shm && g_shm->valid()) ? g_shm->writeSuccesses() : 0;
                if (ws >= lastWriteOk)
                    g_srcFps.store((uint64_t)((ws - lastWriteOk) * 1000 / (now - lastSrcFpsMs)));
                else
                    g_srcFps.store(0);  // 共享内存被重建/重置，计数回退
                lastWriteOk = ws;
                lastSrcFpsMs = now;
            }
        }

        if (!g_shm || !g_shm->valid()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        // 推理启动开关：关闭或未通过卡密验证时完全停止推理（不再 detect），
        // 并清空显示结果，让 FPS 归零、检测框消失，而不是只停自瞄
        if (!g_cfg.enabled || !g_t3Verified.load()) {
            if (wasEnabled) {
                wasEnabled = false;
                std::lock_guard<std::mutex> lock(g_detMutex);
                g_detections.clear();
                g_tracks.clear();
                g_frameCount.store(0);
                g_inferFps.store(0);
                printf("[infer] 推理已停止\n");
            }
            // 推理停止后不再有 processFrame 来释放虚拟手指，这里主动抬起
            releaseAimFingers();
            std::this_thread::sleep_for(10ms);
            continue;
        }
        wasEnabled = true;

        const uint8_t* frame = g_shm->readFrame();
        if (!frame) {
            // 诊断：长时间没有新帧（APK 未写帧 / 共享内存未通）
            long long now = getTimeNowMs();
            if (now - lastNoFrameLog >= 5000) {
                lastNoFrameLog = now;
                printf("[infer] no new frame for 5s, everGotFrame=%d, %s\n",
                       everGotFrame ? 1 : 0, g_shm->diag().c_str());
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        everGotFrame = true;

        long long frameStart = getTimeNowMs();
        processFrame(frame, g_shm->freshHeader());
        long long frameDone = getTimeNowMs();
        g_lastFrameMs.store(frameDone - frameStart);
        g_lastFrameDoneMs.store(frameDone);
        g_lastFrameTimestamp.store(frameDone);
        // EMA 平滑推理延迟（用于绘制时的预测补偿，避免跳变）
        float latMs = (float)(frameDone - frameStart);
        float oldLat = (float)g_inferLatencyMs.load();
        g_inferLatencyMs.store((int)(oldLat * 0.7f + latMs * 0.3f));
        frames++;

        // 推理帧率上限（节流）：达到上限后等待到下一帧时隙，避免无谓的 CPU/耗电
        if (g_cfg.fpsLimit > 0) {
            long long elapsed = getTimeNowMs() - frameStart;
            long long targetMs = 1000LL / g_cfg.fpsLimit;
            if (targetMs > elapsed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(targetMs - elapsed));
            }
        }

        // FPS 统计
        long long now = getTimeNowMs();
        if (now - lastFpsMs >= 1000) {
            g_inferFps.store(frames * 1000 / (now - lastFpsMs));
            frames = 0;
            lastFpsMs = now;
        }

        // 诊断：帧源在产帧但单帧处理超时（大概率 QNN invoke 卡死）
        if (g_lastFrameMs.load() > 3000 && now - lastStuckLog >= 5000) {
            lastStuckLog = now;
            printf("[infer] frame took %lld ms (>=3000ms)! backend=%s, "
                   "shmSeq=%u, processed=%llu\n",
                   g_lastFrameMs.load(),
                   g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "?",
                   g_shm->readSeq(), (unsigned long long)g_frameCount.load());
        }
    }
}

// ---------------------------------------------------------------------------
// UI 绘制
// ---------------------------------------------------------------------------
static void drawDetectionOverlay() {
    if (!g_cfg.showBoxes && !g_cfg.showFps && !g_cfg.showCropBox && !g_cfg.showZones) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    float sx = native_window_screen_x;
    float sy = native_window_screen_y;

    // 可视化触控区/触发区当前位置：调节参数时可直观看到区域范围
    if (g_cfg.showZones) {
        // 触控区（自瞄拖拽注入区）：蓝色半透明填充 + 边框 + 文字
        {
            float zl = g_cfg.touchZoneL * sx, zt = g_cfg.touchZoneT * sy;
            float zr = g_cfg.touchZoneR * sx, zb = g_cfg.touchZoneB * sy;
            draw->AddRectFilled(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(0, 140, 255, 32));
            draw->AddRect(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(0, 140, 255, 230), 0.0f, 0, 2.0f);
            draw->AddText(ImVec2(zl + 4, zt + 4), IM_COL32(0, 170, 255, 255), "触控区(自瞄)");
            char infolbl[48];
            snprintf(infolbl, sizeof(infolbl), "L%.2f T%.2f R%.2f B%.2f",
                     g_cfg.touchZoneL, g_cfg.touchZoneT, g_cfg.touchZoneR, g_cfg.touchZoneB);
            draw->AddText(ImVec2(zl + 4, zt + 20), IM_COL32(0, 170, 255, 255), infolbl);
        }
        // 触发区（扳机暂停区，玩家物理手指在此区域内时自动开火暂停）：红色半透明
        {
            float zl = g_cfg.fireZoneL * sx, zt = g_cfg.fireZoneT * sy;
            float zr = g_cfg.fireZoneR * sx, zb = g_cfg.fireZoneB * sy;
            draw->AddRectFilled(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(255, 70, 70, 32));
            draw->AddRect(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(255, 70, 70, 230), 0.0f, 0, 2.0f);
            draw->AddText(ImVec2(zl + 4, zt + 4), IM_COL32(255, 110, 110, 255), "触发区(扳机)");
            char infolbl[48];
            snprintf(infolbl, sizeof(infolbl), "L%.2f T%.2f R%.2f B%.2f",
                     g_cfg.fireZoneL, g_cfg.fireZoneT, g_cfg.fireZoneR, g_cfg.fireZoneB);
            draw->AddText(ImVec2(zl + 4, zt + 20), IM_COL32(255, 110, 110, 255), infolbl);
        }
        // 自瞄触发区（点击该区域才触发自瞄）：绿色半透明
        if (g_cfg.aimTriggerZoneEnabled) {
            float zl = g_cfg.aimTriggerZoneL * sx, zt = g_cfg.aimTriggerZoneT * sy;
            float zr = g_cfg.aimTriggerZoneR * sx, zb = g_cfg.aimTriggerZoneB * sy;
            draw->AddRectFilled(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(0, 255, 140, 28));
            draw->AddRect(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(0, 255, 140, 230), 0.0f, 0, 2.0f);
            draw->AddText(ImVec2(zl + 4, zt + 4), IM_COL32(0, 255, 160, 255), "自瞄触发区");
            char infolbl[48];
            snprintf(infolbl, sizeof(infolbl), "L%.2f T%.2f R%.2f B%.2f",
                     g_cfg.aimTriggerZoneL, g_cfg.aimTriggerZoneT,
                     g_cfg.aimTriggerZoneR, g_cfg.aimTriggerZoneB);
            draw->AddText(ImVec2(zl + 4, zt + 20), IM_COL32(0, 255, 160, 255), infolbl);
        }
        // 倍镜区（点击该区域才触发自瞄，开镜区域）：橙色半透明
        if (g_cfg.adsZoneEnabled) {
            float zl = g_cfg.adsZoneL * sx, zt = g_cfg.adsZoneT * sy;
            float zr = g_cfg.adsZoneR * sx, zb = g_cfg.adsZoneB * sy;
            draw->AddRectFilled(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(255, 180, 0, 28));
            draw->AddRect(ImVec2(zl, zt), ImVec2(zr, zb), IM_COL32(255, 180, 0, 230), 0.0f, 0, 2.0f);
            draw->AddText(ImVec2(zl + 4, zt + 4), IM_COL32(255, 200, 60, 255), "倍镜区");
            char infolbl[48];
            snprintf(infolbl, sizeof(infolbl), "L%.2f T%.2f R%.2f B%.2f",
                     g_cfg.adsZoneL, g_cfg.adsZoneT, g_cfg.adsZoneR, g_cfg.adsZoneB);
            draw->AddText(ImVec2(zl + 4, zt + 20), IM_COL32(255, 200, 60, 255), infolbl);
        }
    }

    // 裁剪区域描边：在屏幕上画一个矩形框，标出当前推理输入的裁剪范围
    if (g_cfg.showCropBox && g_shm && g_shm->valid()) {
        auto ci = g_shm->cropInfo();
        if (ci.size > 0) {
            float x1 = ci.offX * sx / ci.fullW;
            float y1 = ci.offY * sy / ci.fullH;
            float x2 = (ci.offX + ci.size) * sx / ci.fullW;
            float y2 = (ci.offY + ci.size) * sy / ci.fullH;
            // 外描边（黑色，粗线） + 内描边（黄色虚线风格，实线）
            draw->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(0, 0, 0, 200), 0.0f, 0, 5.0f);
            draw->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 255, 0, 200), 0.0f, 0, 2.0f);
            // 角落标注尺寸文字
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%d×%d", ci.size, ci.size);
            draw->AddText(ImVec2(x1 + 4, y1 + 4), IM_COL32(255, 255, 0, 220), lbl);
        }
    }

    if (g_cfg.showFps) {
        char buf[128];
        snprintf(buf, sizeof(buf), "帧率: %llu  检测: %zu  跟踪: %zu  推理延迟: %.1fms",
                 (unsigned long long)g_inferFps.load(),
                 g_detections.size(), g_tracks.size(),
                 (float)g_inferLatencyMs.load());
        draw->AddText(ImVec2(20, 20), IM_COL32(255, 255, 0, 255), buf);
    }

    std::lock_guard<std::mutex> lock(g_detMutex);

    // 硬锁定跟随：框位置 = 检测位置 + 自适应微补偿
    long long renderTimeUs = getTimeNowUs();

    // 每个 trackId 独立的状态
    struct TrackState {
        float vx = 0, vy = 0;        // 平滑速度
        float lastCx = 0, lastCy = 0;
        int64_t lastTs = 0;
        int frameCount = 0;
    };
    static std::unordered_map<int, TrackState> g_trackStates;

    if (g_cfg.showBoxes) {
        std::unordered_set<int> currentIds;

        for (const auto& t : g_tracks) {
            currentIds.insert(t.trackId);
            float hw = (t.x2 - t.x1) * 0.5f;
            float hh = (t.y2 - t.y1) * 0.5f;
            float boxW = hw * 2.0f;
            float boxH = hh * 2.0f;

            auto& st = g_trackStates[t.trackId];

            // 计算速度
            if (st.frameCount > 0 && st.lastTs > 0) {
                float dtSec = (float)(t.timestamp - st.lastTs) * 0.000001f;
                if (dtSec > 0.001f && dtSec < 1.0f) {
                    float rvx = (t.cx - st.lastCx) / dtSec;
                    float rvy = (t.cy - st.lastCy) / dtSec;
                    // EMA 平滑速度
                    st.vx += 0.4f * (rvx - st.vx);
                    st.vy += 0.4f * (rvy - st.vy);
                }
            }

            // 限制速度（归一化/秒）
            float maxV = 2.0f;
            if (st.vx > maxV) st.vx = maxV;
            if (st.vx < -maxV) st.vx = -maxV;
            if (st.vy > maxV) st.vy = maxV;
            if (st.vy < -maxV) st.vy = -maxV;

            // 更新历史
            st.lastCx = t.cx;
            st.lastCy = t.cy;
            st.lastTs = t.timestamp;
            st.frameCount++;

            // === 核心：硬锁定检测位置 ===
            float pcx = t.cx;
            float pcy = t.cy;

            // === 自适应微补偿 ===
            if (st.frameCount >= 2) {
                float speedMag = sqrtf(st.vx * st.vx + st.vy * st.vy);

                // 自适应补偿系数：
                // - 速度慢（<0.3/秒）：补偿 80% 的延迟 → 几乎跟随预测
                // - 速度快（>1.0/秒）：补偿 30% 的延迟 → 信任检测位置
                // 速度越快，预测越不可靠，越应该直接跟随检测位置
                float compRatio;
                if (speedMag < 0.3f) {
                    compRatio = 0.8f;  // 慢速：多补偿
                } else if (speedMag < 1.0f) {
                    compRatio = 0.6f;  // 中速：中等补偿
                } else {
                    compRatio = 0.25f;  // 快速：少补偿
                }

                // 计算延迟并限制
                float delayMs = (float)(renderTimeUs - t.timestamp) * 0.001f;
                if (delayMs < 0.0f) delayMs = 0.0f;
                if (delayMs > 16.0f) delayMs = 16.0f;  // 最多补偿 16ms（约1帧）

                float compSec = delayMs * 0.001f * compRatio;

                // 计算补偿偏移
                float offX = st.vx * compSec;
                float offY = st.vy * compSec;

                // 硬限制：补偿偏移不超过框宽/高的 3%（极小值）
                float maxOffX = boxW * 0.03f;
                float maxOffY = boxH * 0.03f;
                if (offX > maxOffX) offX = maxOffX;
                if (offX < -maxOffX) offX = -maxOffX;
                if (offY > maxOffY) offY = maxOffY;
                if (offY < -maxOffY) offY = -maxOffY;

                // 应用补偿
                pcx += offX;
                pcy += offY;
            }

            // === 绘制 ===
            ImVec2 p1((pcx - hw) * sx, (pcy - hh) * sy);
            ImVec2 p2((pcx + hw) * sx, (pcy + hh) * sy);
            int thick = std::max(1, g_cfg.boxThickness);
            int outline = thick + 3;
            draw->AddRect(p1, p2, IM_COL32(0, 0, 0, 200), 0.0f, 0, (float)outline);
            draw->AddRect(p1, p2, IM_COL32(0, 255, 0, 255), 0.0f, 0, (float)thick);
            if (g_cfg.showBoxLabels) {
                char lbl[64];
                const char* cname = g_engine ? g_engine->getClassName(t.classId) : nullptr;
                if (cname && cname[0])
                    snprintf(lbl, sizeof(lbl), "%s %.2f", cname, t.score);
                else
                    snprintf(lbl, sizeof(lbl), "%.2f", t.score);
                draw->AddText(ImVec2(p1.x - 1, p1.y - 1), IM_COL32(0, 0, 0, 220), lbl);
                draw->AddText(ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 220), lbl);
                draw->AddText(p1, IM_COL32(0, 255, 0, 255), lbl);
            }
        }

        // 清理
        for (auto it = g_trackStates.begin(); it != g_trackStates.end(); ) {
            if (currentIds.find(it->first) == currentIds.end())
                it = g_trackStates.erase(it);
            else ++it;
        }
    }

    // 准星
    if (g_cfg.aimEnabled) {
        float cx = sx * 0.5f, cy = sy * 0.5f;
        int col = g_aimActive ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 255, 120);
        draw->AddLine(ImVec2(cx - 20, cy), ImVec2(cx + 20, cy), col, 2.0f);
        draw->AddLine(ImVec2(cx, cy - 20), ImVec2(cx, cy + 20), col, 2.0f);
    }

    // 辅助连线：使用检测位置（硬锁定，不做预测补偿）
    if (g_cfg.showAimLines && g_cfg.showBoxes) {
        ImVec2 anchor(sx * 0.5f, 0.0f);
        draw->AddCircleFilled(anchor, 5.0f, IM_COL32(0, 255, 255, 255), 12);
        for (const auto& t : g_tracks) {
            float hh = (t.y2 - t.y1) * 0.5f;
            ImVec2 boxTop(t.cx * sx, (t.cy - hh) * sy);
            draw->AddLine(anchor, boxTop, IM_COL32(0, 255, 255, 150), 1.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// 区域拖拽编辑：在悬浮窗上直接拖拽控制点调整区域大小和位置。
// 通过 ImGui 的鼠标输入（主循环已把物理手指映射为鼠标）检测拖动。
// ---------------------------------------------------------------------------
static void drawZoneEditor() {
    int target = g_cfg.zoneEditTarget;
    if (target < 1 || target > 4) { g_zoneDrag.dragging = false; return; }

    float sx = native_window_screen_x;
    float sy = native_window_screen_y;

    // 获取当前编辑区域的坐标（归一化）
    float* zL = nullptr, *zT = nullptr, *zR = nullptr, *zB = nullptr;
    ImU32 color = 0;
    const char* label = "";
    switch (target) {
    case 1:
        zL = &g_cfg.touchZoneL; zT = &g_cfg.touchZoneT;
        zR = &g_cfg.touchZoneR; zB = &g_cfg.touchZoneB;
        color = IM_COL32(0, 140, 255, 255); label = "触控区"; break;
    case 2:
        zL = &g_cfg.fireZoneL; zT = &g_cfg.fireZoneT;
        zR = &g_cfg.fireZoneR; zB = &g_cfg.fireZoneB;
        color = IM_COL32(255, 70, 70, 255); label = "触发区(扳机)"; break;
    case 3:
        zL = &g_cfg.aimTriggerZoneL; zT = &g_cfg.aimTriggerZoneT;
        zR = &g_cfg.aimTriggerZoneR; zB = &g_cfg.aimTriggerZoneB;
        color = IM_COL32(0, 255, 140, 255); label = "自瞄触发区"; break;
    case 4:
        zL = &g_cfg.adsZoneL; zT = &g_cfg.adsZoneT;
        zR = &g_cfg.adsZoneR; zB = &g_cfg.adsZoneB;
        color = IM_COL32(255, 180, 0, 255); label = "倍镜区"; break;
    default: return;
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    float zl = *zL * sx, zt = *zT * sy, zr = *zR * sx, zb = *zB * sy;

    // 控制点：8 个（4 角 + 4 边中点），拖动调整大小；区域内部拖动移动整个区域
    ImVec2 cps[8] = {
        ImVec2(zl, zt), ImVec2(zr, zt), ImVec2(zr, zb), ImVec2(zl, zb), // 角
        ImVec2((zl + zr) * 0.5f, zt), ImVec2(zr, (zt + zb) * 0.5f),
        ImVec2((zl + zr) * 0.5f, zb), ImVec2(zl, (zt + zb) * 0.5f),     // 边中点
    };

    // 半透明填充 + 边框 + 标签
    draw->AddRectFilled(ImVec2(zl, zt), ImVec2(zr, zb), (color & 0x00FFFFFF) | 0x22000000);
    draw->AddRect(ImVec2(zl, zt), ImVec2(zr, zb), color, 0.0f, 0, 3.0f);
    draw->AddText(ImVec2(zl + 6, zt + 6), color, label);

    // 绘制控制点
    for (int i = 0; i < 8; ++i) {
        draw->AddCircleFilled(cps[i], 8.0f, IM_COL32(255, 255, 255, 255), 12);
        draw->AddCircle(cps[i], 8.0f, color, 12, 2.0f);
    }

    // 触摸拖动逻辑：物理手指在控制点附近按下 → 开始拖；拖动时实时更新区域坐标
    ImGuiIO& io = ImGui::GetIO();
    bool mouseDown = ImGui::IsMouseDown(0);
    ImVec2 mp = io.MousePos;
    // 面板窗口挡住时忽略编辑（防止与面板交互冲突）
    bool overPanel = io.WantCaptureMouse;

    if (g_zoneDrag.dragging) {
        if (!mouseDown) {
            g_zoneDrag.dragging = false; // 抬手结束
        } else {
            // 计算相对拖拽起点的位移（归一化）
            float dnx = (mp.x - g_zoneDrag.dragStartMX) / sx;
            float dny = (mp.y - g_zoneDrag.dragStartMY) / sy;
            float nL = g_zoneDrag.origL, nT = g_zoneDrag.origT;
            float nR = g_zoneDrag.origR, nB = g_zoneDrag.origB;
            int dp = g_zoneDrag.dragPoint;
            if (dp == 0) { nL += dnx; nT += dny; }
            else if (dp == 1) { nR += dnx; nT += dny; }
            else if (dp == 2) { nR += dnx; nB += dny; }
            else if (dp == 3) { nL += dnx; nB += dny; }
            else if (dp == 4) { nT += dny; }
            else if (dp == 5) { nR += dnx; }
            else if (dp == 6) { nB += dny; }
            else if (dp == 7) { nL += dnx; }
            else if (dp == 8) { nL += dnx; nR += dnx; nT += dny; nB += dny; } // 整体移动
            // 限制范围并保证最小尺寸
            const float MIN = 0.02f;
            if (nR - nL < MIN) nR = nL + MIN;
            if (nB - nT < MIN) nB = nT + MIN;
            nL = std::clamp(nL, 0.0f, 1.0f - MIN);
            nT = std::clamp(nT, 0.0f, 1.0f - MIN);
            nR = std::clamp(nR, nL + MIN, 1.0f);
            nB = std::clamp(nB, nT + MIN, 1.0f);
            if (nR > nL) { *zL = nL; *zR = nR; }
            if (nB > nT) { *zT = nT; *zB = nB; }
            // 触发区实时同步给 touch_core
            if (target == 2 && g_touchReady) {
                touch_set_fire_zone((int)(g_cfg.fireZoneL * sx), (int)(g_cfg.fireZoneT * sy),
                                    (int)(g_cfg.fireZoneR * sx), (int)(g_cfg.fireZoneB * sy));
            }
        }
    } else if (mouseDown && !overPanel) {
        // 检测是否点中控制点
        for (int i = 0; i < 8; ++i) {
            if (std::fabs(mp.x - cps[i].x) < 22.0f && std::fabs(mp.y - cps[i].y) < 22.0f) {
                g_zoneDrag.dragging = true;
                g_zoneDrag.dragPoint = i;
                g_zoneDrag.dragStartMX = mp.x;
                g_zoneDrag.dragStartMY = mp.y;
                g_zoneDrag.origL = *zL; g_zoneDrag.origT = *zT;
                g_zoneDrag.origR = *zR; g_zoneDrag.origB = *zB;
                return;
            }
        }
        // 点中区域内部 → 整体移动
        if (mp.x >= zl && mp.x <= zr && mp.y >= zt && mp.y <= zb) {
            g_zoneDrag.dragging = true;
            g_zoneDrag.dragPoint = 8;
            g_zoneDrag.dragStartMX = mp.x;
            g_zoneDrag.dragStartMY = mp.y;
            g_zoneDrag.origL = *zL; g_zoneDrag.origT = *zT;
            g_zoneDrag.origR = *zR; g_zoneDrag.origB = *zB;
        }
    }
}

// 退出进程：立即终止本进程。
// 用 _exit 而不走主循环退出+join，避免推理线程卡在 QNN invoke 时 join 阻塞导致无法退出。
static void exitImgui() {
    main_thread_flag = false;
    fflush(stdout);
    _exit(0);
}

static void drawControlPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 580), ImGuiCond_FirstUseEver);
    
    // 使用无标题栏模式，自行绘制标题栏（左侧折叠/展开，右侧保存/删除/退出）
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    bool windowOpen = true;
    ImGui::Begin("花来ai 控制面板", &windowOpen, flags);
    if (!windowOpen) { exitImgui(); return; }
    
    // 自定义标题栏区域
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
    
    // 标题栏左部分：窗口名 + 折叠按钮
    ImGui::Text("花来ai");
    ImGui::SameLine(100);
    if (ImGui::Button(g_panelCollapsed ? "▲" : "▼")) g_panelCollapsed = !g_panelCollapsed;
    
    // 标题栏右部分：保存/删除/退出
    ImGui::SameLine(280);
    if (ImGui::Button("保存")) {
        saveConfig();
        g_cfgLastSaved = g_cfg;
        printf("config saved\n");
    }
    ImGui::SameLine();
    if (ImGui::Button("删除")) {
        // 删除配置文件并恢复默认值
        remove(g_cfgFile.c_str());
        g_cfg = AimConfig(); // 恢复默认
        // 重置其他状态
        g_aimSmoothTrack = -1;
        g_classEnabled.clear();
        syncClassConfig();
        printf("config deleted, reset to defaults\n");
    }
    ImGui::SameLine();
    if (ImGui::Button("退出")) exitImgui();
    
    ImGui::PopStyleColor(3);
    ImGui::Separator();
    
    // 如果折叠，只显示状态信息
    if (g_panelCollapsed) {
        ImGui::Text("后端: %s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "无");
        ImGui::Text("帧源: %llu FPS | 推理: %llu FPS",
                    (unsigned long long)g_srcFps.load(),
                    (unsigned long long)g_inferFps.load());
        ImGui::End();
        return;
    }
    
    // 展开状态下显示完整控制面板
    ImGui::Text("后端: %s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "无");
    ImGui::Text("帧源: %llu FPS | 推理: %llu FPS",
                (unsigned long long)g_srcFps.load(),
                (unsigned long long)g_inferFps.load());
    ImGui::Separator();

    // ===== 推理分类 =====
    if (ImGui::CollapsingHeader("推理", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 模型选择
        if (!g_modelList.empty()) {
            const char* preview = g_modelIndex >= 0 && (size_t)g_modelIndex < g_modelList.size()
                ? strrchr(g_modelList[g_modelIndex].c_str(), '/') + 1
                : g_modelList[0].c_str();
            if (ImGui::BeginCombo("模型", preview)) {
                for (int i = 0; i < (int)g_modelList.size(); ++i) {
                    const char* name = strrchr(g_modelList[i].c_str(), '/') + 1;
                    if (ImGui::Selectable(name, i == g_modelIndex)) {
                        if (i != g_modelIndex) loadModel(i);
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("启用", &g_cfg.enabled);
        ImGui::Checkbox("显示检测框", &g_cfg.showBoxes);
        ImGui::Checkbox("显示帧率", &g_cfg.showFps);
        ImGui::Checkbox("显示裁剪框", &g_cfg.showCropBox);
        ImGui::Checkbox("显示区域", &g_cfg.showZones);
        ImGui::Checkbox("显示连线", &g_cfg.showAimLines);
        ImGui::SliderFloat("置信度阈值", &g_cfg.confidence, 0.05f, 0.95f);

        // 居中裁剪尺寸选择（面板切换后，传给 APK 重建共享内存）
        {
            char previewBuf[32];
            if (CROP_OPTIONS[g_cfg.cropIndex] == 0)
                snprintf(previewBuf, sizeof(previewBuf), "全屏");
            else
                snprintf(previewBuf, sizeof(previewBuf), "%d", CROP_OPTIONS[g_cfg.cropIndex]);
            if (g_shm && g_shm->valid()) {
                auto ci = g_shm->cropInfo();
                if (ci.size == 0)
                    snprintf(previewBuf, sizeof(previewBuf), "全屏 (当前)");
                else if (ci.size > 0)
                    snprintf(previewBuf, sizeof(previewBuf), "%d (当前)", ci.size);
            }
            if (ImGui::BeginCombo("裁剪尺寸", previewBuf)) {
                for (int i = 0; i < (int)(sizeof(CROP_OPTIONS)/sizeof(CROP_OPTIONS[0])); ++i) {
                    char label[32];
                    if (CROP_OPTIONS[i] == 0)
                        snprintf(label, sizeof(label), "全屏");
                    else
                        snprintf(label, sizeof(label), "%d×%d", CROP_OPTIONS[i], CROP_OPTIONS[i]);
                    bool selected = (i == g_cfg.cropIndex);
                    if (ImGui::Selectable(label, selected)) {
                        if (i != g_cfg.cropIndex) {
                            g_cfg.cropIndex = i;
                            int newSize = CROP_OPTIONS[i];
                            printf("[panel] requesting crop size change to %d\n", newSize);
                            if (g_shm && g_shm->valid()) {
                                g_shm->requestCrop(newSize);
                            }
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        // 推理帧率上限（节流：0=不限，可选 60/90/120/144）
        {
            const int FPS_OPTIONS[] = {0, 60, 90, 120, 144};
            char previewBuf[16];
            if (g_cfg.fpsLimit > 0)
                snprintf(previewBuf, sizeof(previewBuf), "%d FPS", g_cfg.fpsLimit);
            else
                snprintf(previewBuf, sizeof(previewBuf), "不限");
            if (ImGui::BeginCombo("推理帧率上限", previewBuf)) {
                for (int f : FPS_OPTIONS) {
                    char label[16];
                    if (f == 0) snprintf(label, sizeof(label), "不限");
                    else snprintf(label, sizeof(label), "%d FPS", f);
                    bool sel = (g_cfg.fpsLimit == f);
                    if (ImGui::Selectable(label, sel)) g_cfg.fpsLimit = f;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        // 检测框描边粗细
        ImGui::SliderInt("框描边", &g_cfg.boxThickness, 1, 4);
        ImGui::Checkbox("框标签", &g_cfg.showBoxLabels);
        // 检测框跟随模式
        ImGui::Checkbox("框直接跟随", &g_cfg.boxDirectFollow);
        if (g_cfg.boxDirectFollow) {
            ImGui::TextDisabled("当前：直接跟随检测位置（最快响应）");
        } else {
            // 检测框速度预判（毫秒）：仅在预测模式下生效
            float boxPredictMs = g_cfg.boxPredictTime * 1000.0f;
            if (ImGui::SliderFloat("框预判(ms)", &boxPredictMs, 0.0f, 200.0f, "%.0f"))
                g_cfg.boxPredictTime = boxPredictMs / 1000.0f;
            ImGui::TextDisabled("当前：速度预测模式（平滑但有延迟）");
        }

        // 类别显示过滤（类名来自模型同目录 labels 文件，如 head/body）
        if (g_engine && g_engineReady && g_engine->getNumClasses() > 0) {
            syncClassConfig();
            ImGui::Text("显示类别:");
            for (int i = 0; i < g_engine->getNumClasses(); ++i) {
                const char* name = g_engine->getClassName(i);
                char label[64];
                if (name && name[0]) snprintf(label, sizeof(label), "%s##c%d", name, i);
                else snprintf(label, sizeof(label), "类别%d##c%d", i, i);
                bool on = g_classEnabled[i];
                if (ImGui::Checkbox(label, &on)) g_classEnabled[i] = on;
                if (i % 2 == 0) ImGui::SameLine();
            }
        }
        if (ImGui::Button("应用置信度")) {
            if (g_engine) g_engine->setConfidence(g_cfg.confidence);
        }
    }
    ImGui::Separator();

    // ===== 自瞄分类 =====
    if (ImGui::CollapsingHeader("自瞄", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 注意：显示标签必须与折叠头区分 —— 若 Checkbox 与 CollapsingHeader("自瞄")
        // 显示文本相同，二者会在 ImGui 内部被当作同一控件（同 label 同 ID），
        // 折叠头与开关互相抢占点击/状态，导致“自瞄开关无法正常关闭”。
        // 显示改为“自瞄开关”，ID 也通过 ##aim 唯一化。
        ImGui::Checkbox("自瞄开关##aim", &g_cfg.aimEnabled);
        // 自瞄算法切换：0=原版(拖拽+平滑) 1=PID 2=贝塞尔
        {
            static int lastMode = -1;
            const char* modes[] = {"PID", "贝塞尔"};
            int cur = g_cfg.aimMode;
            if (cur < 0 || cur > 1) cur = 0;
            if (ImGui::BeginCombo("自瞄算法", modes[cur])) {
                for (int i = 0; i < 2; ++i) {
                    if (ImGui::Selectable(modes[i], i == cur)) g_cfg.aimMode = i;
                    if (i == cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (g_cfg.aimMode != lastMode) {
                if (g_cfg.aimMode == 0) g_pidAim.reset();
                else if (g_cfg.aimMode == 1) g_bezierAim.reset();
                lastMode = g_cfg.aimMode;
            }
        }
        // 自瞄锁定部位（按模型类别名 head/body 识别头部/身体框）
        {
            const char* parts[] = {"锁中心", "锁头部", "锁身体"};
            const char* preview = parts[g_cfg.aimPart];
            if (g_cfg.aimPart < 0 || g_cfg.aimPart > 2) preview = parts[0];
            if (ImGui::BeginCombo("锁定部位", preview)) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(parts[i], i == g_cfg.aimPart)) g_cfg.aimPart = i;
                    if (i == g_cfg.aimPart) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        // 自瞄瞄准点微调 X/Y
        ImGui::SliderFloat("瞄准微调 X", &g_cfg.aimOffsetX, -0.05f, 0.05f, "%.4f");
        ImGui::SliderFloat("瞄准微调 Y", &g_cfg.aimOffsetY, -0.05f, 0.05f, "%.4f");
    // 自瞄类别锁定（aimClass: -1=全部, >=0=仅锁定该类）
    {
        char clsBuf[64];
        const char* preview = "全部";
        if (g_engine && g_engineReady.load() && g_cfg.aimClass >= 0 &&
            g_cfg.aimClass < g_engine->getNumClasses()) {
            const char* nm = g_engine->getClassName(g_cfg.aimClass);
            if (nm && nm[0]) { snprintf(clsBuf, sizeof(clsBuf), "%s", nm); preview = clsBuf; }
        }
        if (ImGui::BeginCombo("自瞄类别", preview)) {
            if (ImGui::Selectable("全部", g_cfg.aimClass < 0)) g_cfg.aimClass = -1;
            if (g_engine && g_engineReady.load()) {
                for (int i = 0; i < g_engine->getNumClasses(); ++i) {
                    const char* nm = g_engine->getClassName(i);
                    char lbl[64];
                    if (nm && nm[0]) snprintf(lbl, sizeof(lbl), "%s##ac%d", nm, i);
                    else snprintf(lbl, sizeof(lbl), "类别%d##ac%d", i, i);
                    bool sel = (g_cfg.aimClass == i);
                    if (ImGui::Selectable(lbl, sel)) g_cfg.aimClass = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    // 锁定优先级（多目标时如何选择）
    {
        const char* modes[] = {"最近中心", "最大框", "最接近准星"};
        int cur = g_cfg.selectMode;
        if (cur < 0 || cur > 2) cur = 0;
        if (ImGui::BeginCombo("锁定优先级", modes[cur])) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::Selectable(modes[i], i == cur)) g_cfg.selectMode = i;
                if (i == cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SliderFloat("死区", &g_cfg.deadZone, 0.005f, 0.1f);
    ImGui::SliderFloat("X 平滑", &g_cfg.smoothX, 0.0f, 0.95f);
    ImGui::SliderFloat("Y 平滑", &g_cfg.smoothY, 0.0f, 0.95f);
    ImGui::SliderFloat("瞄准点平滑", &g_cfg.aimPointSmooth, 0.0f, 0.95f);
    ImGui::SliderFloat("自瞄速度", &g_cfg.aimSpeed, 0.1f, 3.0f);
    ImGui::SliderFloat("预判", &g_cfg.predictGain, 0.0f, 0.2f);
    // 自瞄回正速度（归一化每帧最大瞄准点移动距离）：目标在裁剪框边缘时防止准星甩飞
    ImGui::SliderFloat("回正速度", &g_cfg.aimApproachSpeed, 0.0f, 0.1f, "%.4f");
    // 算法相关参数：仅在对应模式激活时显示
    if (g_cfg.aimMode == 0) {
        ImGui::Text("PID 参数");
        ImGui::SliderFloat("P##pidKp", &g_cfg.pidKp, 0.0f, 0.050f);
        ImGui::SliderFloat("I##pidKi", &g_cfg.pidKi, 0.0f, 0.020f);
        ImGui::SliderFloat("D##pidKd", &g_cfg.pidKd, 0.0f, 0.010f);
        ImGui::SliderFloat("采样周期(ms)", &g_cfg.pidSamplePeriodMs, 1.0f, 50.0f);
    }
    if (g_cfg.aimMode == 1) {
        ImGui::Text("贝塞尔参数");
        ImGui::SliderFloat("时长系数", &g_cfg.bezierDuration, 5.0f, 100.0f);
    }
    // PID/贝塞尔共用参数
    if (g_cfg.aimMode == 0 || g_cfg.aimMode == 1) {
        ImGui::SliderFloat("收敛阈值(px)", &g_cfg.convergeThresh, 1.0f, 60.0f);
        ImGui::SliderFloat("移动平滑", &g_cfg.aimMoveSmooth, 0.0f, 0.95f);
    }
    // 压枪（并入自瞄分类）：检测物理手指按在“开枪区”时视角自动下拉补偿后坐力
    ImGui::Separator();
    ImGui::Checkbox("压枪", &g_cfg.recoilEnabled);
    ImGui::TextDisabled("物理手指按在“开枪区”时视角自动下拉补偿后坐力，独立于扳机");
    ImGui::SliderInt("压枪开始时间(ms)", &g_cfg.recoilStartMs, 0, 2000, "%d");
    // 压枪力度按模式切换：uinput 用 px/s，陀螺仪用 °/s
    if (g_cfg.gyroAim)
        ImGui::SliderFloat("压枪力度(°/s)", &g_cfg.recoilDegPerSec, 0.0f, 500.0f, "%.1f");
    else
        ImGui::SliderInt("压枪力度(px/s)", &g_cfg.recoilStrength, 0, 2000, "%d");
    if (g_cfg.recoilStrength < 0) g_cfg.recoilStrength = 0;
    // 陀螺仪自瞄参数：勾选“内核陀螺仪”后，自瞄转向按角度注入
    ImGui::Separator();
    ImGui::TextDisabled("陀螺仪自瞄参数（内核陀螺仪模式下生效）");
    ImGui::SliderFloat("陀螺仪灵敏度", &g_cfg.gyroSens, 0.0f, 2.0f, "%.3f");
    ImGui::SliderFloat("单帧最大角度(°)", &g_cfg.gyroMaxDeg, 0.0f, 500.0f, "%.1f");
    ImGui::Checkbox("反转 Pitch", &g_cfg.gyroInvertPitch);
    ImGui::Checkbox("反转 Yaw", &g_cfg.gyroInvertYaw);
    }
    ImGui::Separator();

    // ===== 区域分类 =====
    if (ImGui::CollapsingHeader("区域")) {
        // 区域拖拽编辑：在悬浮窗上直接拖动控制点调整区域大小和位置（扳机区改为拖拽调整）
        ImGui::Text("区域编辑(拖拽调整)");
        {
            const char* zmodes[] = {"关闭", "触控区", "触发区(扳机)", "自瞄触发区", "倍镜区"};
            int cur = g_cfg.zoneEditTarget;
            if (cur < 0 || cur > 4) cur = 0;
            if (ImGui::BeginCombo("编辑区域", zmodes[cur])) {
                for (int i = 0; i < 5; ++i) {
                    if (ImGui::Selectable(zmodes[i], i == cur)) g_cfg.zoneEditTarget = i;
                    if (i == cur) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (g_cfg.zoneEditTarget != 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f),
                                   "拖动控制点调整大小/位置，编辑中自瞄/扳机暂停");
            }
        }
        // 触控区：自瞄拖拽注入区域（与游戏转向/瞄准区对齐）
        ImGui::Text("触控区(自瞄拖拽)");
        ImGui::SliderFloat("触控左##tzL", &g_cfg.touchZoneL, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("触控上##tzT", &g_cfg.touchZoneT, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("触控右##tzR", &g_cfg.touchZoneR, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("触控下##tzB", &g_cfg.touchZoneB, 0.0f, 1.0f, "%.2f");
        // 自瞄触发区/倍镜区：开启后需点击对应区域才触发自瞄
        ImGui::Checkbox("自瞄触发区", &g_cfg.aimTriggerZoneEnabled);
        ImGui::Checkbox("倍镜区", &g_cfg.adsZoneEnabled);
    }
    ImGui::Separator();

    // ===== 扳机分类 =====
    if (ImGui::CollapsingHeader("扳机")) {
        // 显示标签与折叠头区分（“扳机开关”），并加 ##trigger 唯一 ID：
        // 避免与 CollapsingHeader("扳机") 同 label 被视为同一控件，导致开关无法勾选。
        ImGui::Checkbox("扳机开关##trigger", &g_cfg.triggerEnabled);
        ImGui::SliderFloat("扳机灵敏度", &g_cfg.triggerSensitivity, 0.1f, 1.0f);
        ImGui::Checkbox("扳机按住", &g_cfg.triggerHold);
        ImGui::SliderInt("点射间隔(ms)", &g_cfg.triggerCooldownMs, 0, 500);
        // 扳机随机延迟：目标进入触发区后延迟 50~300ms 内随机值再开火（防机械感/防检）
        ImGui::SliderInt("延迟下限(ms)", &g_cfg.triggerDelayMin, 0, 300);
        ImGui::SliderInt("延迟上限(ms)", &g_cfg.triggerDelayMax, 0, 300);
        if (g_cfg.triggerDelayMax < g_cfg.triggerDelayMin)
            g_cfg.triggerDelayMax = g_cfg.triggerDelayMin;
    }
    ImGui::Separator();

    // ===== 触摸注入 =====
    if (ImGui::CollapsingHeader("注入", ImGuiTreeNodeFlags_DefaultOpen)) {
        // 内核陀螺仪模式：勾选后自瞄/压枪走内核陀螺仪（uinput 保持初始化，屏幕可注入触摸），扳机禁用
        if (ImGui::Checkbox("内核陀螺仪", &g_cfg.gyroAim)) applyGyroMode(g_cfg.gyroAim);
        if (touch_kernel_connected())
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f),
                               "驱动已连接 v%u", touch_kernel_version());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "驱动未连接");
        ImGui::TextDisabled(g_cfg.gyroAim && touch_kernel_connected()
            ? "陀螺仪模式：自瞄/压枪走内核陀螺仪，uinput 保持初始化，扳机禁用"
            : (g_cfg.gyroAim
                ? "陀螺仪驱动未连接，回退 uinput 注入（自瞄/扳机仍走 uinput）"
                : "触摸模式：注入统一走 uinput"));
        ImGui::Separator();

        if (g_cfg.gyroAim && !touch_kernel_connected()) {
            // 陀螺仪勾选但驱动未能连接：显示回退提示，uinput 仍可用
            ImGui::TextDisabled("内核陀螺仪未连接，已回退 uinput（驱动加载/版本不匹配时请检查）");
        }
        {
            // uinput 注入初始化状态：始终初始化（陀螺仪模式也保留，屏幕才可注入触摸）
            bool injReady = injectReady();
            if (injReady)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "触摸已初始化");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "触摸未初始化");
            if (!injReady) {
                if (ImGui::Button("初始化触摸")) {
                    if (touch_inject_init())
                        printf("touch injection initialized\n");
                    else
                        fprintf(stderr, "touch injection init failed\n");
                }
            } else {
                if (ImGui::Button("停止触摸")) {
                    g_aimFingerDown = false;
                    g_triggerDown = false;
                    touch_inject_close();
                }
            }
        }
        ImGui::Separator();
        // 内核触摸（TimeDriver）：勾选后自瞄/压枪/扳机的虚拟注入走 TimeDriver 内核驱动，
        // 不再依赖 uinput 的虚拟触摸设备；物理触摸不受影响（Touch_Init 后立即 Touch_Disable）。
        if (ImGui::Checkbox("内核触摸", &g_cfg.kernelTouch))
            applyKernelTouchMode(g_cfg.kernelTouch);
        ImGui::TextDisabled(g_cfg.kernelTouch && kdrv_connected()
            ? "内核触摸模式：自瞄/压枪/扳机走 TimeDriver 注入，物理触摸正常"
            : (g_cfg.kernelTouch
                ? "内核触摸驱动未连接，回退 uinput 注入"
                : "触摸模式：注入统一走 uinput"));

        // 触摸诊断：reader 是否读到物理手指（排查“无法触控屏幕/ImGui”）
        {
            static int diagFrame = 0;
            static bool diagHasFinger = false;
            static int diagX = 0, diagY = 0;
            static bool diagDown = false;
            if (g_touchReady && (++diagFrame & 7) == 0) {
                int mx = 0, my = 0; bool dn = false;
                diagHasFinger = touch_get_primary_finger(&mx, &my, &dn);
                diagX = mx; diagY = my; diagDown = dn;
            }
            ImGui::TextDisabled("触摸诊断: 设备%d 主手指:%s (%d,%d)%s",
                                g_touchReady ? touch_device_count() : -1,
                                diagHasFinger ? "有" : "无", diagX, diagY,
                                diagDown ? " 按下" : "");
        }
    }
    ImGui::End();
}

// 折叠后的小状态框：可拖动，点击/按钮展开回控制面板
static void drawMiniPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("花来ai", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    // 展开 + 退出 并列，便于折叠状态下也能快速退出
    if (ImGui::Button("展开 ▶")) g_panelCollapsed = false;
    ImGui::SameLine();
    if (ImGui::Button("退出")) {
        exitImgui();
    }
    ImGui::Text("后端: %s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "无");
    ImGui::Text("推理: %llu FPS", (unsigned long long)g_inferFps.load());
    ImGui::End();
}

// 卡密登录窗口（定义见下方 t3auth 辅助函数之后）
static void drawLoginWindow();

// 模板要求的 UI 回调
void Layout_tick_UI() {
    // 未验证通过：只显示卡密登录窗口，隐藏控制面板 / 推理 / 检测框 / 区域编辑
    if (!g_t3Verified.load()) {
        drawLoginWindow();
        return;
    }
    if (g_panelCollapsed) drawMiniPanel();
    else drawControlPanel();
    drawDetectionOverlay();
    drawZoneEditor();
}

// ---------------------------------------------------------------------------
// T3 卡密验证（防破解）
// 与 APK 侧使用同一套调用码 + APPKEY + RSA 公钥（见 t3auth.cpp），验证同一张卡密。
// 在悬浮窗提供 输入框 + 粘贴 + 登录，验证通过后才启用推理功能。
// 防破解双保险：即使 APK 被绕过，native 也会在悬浮窗独立要求登录。
// ---------------------------------------------------------------------------

static void t3auth_set_message(const std::string& m) {
    std::lock_guard<std::mutex> lk(g_t3MsgMutex);
    g_t3Message = m;
}
static std::string t3auth_get_message() {
    std::lock_guard<std::mutex> lk(g_t3MsgMutex);
    return g_t3Message;
}

// 同步登录：成功返回 true 并输出 statecode；失败返回 false 并输出错误信息
static bool t3auth_try_login(const std::string& card, std::string& statecodeOut, std::string& errOut) {
    std::string imei = t3auth_machine_code();
    printf("[T3验证] 正在验证卡密，机器码=%s ...\n", imei.c_str());
    fflush(stdout);
    T3AuthResult r = t3auth_login(card, imei);
    if (!r.ok) {
        errOut = r.error.empty() ? "未知错误" : r.error;
        printf("[T3验证] 卡密验证失败: %s\n", errOut.c_str());
        fflush(stdout);
        return false;
    }
    statecodeOut = r.statecode;
    printf("[T3验证] 卡密验证通过\n");
    fflush(stdout);
    return true;
}

// 异步登录（悬浮窗“登录”按钮调用）：成功后置 g_t3Verified 并启动心跳
static void t3auth_do_login_async(const std::string& card) {
    if (g_t3LoggingIn.exchange(true)) return;  // 已在登录中则忽略本次
    t3auth_set_message("正在验证卡密...");
    std::thread([card]() {
        std::string statecode, err;
        if (t3auth_try_login(card, statecode, err)) {
            g_t3Card = card;
            g_t3Statecode = statecode;
            g_t3Verified.store(true);
            t3auth_start_heartbeat(card, statecode);
            t3auth_set_message("验证通过，推理功能已启用");
            printf("[T3验证] 悬浮窗登录成功，已启动心跳保活\n");
        } else {
            t3auth_set_message("验证失败: " + err);
        }
        g_t3LoggingIn.store(false);
    }).detach();
}

// 通过 root shell 读取 Android 剪贴板（用于“粘贴”按钮）
static std::string t3auth_read_clipboard() {
    FILE* p = popen("cmd clipboard get-primary-clip 2>/dev/null", "r");
    if (!p) return "";
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    // 命令失败或输出是错误提示时视为无法读取
    if (rc != 0 || out.empty()) return "";
    if (out.find("Denial") != std::string::npos ||
        out.find("denial") != std::string::npos ||
        out.find("Exception") != std::string::npos ||
        out.find("exception") != std::string::npos) return "";
    // 去除首尾空白/换行
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
    return out;
}

// 卡密登录窗口：未验证时在悬浮窗显示，提供 输入框 + 粘贴 + 登录
static void drawLoginWindow() {
    ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 230), ImGuiCond_FirstUseEver);
    ImGui::Begin("花来ai 卡密验证", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextWrapped("请输入卡密登录后，才能启用推理功能");
    ImGui::TextWrapped("（与 APK 同一套卡密验证，防止破解）");
    ImGui::Separator();

    ImGui::InputText("卡密", g_t3InputBuf, sizeof(g_t3InputBuf));

    if (ImGui::Button("粘贴")) {
        std::string clip = t3auth_read_clipboard();
        if (!clip.empty()) {
            snprintf(g_t3InputBuf, sizeof(g_t3InputBuf), "%s", clip.c_str());
            t3auth_set_message("已粘贴，请点击登录");
        } else {
            t3auth_set_message("剪贴板为空或无法读取");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("登录", ImVec2(160, 0))) {
        std::string card(g_t3InputBuf);
        // 去除首尾空白
        while (!card.empty() && (card.back() == '\r' || card.back() == '\n' || card.back() == ' '))
            card.pop_back();
        while (!card.empty() && (card.front() == ' ' || card.front() == '\t'))
            card.erase(card.begin());
        if (card.empty()) {
            t3auth_set_message("请输入卡密");
        } else {
            t3auth_do_login_async(card);
        }
    }

    std::string msg = t3auth_get_message();
    if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());

    ImGui::End();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 参数：model_path shm_path [workdir] [card]
    if (argc < 3) {
        printf("Usage: %s <model.tflite> <shm_path> [workdir] [card]\n", argv[0]);
        return -1;
    }
    const char* modelPath = argv[1];
    const char* shmPath = argv[2];
    if (argc >= 4) chdir(argv[3]);

    // 卡密验证（防破解）：
    // 启动时一律不自动验证，强制在悬浮窗内手动输入卡密点击“登录”。
    // APK 传入的 argv[4] 或工作目录 .t3card 仅用于预填输入框，绝不自动放行；
    // 只有悬浮窗内验证通过后 g_t3Verified 才置 true，推理/自瞄功能才启用。
    {
        std::string card;
        if (argc >= 5 && argv[4] && argv[4][0]) {
            card = argv[4];
        } else {
            std::ifstream ifs(".t3card");
            if (ifs.is_open()) std::getline(ifs, card);
        }
        // 去除首尾空白
        while (!card.empty() && (card.back() == '\r' || card.back() == '\n' || card.back() == ' '))
            card.pop_back();
        while (!card.empty() && (card.front() == ' ' || card.front() == '\t'))
            card.erase(card.begin());

        if (!card.empty()) {
            // 仅预填输入框，让用户确认后点击“登录”
            snprintf(g_t3InputBuf, sizeof(g_t3InputBuf), "%s", card.c_str());
            printf("[T3验证] 已预填卡密到输入框，等待悬浮窗手动登录\n");
        }
        t3auth_set_message("请输入卡密并点击登录，验证通过后才启用推理");
    }

    // 清理 /data/local/tmp 下的 log 文件和 kaixin.com 文件夹
    {
        system("rm -f /data/local/tmp/*.log 2>/dev/null");
        system("rm -rf /data/local/tmp/kaixin.com 2>/dev/null");
    }

    // 加载上次保存的配置（工作目录下 yolotouch_cfg.bin），失败则用默认值
    g_cfgFile = std::string(argv[3] ? argv[3] : ".") + "/yolotouch_cfg.bin";
    if (loadConfig()) {
        printf("config loaded from %s\n", g_cfgFile.c_str());
        // 加载后钳制数组索引（模型/裁剪/算法等），防止越界
        int cropN = (int)(sizeof(CROP_OPTIONS) / sizeof(CROP_OPTIONS[0]));
        if (g_cfg.cropIndex < 0 || g_cfg.cropIndex >= cropN) g_cfg.cropIndex = 6;
        if (g_cfg.aimMode < 0 || g_cfg.aimMode > 1) g_cfg.aimMode = 0;
        if (g_cfg.aimPart < 0 || g_cfg.aimPart > 2) g_cfg.aimPart = 0;
        if (g_cfg.selectMode < 0 || g_cfg.selectMode > 2) g_cfg.selectMode = 0;
        if (g_cfg.zoneEditTarget < 0 || g_cfg.zoneEditTarget > 4) g_cfg.zoneEditTarget = 0;
    } else {
        printf("no saved config, using defaults\n");
    }
    g_cfgLastSaved = g_cfg;

    // 扫描模型目录（模型文件所在目录），供面板切换
    {
        std::string modelDir = modelPath;
        size_t slash = modelDir.find_last_of('/');
        if (slash != std::string::npos) modelDir = modelDir.substr(0, slash);
        else modelDir = ".";
        scanModels(modelDir.c_str());
    }

    // 屏幕信息
    screen_config();
    ::abs_ScreenX = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    ::abs_ScreenY = (displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width);
    ::native_window_screen_x = displayInfo.width;
    ::native_window_screen_y = displayInfo.height;
    g_rotation = displayInfo.orientation;

    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
        fprintf(stderr, "initGUI_draw failed\n");
        return -1;
    }

    // 初始化触摸。触摸注入统一走 uinput：
    // touch_init 只扫描设备/准备坐标系并启动 reader（供 ImGui 交互、区域判断），
    // 不创建 uinput 注入设备；uinput 模式按需初始化注入设备，陀螺仪模式屏蔽 uinput 初始化。
    g_touchReady = touch_init(native_window_screen_x, native_window_screen_y);
    // 屏幕参数（含旋转角）无条件同步给 touch_core：内核陀螺仪注入的 orientation
    // 依赖它，即使 touch_init 失败/未启动 reader 也不能缺失，否则陀螺仪注入方向错乱/无效果。
    touch_set_screen_params(native_window_screen_x, native_window_screen_y, g_rotation);
    if (g_touchReady) {
        // 初始同步触发区（默认右下角区域）
        int fzL = (int)(g_cfg.fireZoneL * native_window_screen_x);
        int fzT = (int)(g_cfg.fireZoneT * native_window_screen_y);
        int fzR = (int)(g_cfg.fireZoneR * native_window_screen_x);
        int fzB = (int)(g_cfg.fireZoneB * native_window_screen_y);
        touch_set_fire_zone(fzL, fzT, fzR, fzB);
        touch_start_readers();
        // uinput 注入设备始终初始化：自瞄/扳机/压枪走 uinput 或陀螺仪取决于模式，
        // 但 uinput 必须保持初始化，否则游戏屏幕不可被注入触摸（imgui 交互不受影响）。
        touch_inject_init();
        printf("touch injection ready (gyro=%d)\n", (int)g_cfg.gyroAim);
    } else {
        fprintf(stderr, "touch_init failed (need root + /dev/input)\n");
    }

    // 配置里若已开启内核陀螺仪模式，则启动时直接应用（连接驱动 + 屏蔽 uinput 初始化）
    if (g_cfg.gyroAim) applyGyroMode(true);

    // 配置里若已开启内核触摸模式，则启动时直接应用（连接 TimeDriver + 初始化虚拟触摸设备）
    if (g_cfg.kernelTouch) applyKernelTouchMode(true);

    // 初始化推理引擎
    auto engine = std::make_unique<LiteRtEngine>();
    engine->setConfidence(g_cfg.confidence);
    bool engOk = engine->init(modelPath);
    g_engine = engine.get();
    g_engineReady.store(engOk);
    syncClassConfig();

    // 若默认模型在扫描列表中，记录其索引（供面板高亮）
    {
        std::string target = modelPath;
        for (size_t i = 0; i < g_modelList.size(); ++i) {
            if (g_modelList[i] == target) { g_modelIndex = (int)i; break; }
        }
    }

    if (!engOk) {
        fprintf(stderr, "engine init failed: %s\n", modelPath);
    } else {
        printf("engine ready, backend=%s\n", engine->getBackendType().c_str());
    }

    // 打开共享内存
    g_shm = ShmFrameReader::open(shmPath, 10000);
    g_shmReady.store(g_shm != nullptr);
    if (g_shm && g_shm->valid()) {
        g_shmWidth = (int)g_shm->header()->width;
        g_shmHeight = (int)g_shm->header()->height;
        g_rotation = (int)g_shm->header()->rotation;
        // 共享内存打开后屏幕真实旋转角才确定（可能与 displayInfo.orientation 不同）。
        // 必须重新同步给 touch_core：内核陀螺仪注入的 orientation、以及 uinput 触摸
        // 的旋转换算都依赖它。若不同步，陀螺仪自瞄注入方向错乱/无效果。
        touch_set_screen_params(native_window_screen_x, native_window_screen_y, g_rotation);
    }

    // 启动推理线程
    g_inferRunning.store(true);
    std::thread inferThread(inferenceLoop);

    // 主循环
    bool lastFingerDown = false;
    long long lastWatchdogLog = getTimeNowMs();
    long long lastCropResendMs = getTimeNowMs();
    while (main_thread_flag) {
        // 喂入触摸输入：把真实物理手指坐标转成 ImGui 鼠标输入，
        // 使悬浮窗（SurfaceFlinger 直接创建，不经系统 InputDispatcher）可交互。
        // 触摸仍会正常透传给前台应用（游戏），此处仅读取用于 UI 操作。
        if (g_touchReady) {
            ImGuiIO& io = ImGui::GetIO();
            int mx = 0, my = 0;
            bool down = false;
            bool hasFinger = touch_get_primary_finger(&mx, &my, &down);
            if (hasFinger) {
                io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                io.AddMousePosEvent((float)mx, (float)my);
                if (down != lastFingerDown)
                    io.AddMouseButtonEvent(0, down);
            } else if (lastFingerDown) {
                io.AddMouseButtonEvent(0, false);
            }
            lastFingerDown = down;
        }

        // 看门狗：推理线程连续 10s 未完成一帧 → 写入日志
        long long now = getTimeNowMs();
        long long lastDone = g_lastFrameDoneMs.load();
        if (lastDone > 0 && now - lastDone > 10000 && now - lastWatchdogLog > 10000) {
            lastWatchdogLog = now;
            fprintf(stderr, "[WATCHDOG] inference thread no frame for 10s! "
                    "engine=%s processed=%llu lastFrameMs=%lld shmDiag=%s\n",
                    g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "?",
                    (unsigned long long)g_frameCount.load(),
                    (long long)g_lastFrameMs.load(),
                    g_shm && g_shm->valid() ? g_shm->diag().c_str() : "no-shm");
        }

        // 裁剪尺寸自愈：面板选定的目标与 APK 实际应用值不一致时，每 200ms 重发
        // 裁剪请求。修复“切到全屏后无法切回其他尺寸”的锁死——cropRequest 是一次性
        // 消息，可能被 APK 写头部覆盖丢失；这里周期性对账并自动补发，直到 APK
        // 应用为止，不依赖用户再次点击同一项。
        if (g_shm && g_shm->valid()) {
            long long now = getTimeNowMs();
            if (now - lastCropResendMs >= 200) {
                lastCropResendMs = now;
                int target = CROP_OPTIONS[g_cfg.cropIndex];
                int actual = (int)g_shm->readHeader().cropSize;
                if (actual != target) {
                    g_shm->requestCrop(target);
                }
            }
        }

        // ── 陀螺仪平滑注入（主线程）──
        // 推理线程每帧（~50ms）写入目标 pitch/yaw 到 g_gyroTarget*
        // 主线程每帧（~1ms）用指数移动平均（EMA）平滑插值后注入
        {
            std::lock_guard<std::mutex> lock(g_gyroMutex);
            if (g_gyroTargetActive && g_cfg.gyroAim && touch_kernel_connected() && g_t3Verified.load()) {
                float sens = g_cfg.gyroSens;
                float baseAlpha = 0.20f - sens * 0.08f;
                if (baseAlpha < 0.02f) baseAlpha = 0.02f;

                // 分离 Pitch 和 Yaw 的 alpha：
                // - Yaw（移动跟踪）：更高 alpha，快速响应人物移动
                // - Pitch（压枪）：检测到压枪时更高 alpha
                float pitchAlpha = baseAlpha;
                float yawAlpha = baseAlpha;

                // 方向反转时：使用高 alpha 快速收敛
                if (g_gyroTargetDirReversed) {
                    pitchAlpha = 0.65f;
                    yawAlpha = 0.65f;
                    g_gyroTargetDirReversed = false;
                }

                // 检测压枪：目标 Pitch 比平滑 Pitch 低很多（压枪下拉）
                float pitchDiff = g_gyroTargetPitch - g_gyroSmoothPitch;
                if (pitchDiff < -1.0f) {  // 目标下拉超过 1°
                    // 压枪时使用更高 alpha，快速跟随后坐力
                    pitchAlpha = std::max(pitchAlpha, 0.55f);
                }

                // 检测快速移动：Yaw 目标变化大
                float yawDiff = g_gyroTargetYaw - g_gyroSmoothYaw;
                if (std::fabs(yawDiff) > 2.0f) {  // Yaw 变化超过 2°
                    // 移动时使用更高 alpha，跟上人物移动
                    yawAlpha = std::max(yawAlpha, 0.50f);
                }
                
                g_gyroSmoothPitch += pitchAlpha * pitchDiff;
                g_gyroSmoothYaw   += yawAlpha * yawDiff;
                touch_gyro_apply(true, g_gyroSmoothPitch, g_gyroSmoothYaw);
            } else if (!g_gyroTargetActive) {
                g_gyroSmoothPitch *= 0.85f;
                g_gyroSmoothYaw   *= 0.85f;
                g_gyroTargetDirReversed = false;
                float mag = std::sqrt(g_gyroSmoothPitch*g_gyroSmoothPitch
                                    + g_gyroSmoothYaw*g_gyroSmoothYaw);
                if (mag < 0.01f) {
                    touch_gyro_stop();
                    g_gyroSmoothPitch = 0.0f;
                    g_gyroSmoothYaw   = 0.0f;
                } else {
                    touch_gyro_apply(true, g_gyroSmoothPitch, g_gyroSmoothYaw);
                }
            }
        }

        drawBegin();
        Layout_tick_UI();
        drawEnd();

        // 配置自动保存：检测到配置变化（含区域拖拽）时写入文件，避免每次重进重新调
        if (memcmp(&g_cfg, &g_cfgLastSaved, sizeof(g_cfg)) != 0) {
            g_cfgLastSaved = g_cfg;
            saveConfig();
        }

        std::this_thread::sleep_for(1ms);
    }

    // 清理
    g_inferRunning.store(false);
    if (inferThread.joinable()) inferThread.join();
    // 退出前确保陀螺仪停止
    {
        std::lock_guard<std::mutex> lock(g_gyroMutex);
        g_gyroTargetActive = false;
        g_gyroSmoothPitch = 0.0f;
        g_gyroSmoothYaw   = 0.0f;
        g_gyroLastTargetPitch = 0.0f;
        g_gyroLastTargetYaw   = 0.0f;
        g_gyroTargetDirReversed = false;
        touch_gyro_stop();
    }
    if (g_touchReady) touch_close();
    if (g_engine) g_engine->release();
    delete g_shm;
    shutdown();
    return 0;
}
