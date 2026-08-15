#include "Android_draw/draw.h"

#if defined(USE_OPENGL)
    #include "imgui_image.h"
#else
    #include "VulkanUtils.h"
#endif

#include "picture_ZhenAiKun_png.h"


 
bool show_demo_window = true;
bool show_draw_Line = true;
bool main_thread_flag = true;

int abs_ScreenX = 0;
int abs_ScreenY = 0;



/**
 * imgui测试
 */
#if defined(USE_OPENGL)
    TextureInfo op_img;
#else
    MyTextureData vk_img;
#endif

// ==================== YOLO 推理（可选） ====================
#ifdef ENABLE_YOLO
#include "Yolo/YoloDetector.h"
#include "Yolo/ScreenCapture.h"
#include "H264/H264Stream.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <set>
#include <sstream>
#include <cstdlib>
#include <cstring>

// 解析逗号分隔字符串（去空白），用于类别名称 / 类别过滤
static std::vector<std::string> ParseCSV(const char *s) {
    std::vector<std::string> out;
    std::stringstream ss(s ? s : "");
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        out.push_back(item.substr(b, e - b + 1));
    }
    return out;
}

// 类别过滤：filter 为空表示显示全部，否则仅显示集合内的 classId
static bool ClassAllowed(int classId, const std::set<int> &filter) {
    return filter.empty() || filter.count(classId) > 0;
}

static YoloDetector        g_detector;
static std::mutex          g_detMutex;
static std::vector<Detection> g_detections;

// H264 流式采集（高帧率）：启动后由 screenrecord + AMediaCodec 硬件解码，
// 相比逐帧 screencap 可把采集帧率从 ~2FPS 提升到 30~60FPS。
static H264Stream          g_h264;

// 采集 → 推理 流水线：采集线程持续抓最新屏幕帧，推理线程只跑最新帧，两者重叠互不阻塞
static std::thread         g_captureThread;
static std::thread         g_inferThread;
static std::atomic<bool>   g_captureRunning{false};
static std::atomic<bool>   g_inferRunning{false};

static std::mutex              g_frameMutex;
static std::condition_variable g_frameCv;
static std::vector<uint8_t>    g_latestRgba;   // 最新一帧 RGBA
static int g_latestW = 0;
static int g_latestH = 0;
static int g_latestOx = 0;   // 采集范围在屏幕上的原点（裁剪时为居中偏移）
static int g_latestOy = 0;
static bool g_frameReady = false;

// 采集尺寸选择：0=全屏，否则为屏幕正中间的正方形边长（640/416/320/256）
// 默认 256：与 256×256 模型输入一致，走 Preprocess 等尺寸快路径，追 120+ FPS。
static std::atomic<int> g_captureSize{256};
// 当前推理帧率（由推理线程按相邻两次出结果间隔计算）
static std::atomic<float> g_inferFps{0.0f};
// 当前用于推理的采集尺寸与采集范围原点（与 g_detections 一起在 g_detMutex 下更新）
static int g_capW = 0;
static int g_capH = 0;
static int g_ox = 0;
static int g_oy = 0;
// H264 解码帧相对全屏的缩放因子（解码降为 1/2 分辨率时 = 2.0）。
// 检测框坐标从解码帧坐标映射回全屏坐标时乘此因子。
static float g_h264Scale = 1.0f;

// 在屏幕正中间裁剪一个 cs x cs 的 RGBA 区域
static void CropCenterRGBA(const uint8_t *src, int sw, int sh,
                           std::vector<uint8_t> &dst, int cs, int &ox, int &oy) {
    ox = (sw - cs) / 2;
    oy = (sh - cs) / 2;
    dst.resize((size_t)cs * cs * 4);
    for (int y = 0; y < cs; y++) {
        memcpy(&dst[(size_t)y * cs * 4],
               &src[((size_t)(oy + y) * sw + ox) * 4],
               (size_t)cs * 4);
    }
}

