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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
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
static bool g_frameReady = false;

// 采集尺寸选择：0=全屏，否则为方形边长（640/416/320/256）
static std::atomic<int> g_captureSize{0};
// 当前用于推理的采集尺寸（与 g_detections 一起在 g_detMutex 下更新）
static int g_capW = 0;
static int g_capH = 0;

// RGBA 双线性缩放到目标方形尺寸（用于降低采集分辨率，减小预处理开销）
static void ResizeRGBA(const uint8_t *src, int sw, int sh,
                       std::vector<uint8_t> &dst, int ds) {
    dst.resize((size_t)ds * ds * 4);
    for (int y = 0; y < ds; y++) {
        float sy = (sh > 1) ? (y + 0.5f) * sh / ds - 0.5f : 0.0f;
        if (sy < 0) sy = 0; else if (sy > sh - 1) sy = (float)(sh - 1);
        int y0 = (int)sy;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        float fy = sy - y0;
        for (int x = 0; x < ds; x++) {
            float sx = (sw > 1) ? (x + 0.5f) * sw / ds - 0.5f : 0.0f;
            if (sx < 0) sx = 0; else if (sx > sw - 1) sx = (float)(sw - 1);
            int x0 = (int)sx;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            float fx = sx - x0;
            uint8_t *out = &dst[((size_t)y * ds + x) * 4];
            for (int c = 0; c < 4; c++) {
                float v = (1 - fy) * (1 - fx) * src[((size_t)y0 * sw + x0) * 4 + c]
                        + (1 - fy) * fx      * src[((size_t)y0 * sw + x1) * 4 + c]
                        + fy      * (1 - fx) * src[((size_t)y1 * sw + x0) * 4 + c]
                        + fy      * fx       * src[((size_t)y1 * sw + x1) * 4 + c];
                out[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

// 采集线程：以屏幕变化频率持续抓取最新帧（RAW screencap，无 PNG 编解码）
static void CaptureLoop() {
    printf("[yolo] capture thread started\n");
    while (g_captureRunning) {
        CapturedFrame fr;
        if (ScreenCaptureRGBA(fr) && fr.width > 0 && fr.height > 0) {
            int cs = g_captureSize.load();
            int capW = fr.width, capH = fr.height;
            if (cs > 0) {  // 按所选尺寸降采样（方形）
                std::vector<uint8_t> small;
                ResizeRGBA(fr.rgba.data(), fr.width, fr.height, small, cs);
                fr.rgba = std::move(small);
                capW = cs;
                capH = cs;
            }
            {
                std::lock_guard<std::mutex> lk(g_frameMutex);
                g_latestRgba = std::move(fr.rgba);
                g_latestW = capW;
                g_latestH = capH;
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
            g_capW = w;   // 记录本次推理的采集尺寸，供绘制缩放到全屏
            g_capH = h;
        }
    }
    printf("[yolo] inference thread stopped\n");
}

static void StartInfer() {
    if (g_captureRunning) return;
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
        // 采集帧尺寸 -> 全屏坐标 的缩放（全屏采集时 g_capW/H=全屏尺寸，缩放为 1）
        float sx = (g_capW > 0) ? (float)abs_ScreenX / g_capW : 1.0f;
        float sy = (g_capH > 0) ? (float)abs_ScreenY / g_capH : 1.0f;
        for (const auto &d : g_detections) {
            if (!ClassAllowed(d.classId, filter)) continue;
            ImVec2 p1(d.x1 * sx, d.y1 * sy), p2(d.x2 * sx, d.y2 * sy);
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
         
        
        
         
        
  
    
    
        
    
            
    