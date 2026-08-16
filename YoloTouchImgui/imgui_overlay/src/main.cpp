#include "Android_draw/draw.h"

#include "aim_types.h"
#include "kalman_tracker.h"
#include "aim_controller.h"
#include "trigger_controller.h"
#include "shared_mem_client.h"
#include "inference/inference_engine.h"
#include "inference/litert_engine.h"
#include "injection/touch_core.h"

#include <atomic>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
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

// 录屏帧尺寸（从共享内存头读取，供绘制坐标缩放）
static int g_shmWidth = 0;
static int g_shmHeight = 0;
static int g_rotation = 0;

// 触摸注入
static bool g_touchReady = false;

// 共享内存
static ShmFrameReader* g_shm = nullptr;
static std::atomic<bool> g_shmReady{false};

// 推理结果（跨线程）
static std::mutex g_detMutex;
static std::vector<Detection> g_detections;
static std::vector<AimTarget> g_tracks;
static std::atomic<uint64_t> g_inferFps{0};
static std::atomic<uint64_t> g_frameCount{0};

// 控制配置
static AimConfig g_cfg;
static bool g_aimActive = false;
static float g_aimX = 0.5f, g_aimY = 0.5f;

// 自瞄/触发控制器
static KalmanTracker g_tracker;
static AimController g_aim;
static TriggerController g_trigger;

// ---------------------------------------------------------------------------
// 时间工具
// ---------------------------------------------------------------------------
static long long getTimeNowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// ---------------------------------------------------------------------------
// 单帧处理：推理 + 跟踪 + 自瞄 + 触发 + 注入
// ---------------------------------------------------------------------------
static void processFrame(const uint8_t* frame, const ShmFrameHeader* h) {
    const int w = (int)h->width;
    const int hh = (int)h->height;
    if (w <= 0 || hh <= 0) return;
    if (!g_engine) return;

    auto dets = g_engine->detect(
        const_cast<uint8_t*>(frame), 0, 0, w, hh, w, hh,
        (int)h->rowStride, (int)h->pixelStride);

    // 更新检测结果（供绘制）
    {
        std::lock_guard<std::mutex> lock(g_detMutex);
        g_detections = dets;
    }
    g_frameCount++;

    if (!g_cfg.enabled) return;

    // 转为 AimTarget 并跟踪
    std::vector<AimTarget> targets;
    for (const auto& d : dets) {
        AimTarget t;
        t.x1 = d.x1; t.y1 = d.y1; t.x2 = d.x2; t.y2 = d.y2;
        t.score = d.score; t.classId = (int)d.classId;
        t.cx = (d.x1 + d.x2) * 0.5f;
        t.cy = (d.y1 + d.y2) * 0.5f;
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
        g_aimActive = false;
        return;
    }

    // 选择目标
    const AimTarget* pick = nullptr;
    float bestScore = -1.0f;
    float screenCx = 0.5f, screenCy = 0.5f;
    for (const auto& t : tracks) {
        float score;
        float dx = t.cx - screenCx;
        float dy = t.cy - screenCy;
        switch (g_cfg.selectMode) {
        case 1: score = (t.x2 - t.x1) * (t.y2 - t.y1); break;  // 最大框
        case 2: score = -std::sqrt(dx*dx + dy*dy); break;        // 最接近准星
        default: score = -std::sqrt(dx*dx + dy*dy); break;       // 最近中心
        }
        if (score > bestScore) { bestScore = score; pick = &t; }
    }
    if (!pick) { g_aimActive = false; return; }

    // 触摸注入（uinput）
    if (g_touchReady && g_cfg.aimEnabled && g_cfg.enabled) {
        int screenW = g_shmWidth > 0 ? g_shmWidth : native_window_screen_x;
        int screenH = g_shmHeight > 0 ? g_shmHeight : native_window_screen_y;

        float aimX = (float)native_window_screen_x / 2.0f;
        float aimY = (float)native_window_screen_y / 2.0f;
        float tx = pick->cx * screenW;
        float ty = pick->cy * screenH;

        // 相对移动：移动到目标位置（模拟拖动视角）
        // 通过 touch_move 平滑移动
        touch_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID, (int)aimX, (int)aimY);
        touch_move(TOUCH_VIRTUAL_SLOT, (int)tx, (int)ty);
        touch_up(TOUCH_VIRTUAL_SLOT);
        g_aimActive = true;
        g_aimX = pick->cx;
        g_aimY = pick->cy;
    }

    // 触发
    if (g_touchReady && g_cfg.triggerEnabled && g_cfg.enabled) {
        bool fire = false, hold = false;
        bool fingerDown = touch_is_finger_in_fire_zone();
        g_trigger.update(*pick, g_cfg, 0.5f, 0.5f, fingerDown, fire, hold);
        if (fire) {
            touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID,
                       native_window_screen_x / 2, native_window_screen_y / 2);
            touch_up(TOUCH_TRIGGER_SLOT);
        } else if (hold) {
            touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID,
                       native_window_screen_x / 2, native_window_screen_y / 2);
        }
    }
}

// ---------------------------------------------------------------------------
// 推理线程
// ---------------------------------------------------------------------------
static std::atomic<bool> g_inferRunning{false};

