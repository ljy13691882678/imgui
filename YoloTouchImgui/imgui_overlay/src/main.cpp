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
#include <shared_mutex>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
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
// 最近一帧处理耗时（ms，含推理），供面板/日志诊断 QNN 是否过慢
static std::atomic<long long> g_lastFrameMs{0};

// 控制配置
static AimConfig g_cfg;
static bool g_aimActive = false;
static float g_aimX = 0.5f, g_aimY = 0.5f;

// 控制面板折叠：true 时只显示一个小状态框
static bool g_panelCollapsed = false;

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
    const int w = (int)h->width;
    const int hh = (int)h->height;
    if (w <= 0 || hh <= 0) return;
    if (!g_engine) return;

    // 读锁保护引擎（切换模型时会取写锁等待）
    std::shared_lock<std::shared_mutex> engLock(g_engineMutex);
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

// 最近一次 processFrame 完成时刻（ms），用于主循环诊断推理是否卡死
static std::atomic<long long> g_lastFrameDoneMs{0};

static void inferenceLoop() {
    uint64_t frames = 0;
    long long lastFpsMs = getTimeNowMs();
    long long lastNoFrameLog = getTimeNowMs();
    long long lastStuckLog = getTimeNowMs();
    bool everGotFrame = false;

    while (g_inferRunning.load()) {
        if (!g_shm || !g_shm->valid()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        const uint8_t* frame = g_shm->readFrame();
        if (!frame) {
            // 诊断：长时间没有新帧（APK 未写帧 / 共享内存未通）
            long long now = getTimeNowMs();
            if (now - lastNoFrameLog >= 5000) {
                lastNoFrameLog = now;
                printf("[infer] no new frame for 5s, everGotFrame=%d, shmSeq=%u\n",
                       everGotFrame ? 1 : 0, g_shm->readSeq());
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        everGotFrame = true;

        long long frameStart = getTimeNowMs();
        processFrame(frame, g_shm->freshHeader());
        g_lastFrameMs.store(getTimeNowMs() - frameStart);
        g_lastFrameDoneMs.store(getTimeNowMs());
        frames++;

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
    if (!g_cfg.showBoxes && !g_cfg.showFps) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    float sx = native_window_screen_x;
    float sy = native_window_screen_y;

    if (g_cfg.showFps) {
        char buf[96];
        snprintf(buf, sizeof(buf), "帧率: %llu  检测: %zu  跟踪: %zu",
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
    ImGui::Begin("YoloTouch 控制面板", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("折叠 ▾")) g_panelCollapsed = true;
    ImGui::SameLine();
    ImGui::Text("后端: %s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "无");
    if (g_engine && g_engineReady) {
        std::string diag = g_engine->getDiag();
        if (!diag.empty()) {
            ImGui::TextWrapped("诊断:\n%s", diag.c_str());
        }
    }
    ImGui::Text("帧: %ux%u 旋转=%d", g_shmWidth, g_shmHeight, g_rotation);
    ImGui::Text("推理: %llu FPS  帧源序号: %u  已处理: %llu 帧",
                (unsigned long long)g_inferFps.load(),
                g_shm && g_shm->valid() ? g_shm->readSeq() : 0u,
                (unsigned long long)g_frameCount.load());
    ImGui::Text("最近推理耗时: %lld ms", (long long)g_lastFrameMs.load());
    ImGui::Separator();

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
        ImGui::Separator();
    }

    ImGui::Checkbox("启用", &g_cfg.enabled);
    ImGui::Checkbox("显示检测框", &g_cfg.showBoxes);
    ImGui::Checkbox("显示帧率", &g_cfg.showFps);
    ImGui::SliderFloat("置信度阈值", &g_cfg.confidence, 0.05f, 0.95f);
    ImGui::Separator();

    ImGui::Checkbox("自瞄", &g_cfg.aimEnabled);
    ImGui::SliderFloat("死区", &g_cfg.deadZone, 0.005f, 0.1f);
    ImGui::SliderFloat("X 平滑", &g_cfg.smoothX, 0.0f, 0.95f);
    ImGui::SliderFloat("Y 平滑", &g_cfg.smoothY, 0.0f, 0.95f);
    ImGui::SliderFloat("自瞄速度", &g_cfg.aimSpeed, 0.1f, 3.0f);
    ImGui::SliderFloat("预判", &g_cfg.predictGain, 0.0f, 0.2f);
    ImGui::Separator();

    ImGui::Checkbox("扳机", &g_cfg.triggerEnabled);
    ImGui::SliderFloat("扳机灵敏度", &g_cfg.triggerSensitivity, 0.1f, 1.0f);
    ImGui::Checkbox("扳机按住", &g_cfg.triggerHold);

    if (ImGui::Button("应用置信度")) {
        if (g_engine) g_engine->setConfidence(g_cfg.confidence);
    }
    ImGui::Separator();
    if (ImGui::Button("退出进程", ImVec2(-1, 0))) {
        exitImgui();
    }
    ImGui::End();
}

// 折叠后的小状态框：可拖动，点击/按钮展开回控制面板
static void drawMiniPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("YoloTouch", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::Button("展开 ▶")) g_panelCollapsed = false;
    ImGui::SameLine();
    ImGui::Text("%s", g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "无");
    ImGui::Text("推理: %llu FPS", (unsigned long long)g_inferFps.load());
    if (ImGui::Button("退出")) {
        exitImgui();
    }
    ImGui::End();
}

// 模板要求的 UI 回调
void Layout_tick_UI() {
    if (g_panelCollapsed) drawMiniPanel();
    else drawControlPanel();
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
    }

    // 启动推理线程
    g_inferRunning.store(true);
    std::thread inferThread(inferenceLoop);

    // 主循环
    bool lastFingerDown = false;
    long long lastWatchdogLog = getTimeNowMs();
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
                    "engine=%s shmSeq=%u processed=%llu lastFrameMs=%lld\n",
                    g_engine && g_engineReady ? g_engine->getBackendType().c_str() : "?",
                    g_shm ? g_shm->readSeq() : 0u,
                    (unsigned long long)g_frameCount.load(),
                    (long long)g_lastFrameMs.load());
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