// 采集线程：以屏幕变化频率持续抓取最新帧
// 优先用 H264 流式采集（高帧率），不可用时回退逐帧 screencap
static void CaptureLoop() {
    printf("[yolo] capture thread started\n");
    while (g_captureRunning) {
        CapturedFrame fr;
        bool got = false;
        if (g_h264.Running()) {
            // H264 流式：取最新解码帧，无新帧则稍等避免空转
            got = g_h264.GrabFrame(fr.rgba, fr.width, fr.height);
            if (!got) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
        } else {
            // 回退：逐帧 screencap
            got = ScreenCaptureRGBA(fr);
        }
        if (got && fr.width > 0 && fr.height > 0) {
            // 记录解码帧相对全屏的缩放因子，供检测框坐标还原到全屏。
            // 全屏时 =1.0；解码降为 1/2 分辨率时 =2.0。
            if (displayInfo.width > 0 && fr.width > 0)
                g_h264Scale = (float)displayInfo.width / fr.width;
            int cs = g_captureSize.load();
            int capW = fr.width, capH = fr.height;
            int ox = 0, oy = 0;
            if (cs > 0 && cs < fr.width && cs < fr.height) {
                // 采集屏幕正中间 cs x cs 区域
                std::vector<uint8_t> cropped;
                CropCenterRGBA(fr.rgba.data(), fr.width, fr.height, cropped, cs, ox, oy);
                fr.rgba = std::move(cropped);
                capW = cs;
                capH = cs;
            }
            {
                std::lock_guard<std::mutex> lk(g_frameMutex);
                g_latestRgba = std::move(fr.rgba);
                g_latestW = capW;
                g_latestH = capH;
                g_latestOx = ox;
                g_latestOy = oy;
                g_frameReady = true;
            }
            g_frameCv.notify_one();
        }
    }
    printf("[yolo] capture thread stopped\n");
}

// 推理线程：取最新一帧推理（采集快时自动跳过中间帧，始终用最新画面）
static void InferLoop() {
    printf("[yolo] inference thread started\n");
    std::vector<uint8_t> rgba;
    auto last = std::chrono::steady_clock::now();
    float ema = 0.0f;
    while (true) {
        int w = 0, h = 0;
        {
            std::unique_lock<std::mutex> lk(g_frameMutex);
            g_frameCv.wait(lk, [] { return g_frameReady || !g_captureRunning; });
            if (!g_frameReady) {
                if (!g_captureRunning) break;  // 采集停止且无新帧 → 退出
                continue;
            }
            rgba = std::move(g_latestRgba);
            w = g_latestW;
            h = g_latestH;
            g_frameReady = false;
        }
        std::vector<Detection> dets = g_detector.Detect(rgba.data(), w, h);
        {
            std::lock_guard<std::mutex> lk(g_detMutex);
            g_detections = std::move(dets);
            g_capW = w;   // 记录本次推理的采集尺寸与采集范围原点，供绘制还原到全屏
            g_capH = h;
            g_ox = g_latestOx;
            g_oy = g_latestOy;
        }
        // 诊断：每 60 帧打印一次检测数量与画面平均亮度，用于排查“有帧但是没框”。
        // 平均亮度过低(接近 0)= 画面黑/数据损坏；检测数为 0 = 模型在画面上没检到目标。
        {
            static long diagCount = 0;
            if ((diagCount++ % 60) == 0) {
                size_t dN = 0;
                uint64_t lum = 0;
                {
                    std::lock_guard<std::mutex> lk(g_detMutex);
                    dN = g_detections.size();
                }
                size_t px = (size_t)w * h;
                if (px > 0) {
                    for (size_t i = 0; i < px; i += 4)  // 采样，避免拖慢推理线程
                        lum += rgba[i * 4 + 0];
                    lum = lum / (px / 4);
                }
                printf("[yolo] diag det=%zu avgR=%llu cap=%dx%d\n",
                       dN, (unsigned long long)lum, w, h);
            }
        }
        // 推理帧率：相邻两次出结果间隔的滑动平均
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0.0) {
            float inst = (float)(1.0 / dt);
            ema = (ema == 0.0f) ? inst : ema * 0.9f + inst * 0.1f;
            g_inferFps.store(ema);
        }
    }
    printf("[yolo] inference thread stopped\n");
}

static void StartInfer() {
    if (g_captureRunning) return;
    // 启动高帧率 H264 流式采集：解码为 1/2 分辨率，显著降低每帧 memcpy/解码开销，
    // 让采集线程能跟上高帧率推理。坐标还原由 g_h264Scale 处理。
    int decodeW = displayInfo.width / 2, decodeH = displayInfo.height / 2;
    if (decodeW < 2) decodeW = displayInfo.width;
    if (decodeH < 2) decodeH = displayInfo.height;
    if (!g_h264.Start(decodeW, decodeH, 8 * 1000 * 1000)) {
        printf("[yolo] H264 capture start failed: %s, fallback to screencap\n",
               g_h264.LastError());
    } else {
        printf("[yolo] H264 capture started\n");
    }
    g_captureRunning = true;
    g_inferRunning = true;
    g_captureThread = std::thread(CaptureLoop);
    g_inferThread = std::thread(InferLoop);
}

static void StopInfer() {
    if (!g_captureRunning) return;
    g_captureRunning = false;
    g_inferRunning = false;
    g_frameCv.notify_all();
    if (g_captureThread.joinable()) g_captureThread.join();
    if (g_inferThread.joinable()) g_inferThread.join();
    g_h264.Stop();
}
#endif // ENABLE_YOLO

