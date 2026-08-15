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
#include <atomic>

static YoloDetector        g_detector;
static std::mutex          g_detMutex;
static std::vector<Detection> g_detections;
static std::thread         g_inferThread;
static std::atomic<bool>   g_inferRunning{false};

static void CaptureAndInfer() {
    printf("[yolo] inference thread started\n");
    while (g_inferRunning) {
        CapturedFrame fr;
        if (ScreenCaptureRGBA(fr)) {
            std::vector<Detection> dets = g_detector.Detect(fr.rgba.data(), fr.width, fr.height);
            {
                std::lock_guard<std::mutex> lk(g_detMutex);
                g_detections = std::move(dets);
            }
        }
    }
    printf("[yolo] inference thread stopped\n");
}

static void StartInfer() {
    if (g_inferRunning) return;
    g_inferRunning = true;
    g_inferThread = std::thread(CaptureAndInfer);
}

static void StopInfer() {
    if (!g_inferRunning) return;
    g_inferRunning = false;
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
    static char modelPath[512] = "/data/local/tmp/models/yolov8n-int8.tflite";
    static bool modelLoadFailed = false;

#ifdef ENABLE_YOLO
    // 同步阈值到检测器
    g_detector.SetConfidence(conf);
    g_detector.SetIou(iou);

    // 1) 在透明全屏层上叠加检测框（真实屏幕坐标）
    if (showBoxes && yoloOn) {
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        std::lock_guard<std::mutex> lk(g_detMutex);
        for (const auto &d : g_detections) {
            dl->AddRect(ImVec2(d.x1, d.y1), ImVec2(d.x2, d.y2),
                        IM_COL32(0, 255, 0, 255), 0.0f, 0, 3.0f);
            if (showLabels) {
                char label[96];
                snprintf(label, sizeof(label), "id%d %.2f", d.classId, d.score);
                dl->AddText(ImVec2(d.x1, d.y1 - 16), IM_COL32(0, 255, 0, 255), label);
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
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("YOLO 控制", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

        if (ImGui::Button("收起 <<")) collapsed = true;
        ImGui::SameLine();
        ImGui::Text("可拖动本窗口");
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
         
        
        
         
        
  
    
    
        
    
            
    