static void inferenceLoop() {
    uint64_t frames = 0;
    long long lastFpsMs = getTimeNowMs();

    while (g_inferRunning.load()) {
        if (!g_shm || !g_shm->valid()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        const uint8_t* frame = g_shm->readFrame();
        if (!frame) {
            std::this_thread::sleep_for(5ms);
            continue;
        }
        processFrame(frame, g_shm->header());
        frames++;

        // FPS 统计
        long long now = getTimeNowMs();
        if (now - lastFpsMs >= 1000) {
            g_inferFps.store(frames * 1000 / (now - lastFpsMs));
            frames = 0;
            lastFpsMs = now;
        }
    }
}

// ---------------------------------------------------------------------------
// UI 绘制
// ---------------------------------------------------------------------------
static void drawDetectionOverlay() {
    if (!g_cfg.showBoxes && !g_cfg.showFps) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    float sx = native_window_screen_x;
    float sy = native_window_screen_y;

    if (g_cfg.showFps) {
        char buf[64];
        snprintf(buf, sizeof(buf), "FPS: %llu  Det: %zu  Track: %zu",
                 (unsigned long long)g_inferFps.load(),
                 g_detections.size(), g_tracks.size());
        draw->AddText(ImVec2(20, 20), IM_COL32(255, 255, 0, 255), buf);
    }

    std::lock_guard<std::mutex> lock(g_detMutex);
    for (const auto& d : g_detections) {
        if (!g_cfg.showBoxes) break;
        ImVec2 p1(d.x1 * sx, d.y1 * sy);
        ImVec2 p2(d.x2 * sx, d.y2 * sy);
        draw->AddRect(p1, p2, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%.2f", d.score);
        draw->AddText(p1, IM_COL32(0, 255, 0, 255), lbl);
    }

    // 准星
    if (g_cfg.aimEnabled) {
        float cx = sx * 0.5f, cy = sy * 0.5f;
        int col = g_aimActive ? IM_COL32(255, 0, 0, 255) : IM_COL32(255, 255, 255, 120);
        draw->AddLine(ImVec2(cx - 20, cy), ImVec2(cx + 20, cy), col, 2.0f);
        draw->AddLine(ImVec2(cx, cy - 20), ImVec2(cx, cy + 20), col, 2.0f);
    }
}

static void drawControlPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin("YoloTouch Control", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Backend: %s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "none");
    ImGui::Text("Frame: %ux%u rot=%d", g_shmWidth, g_shmHeight, g_rotation);
    ImGui::Separator();

    ImGui::Checkbox("Enable", &g_cfg.enabled);
    ImGui::Checkbox("Show Boxes", &g_cfg.showBoxes);
    ImGui::Checkbox("Show FPS", &g_cfg.showFps);
    ImGui::SliderFloat("Confidence", &g_cfg.confidence, 0.05f, 0.95f);
    ImGui::Separator();

    ImGui::Checkbox("Aimbot", &g_cfg.aimEnabled);
    ImGui::SliderFloat("Dead Zone", &g_cfg.deadZone, 0.005f, 0.1f);
    ImGui::SliderFloat("Smooth X", &g_cfg.smoothX, 0.0f, 0.95f);
    ImGui::SliderFloat("Smooth Y", &g_cfg.smoothY, 0.0f, 0.95f);
    ImGui::SliderFloat("Aim Speed", &g_cfg.aimSpeed, 0.1f, 3.0f);
    ImGui::SliderFloat("Predict", &g_cfg.predictGain, 0.0f, 0.2f);
    ImGui::Separator();

    ImGui::Checkbox("Trigger", &g_cfg.triggerEnabled);
    ImGui::SliderFloat("Trigger Sens", &g_cfg.triggerSensitivity, 0.1f, 1.0f);
    ImGui::Checkbox("Trigger Hold", &g_cfg.triggerHold);

    if (ImGui::Button("Apply Confidence")) {
        if (g_engine) g_engine->setConfidence(g_cfg.confidence);
    }
    ImGui::End();
}

// 模板要求的 UI 回调
void Layout_tick_UI() {
    drawControlPanel();
    drawDetectionOverlay();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 参数：model_path shm_path [workdir]
    if (argc < 3) {
        printf("Usage: %s <model.tflite> <shm_path> [workdir]\n", argv[0]);
        return -1;
    }
    const char* modelPath = argv[1];
    const char* shmPath = argv[2];
    if (argc >= 4) chdir(argv[3]);

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

    // 初始化触摸注入（uinput）
    g_touchReady = touch_init(native_window_screen_x, native_window_screen_y);
    if (g_touchReady) {
        touch_set_screen_params(native_window_screen_x, native_window_screen_y, g_rotation);
        touch_start_readers();
        printf("touch injection ready\n");
    } else {
        fprintf(stderr, "touch_init failed (need root + uinput)\n");
    }

    // 初始化推理引擎
    auto engine = std::make_unique<LiteRtEngine>();
    engine->setConfidence(g_cfg.confidence);
    bool engOk = engine->init(modelPath);
    g_engine = engine.get();
    g_engineReady.store(engOk);
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
    }

    // 启动推理线程
    g_inferRunning.store(true);
    std::thread inferThread(inferenceLoop);

    // 主循环
    bool lastFingerDown = false;
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

        drawBegin();
        Layout_tick_UI();
        drawEnd();
        std::this_thread::sleep_for(1ms);
    }

    // 清理
    g_inferRunning.store(false);
    if (inferThread.joinable()) inferThread.join();
    if (g_touchReady) touch_close();
    if (g_engine) g_engine->release();
    delete g_shm;
    shutdown();
    return 0;
}