int main(int argc, char *argv[]) {
    // 关闭 stdout 缓冲：重定向到文件时 printf 立即写入，便于排错
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("[imgui] main start, pid=%d\n", getpid());
    //获取屏幕信息
    screen_config();
    printf("[imgui] displayInfo: w=%d h=%d orientation=%d\n",
           displayInfo.width, displayInfo.height, displayInfo.orientation);
    // 悬浮窗覆盖整个屏幕：直接使用显示尺寸
    ::abs_ScreenX = displayInfo.width;
    ::abs_ScreenY = displayInfo.height;
    ::native_window_screen_x = displayInfo.width;
    ::native_window_screen_y = displayInfo.height;
    printf("[imgui] window size: %dx%d\n", native_window_screen_x, native_window_screen_y);
    // 初始化imgui
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
        printf("[imgui] initGUI_draw FAILED\n");
        return -1;
    }
    printf("[imgui] initGUI_draw OK\n");

        
    printf("安卓开发环境\n");
    
    #if defined(USE_OPENGL)
        op_img = createTexture_ALL_FromMem(picture_ZhenAiKun_PNG_H, sizeof(picture_ZhenAiKun_PNG_H));
    #else
        LoadTextureFromMemory((const void *)&picture_ZhenAiKun_PNG_H, sizeof(picture_ZhenAiKun_PNG_H), &vk_img);
    #endif
    
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    
    while (main_thread_flag) {
        // imgui画图开始前调用
        drawBegin(); //
        
        Layout_tick_UI();
        
        drawEnd();// imgui画图结束调用
        std::this_thread::sleep_for(1ms);
    }   
    
    shutdown();
#ifdef ENABLE_YOLO
    StopInfer();
