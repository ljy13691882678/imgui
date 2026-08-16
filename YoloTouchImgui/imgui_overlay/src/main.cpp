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
static std::atomic<uint64_t> g_srcFps{0};      // APK 帧源写帧速率（帧/s），区分“帧源到顶”和“推理慢”
static std::atomic<uint64_t> g_frameCount{0};
// 最近一帧处理耗时（ms，含推理），供面板/日志诊断 QNN 是否过慢
static std::atomic<long long> g_lastFrameMs{0};

// 控制配置
static AimConfig g_cfg;
static bool g_aimActive = false;
static float g_aimX = 0.5f, g_aimY = 0.5f;

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

// 自瞄/触发控制器
static KalmanTracker g_tracker;
static AimController g_aim;
static TriggerController g_trigger;

// 自瞄虚拟手指状态（拖动视角式：手指保持按下，逐帧按增量移动，目标进入死区时抬起）
static bool   g_aimFingerDown = false;
static float  g_aimFingerX = 0.0f, g_aimFingerY = 0.0f;
// 扳机虚拟手指状态（按住模式需保持按下，并在目标离开/停用时抬起）
static bool   g_triggerDown = false;

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
        if (g_touchReady) touch_up(TOUCH_VIRTUAL_SLOT);
        g_aimFingerDown = false;
    }
    if (g_triggerDown) {
        if (g_touchReady) touch_up(TOUCH_TRIGGER_SLOT);
        g_triggerDown = false;
    }
}

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
        // 目标全部丢失：释放自瞄/扳机虚拟手指，避免卡在屏幕上
        g_aimActive = false;
        releaseAimFingers();
        return;
    }

    // 选择目标
    const AimTarget* pick = nullptr;
    float bestScore = -1.0f;
    float screenCx = 0.5f, screenCy = 0.5f;
    for (const auto& t : tracks) {
        // 类别锁定：若 aimClass≥0，仅选择该类目标
        if (g_cfg.aimClass >= 0 && (int)t.classId != g_cfg.aimClass) continue;

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
    if (!pick) {
        g_aimActive = false;
        releaseAimFingers();
        return;
    }

    // 按锁定部位调整瞄准点（复制目标，把中心替换为 head/body/center 点）
    AimTarget aimTarget = *pick;
    {
        float ax, ay;
        computeAimPoint(*pick, g_cfg.aimPart, &ax, &ay);
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

    // ── 自瞄（拖动视角式）──
    // 用 AimController 计算每帧增量，保持虚拟手指按下并逐帧移动，模拟人手拖屏转向。
    // 目标进入死区后抬起手指停止转向。手指被 clamp 在触控区内。
    if (g_touchReady && g_cfg.aimEnabled && g_cfg.enabled) {
        auto out = g_aim.compute(aimTarget, g_cfg, screenCx, screenCy, dt);
        if (out.active) {
            // 归一化增量 → 像素拖拽量（× 拖拽灵敏度）
            float dpx = out.deltaX * scrW * g_cfg.dragSens;
            float dpy = out.deltaY * scrH * g_cfg.dragSens;
            // 限制单帧最大拖拽像素，避免目标偏离过大时瞬移/抖动
            float maxStep = (float)g_cfg.aimMaxStepPx;
            float step = std::sqrt(dpx*dpx + dpy*dpy);
            if (step > maxStep) { dpx *= maxStep / step; dpy *= maxStep / step; }

            if (!g_aimFingerDown) {
                // 手指从触控区中心按下
                g_aimFingerX = tzCx;
                g_aimFingerY = tzCy;
                touch_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID,
                           (int)g_aimFingerX, (int)g_aimFingerY);
                g_aimFingerDown = true;
            }
            g_aimFingerX = std::clamp(g_aimFingerX + dpx, tzLpx, tzRpx);
            g_aimFingerY = std::clamp(g_aimFingerY + dpy, tzTpx, tzBpx);
            touch_move(TOUCH_VIRTUAL_SLOT, (int)g_aimFingerX, (int)g_aimFingerY);
            // 拖到远离触控区中心时抬手回中心再按下，模拟人手重新起指，
            // 保证目标在屏幕边缘也能持续转向（游戏累积每段拖拽的旋转量）
            float drift = std::sqrt((g_aimFingerX - tzCx) * (g_aimFingerX - tzCx) +
                                    (g_aimFingerY - tzCy) * (g_aimFingerY - tzCy));
            if (drift > std::max(tzRpx - tzLpx, tzBpx - tzTpx) * 0.5f) {
                touch_up(TOUCH_VIRTUAL_SLOT);
                g_aimFingerX = tzCx;
                g_aimFingerY = tzCy;
                touch_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID,
                           (int)g_aimFingerX, (int)g_aimFingerY);
            }
            g_aimActive = true;
            g_aimX = out.targetX;
            g_aimY = out.targetY;
        } else {
            g_aimActive = false;
            if (g_aimFingerDown) {
                touch_up(TOUCH_VIRTUAL_SLOT);
                g_aimFingerDown = false;
            }
        }
    } else {
        if (g_aimFingerDown) {
            touch_up(TOUCH_VIRTUAL_SLOT);
            g_aimFingerDown = false;
        }
        g_aimActive = false;
    }

    // ── 扳机（点射/按住可切换）──
    // 触发区（fire zone）：玩家物理手指在此区域内时，暂停自动开火，避免与手动开火冲突。
    // 先按配置同步触发区到 touch_core，再让 reader 线程做硬件手指检测。
    if (g_touchReady && g_cfg.triggerEnabled && g_cfg.enabled) {
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
            if (g_triggerDown) { touch_up(TOUCH_TRIGGER_SLOT); g_triggerDown = false; }
            touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID, trigX, trigY);
            touch_up(TOUCH_TRIGGER_SLOT);
        } else if (hold) {
            if (!g_triggerDown) {
                touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID, trigX, trigY);
                g_triggerDown = true;
            }
        } else if (holdRelease || g_triggerDown) {
            touch_up(TOUCH_TRIGGER_SLOT);
            g_triggerDown = false;
        }
    } else if (g_triggerDown) {
        touch_up(TOUCH_TRIGGER_SLOT);
        g_triggerDown = false;
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

        // 推理启动开关：关闭时完全停止推理（不再 detect），并清空显示结果，
        // 让 FPS 归零、检测框消失，而不是只停自瞄
        if (!g_cfg.enabled) {
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
        g_lastFrameMs.store(getTimeNowMs() - frameStart);
        g_lastFrameDoneMs.store(getTimeNowMs());
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
    if (!g_cfg.showBoxes && !g_cfg.showFps && !g_cfg.showCropBox) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    float sx = native_window_screen_x;
    float sy = native_window_screen_y;

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
        char buf[96];
        snprintf(buf, sizeof(buf), "帧率: %llu  检测: %zu  跟踪: %zu",
                 (unsigned long long)g_inferFps.load(),
                 g_detections.size(), g_tracks.size());
        draw->AddText(ImVec2(20, 20), IM_COL32(255, 255, 0, 255), buf);
    }

    std::lock_guard<std::mutex> lock(g_detMutex);
    // 用跟踪结果绘制：真实框尺寸 + 速度预判，消除快速转动时的脱框。
    // 原始 g_detections 是最新检测快照，但不含跨帧平滑/预判，绘制会滞后。
    if (g_cfg.showBoxes) {
        for (const auto& t : g_tracks) {
            // 预判位置：用目标速度把框前移 boxPredictTime，补偿“捕获→推理→绘制”延迟
            float pcx = t.cx + t.vx * g_cfg.boxPredictTime;
            float pcy = t.cy + t.vy * g_cfg.boxPredictTime;
            float hw = (t.x2 - t.x1) * 0.5f;
            float hh = (t.y2 - t.y1) * 0.5f;
            ImVec2 p1((pcx - hw) * sx, (pcy - hh) * sy);
            ImVec2 p2((pcx + hw) * sx, (pcy + hh) * sy);
            // 描边：先画一圈黑色粗边框，再画亮色内框，保证任何背景下都清晰可见
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
                // 标签带黑色描边，避免浅色背景下看不清
                draw->AddText(ImVec2(p1.x - 1, p1.y - 1), IM_COL32(0, 0, 0, 220), lbl);
                draw->AddText(ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 220), lbl);
                draw->AddText(p1, IM_COL32(0, 255, 0, 255), lbl);
            }
        }
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
    ImGui::Text("帧源: %llu FPS | 推理: %llu FPS",
                (unsigned long long)g_srcFps.load(),
                (unsigned long long)g_inferFps.load());
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
    ImGui::Checkbox("显示裁剪框", &g_cfg.showCropBox);
    ImGui::SliderFloat("置信度阈值", &g_cfg.confidence, 0.05f, 0.95f);
    ImGui::Separator();

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
    // 检测框速度预判（毫秒）：调大让框更超前，补偿快速转动时的捕获→绘制延迟
    float boxPredictMs = g_cfg.boxPredictTime * 1000.0f;
    if (ImGui::SliderFloat("框预判(ms)", &boxPredictMs, 0.0f, 80.0f, "%.0f"))
        g_cfg.boxPredictTime = boxPredictMs / 1000.0f;
    ImGui::Separator();

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
        ImGui::Separator();
    }

    ImGui::Checkbox("自瞄", &g_cfg.aimEnabled);
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
    ImGui::SliderFloat("自瞄速度", &g_cfg.aimSpeed, 0.1f, 3.0f);
    ImGui::SliderFloat("拖拽灵敏度", &g_cfg.dragSens, 0.1f, 2.0f);
    ImGui::SliderInt("最大步长(px)", &g_cfg.aimMaxStepPx, 4, 160);
    ImGui::SliderFloat("预判", &g_cfg.predictGain, 0.0f, 0.2f);
    ImGui::Separator();
    // 触控区：自瞄拖拽注入区域（与游戏转向/瞄准区对齐）
    ImGui::Text("触控区(自瞄拖拽)");
    ImGui::SliderFloat("触控左##tzL", &g_cfg.touchZoneL, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触控上##tzT", &g_cfg.touchZoneT, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触控右##tzR", &g_cfg.touchZoneR, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触控下##tzB", &g_cfg.touchZoneB, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();
    // 触发区：玩家物理手指在此区域内时，扳机暂停自动开火
    ImGui::Text("触发区(扳机暂停)");
    ImGui::SliderFloat("触发左##fzL", &g_cfg.fireZoneL, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触发上##fzT", &g_cfg.fireZoneT, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触发右##fzR", &g_cfg.fireZoneR, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("触发下##fzB", &g_cfg.fireZoneB, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();

    ImGui::Checkbox("扳机", &g_cfg.triggerEnabled);
    ImGui::SliderFloat("扳机灵敏度", &g_cfg.triggerSensitivity, 0.1f, 1.0f);
    ImGui::Checkbox("扳机按住", &g_cfg.triggerHold);
    ImGui::SliderInt("点射间隔(ms)", &g_cfg.triggerCooldownMs, 0, 500);

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
        // 初始同步触发区（默认右下角区域）
        int fzL = (int)(g_cfg.fireZoneL * native_window_screen_x);
        int fzT = (int)(g_cfg.fireZoneT * native_window_screen_y);
        int fzR = (int)(g_cfg.fireZoneR * native_window_screen_x);
        int fzB = (int)(g_cfg.fireZoneB * native_window_screen_y);
        touch_set_fire_zone(fzL, fzT, fzR, fzB);
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
