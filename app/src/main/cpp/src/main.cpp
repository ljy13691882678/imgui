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
    Touch_Close();
    return 0;
} 


   
void Layout_tick_UI() {
    static bool show_demo_window = false;
    static bool show_another_window = false;
    static float f = 0.0f;
    static int counter = 0;

    // 主悬浮窗：确保能稳稳覆盖在屏幕最上层
    ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("ImGui Overlay", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("你好，ImGui!");   // 中文显示
    ImGui::Text("屏幕: %d x %d", native_window_screen_x, native_window_screen_y);
    ImGui::Text("帧率: %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::Separator();
    if (ImGui::Button("显示 Demo 窗口"))       show_demo_window = !show_demo_window;
    ImGui::SameLine();
    if (ImGui::Button("计数器 +1"))            counter++;
    ImGui::Text("counter = %d", counter);
    ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
    ImGui::End();

    if (show_demo_window) {
        ImGui::SetNextWindowPos(ImVec2(400, 80), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
        ImGui::ShowDemoWindow(&show_demo_window);
    }
}
         
        
        
         
        
  
    
    
        
    
            
    