#endif
    Touch_Close();
    return 0;
} 


   
void Layout_tick_UI() {
    static bool collapsed = false;          // 面板收起状态
    static bool yoloOn = false;             // 检测开关
    static bool showBoxes = true;           // 显示检测框
    static bool showLabels = true;          // 显示标签
    static float conf = 0.25f;
    static float iou = 0.45f;
    static char modelPath[512] = "/data/local/tmp/models/valorant_256_v26n.tflite";
    static bool modelLoadFailed = false;

    // 类别设置：名称（逗号分隔）+ 过滤
    static char classNames[2048] = "player,enemy,head";
    static bool enableClassFilter = false;
    static char classFilter[256] = "";

#ifdef ENABLE_YOLO
    // 同步阈值到检测器
    g_detector.SetConfidence(conf);
    g_detector.SetIou(iou);

    // 解析类别名称与过滤集合
    std::vector<std::string> names = ParseCSV(classNames);
    std::set<int> filter;
    if (enableClassFilter) {
        for (auto &tok : ParseCSV(classFilter)) {
            int v = atoi(tok.c_str());
            if (v >= 0) filter.insert(v);
        }
    }

    // 1) 在透明全屏层上叠加检测框（真实屏幕坐标，描边框）
    if (showBoxes && yoloOn) {
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        std::lock_guard<std::mutex> lk(g_detMutex);
        const int ox = g_ox, oy = g_oy;   // 采集范围在解码帧上的原点（全屏为 0,0）
        const float sc = g_h264Scale;      // 解码帧 → 全屏 缩放因子
        const int capW = g_capW, capH = g_capH;
        for (const auto &d : g_detections) {
            if (!ClassAllowed(d.classId, filter)) continue;
            float bx1 = (d.x1 + ox) * sc, by1 = (d.y1 + oy) * sc;
            float bx2 = (d.x2 + ox) * sc, by2 = (d.y2 + oy) * sc;
            ImVec2 p1(bx1, by1), p2(bx2, by2);
            dl->AddRect(p1, p2,
                        IM_COL32(0, 255, 0, 255), 0.0f, 0, 3.0f);
            if (showLabels) {
                char label[128];
                const char *cname = (d.classId >= 0 && d.classId < (int)names.size())
                                        ? names[d.classId].c_str() : nullptr;
                if (cname && cname[0])
                    snprintf(label, sizeof(label), "%s %.2f", cname, d.score);
                else
                    snprintf(label, sizeof(label), "id%d %.2f", d.classId, d.score);
                dl->AddText(ImVec2(p1.x, p1.y - 16), IM_COL32(0, 255, 0, 255), label);
            }
        }
        // 2) 描出采集范围（让用户知道当前采集区域在哪）
        if (capW > 0 && capH > 0) {
            dl->AddRect(ImVec2(ox * sc, oy * sc),
                        ImVec2((ox + capW) * sc, (oy + capH) * sc),
                        IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);
        }
    }

    // 3) 右上角常驻 HUD：推理帧率 + 推理引擎
    if (yoloOn) {
        ImDrawList *fdl = ImGui::GetForegroundDrawList();
        char hud[128];
        snprintf(hud, sizeof(hud), "推理 FPS: %.1f  |  %s  |  %s",
                 g_inferFps.load(),
                 g_detector.IsLoaded() ? g_detector.EngineName() : "未加载",
                 g_h264.Running() ? "H264流式" : "逐帧screencap");
        const char *text = hud;
        ImVec2 tsz = ImGui::CalcTextSize(text);
        float pad = 8.0f;
        float x = abs_ScreenX - tsz.x - pad - 10.0f;
        float y = 10.0f;
        // 半透明背景
        fdl->AddRectFilled(ImVec2(x - pad, y - pad),
                           ImVec2(x + tsz.x + pad, y + tsz.y + pad),
                           IM_COL32(0, 0, 0, 160), 4.0f);
        fdl->AddText(ImVec2(x, y), IM_COL32(255, 255, 0, 255), text);
    }
#endif

    // 2) 单个可收起面板
    if (collapsed) {
        // 收起态：只显示一个小图标按钮，点击展开
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("##yoloIcon", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        if (ImGui::Button("⚙")) collapsed = false;
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("YOLO 控制", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

        if (ImGui::Button("收起 <<")) collapsed = true;
        ImGui::SameLine();
        ImGui::Text("可拖动本窗口");
        ImGui::SameLine();
        if (ImGui::Button("退出")) main_thread_flag = false;
        ImGui::Separator();

        // ---- 推理参数 ----
        if (ImGui::CollapsingHeader("推理参数", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("启用检测", &yoloOn);
            ImGui::SliderFloat("置信度", &conf, 0.05f, 0.95f, "%.2f");
            ImGui::SliderFloat("IOU", &iou, 0.1f, 0.9f, "%.2f");

            if (yoloOn && !g_detector.IsLoaded()) {
                if (ImGui::Button("加载模型")) {
                    modelLoadFailed = !g_detector.LoadModel(modelPath);
                    if (modelLoadFailed)
                        printf("[yolo] load failed: %s\n", g_detector.LastError());
                    else {
                        StartInfer();
                        printf("[yolo] infer started\n");
                    }
                }
            }
            if (modelLoadFailed) {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "模型加载失败：%s", g_detector.LastError());
            }
        }

        // ---- 模型与显示 ----
        if (ImGui::CollapsingHeader("模型与显示", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::InputText("模型路径", modelPath, sizeof(modelPath));
            ImGui::Checkbox("显示检测框", &showBoxes);
            ImGui::Checkbox("显示标签", &showLabels);
            // 采集尺寸选择：越小预处理越快（精度略降），全屏精度最高
            {
                static int capIdx = 0;
                static const char *capNames[] = {"全屏", "640x640", "416x416", "320x320", "256x256"};
                static const int capVals[]  = {0, 640, 416, 320, 256};
                if (ImGui::Combo("采集尺寸", &capIdx, capNames, 5)) {
                    g_captureSize.store(capVals[capIdx]);
                    printf("[yolo] capture size -> %d\n", capVals[capIdx]);
                }
            }
        }

        // ---- 类别设置 ----
        if (ImGui::CollapsingHeader("类别设置", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextWrapped("类别名称按类别ID顺序用逗号分隔，例如：player,enemy,head");
            ImGui::InputText("类别名称", classNames, sizeof(classNames));
            ImGui::Checkbox("启用类别过滤", &enableClassFilter);
            if (enableClassFilter) {
                ImGui::TextWrapped("仅显示这些类别ID（逗号分隔），留空=全部");
                ImGui::InputText("类别过滤", classFilter, sizeof(classFilter));
            }
        }

        // ---- 性能信息 ----
        if (ImGui::CollapsingHeader("性能信息", ImGuiTreeNodeFlags_DefaultOpen)) {
#ifdef ENABLE_YOLO
            ImGui::Text("帧率: %.1f FPS", ImGui::GetIO().Framerate);
            ImGui::Text("推理耗时: %.1f ms", g_detector.lastInferMs);
            ImGui::Text("总耗时: %.1f ms", g_detector.lastTotalMs);
            ImGui::Text("模型: %dx%d", g_detector.inputW, g_detector.inputH);
            {
                std::lock_guard<std::mutex> lk(g_detMutex);
                ImGui::Text("检测目标: %zu", g_detections.size());
            }
#else
            ImGui::Text("本版本未编译 YOLO");
#endif
        }

        ImGui::End();
    }
}
         
        
        
         
        
  
    
    
        
    
            
    