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
    //获取屏幕信息
    screen_config(); 
    ::abs_ScreenX = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    ::abs_ScreenY = (displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width);
    
    ::native_window_screen_x = displayInfo.width;
    ::native_window_screen_y = displayInfo.height;
    // 初始化imgui
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
        return -1;
    }

        
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
    static bool show_demo_window = true;
    static bool show_test_window = true;
    {
        //代码编写处

        // 显示 ImGui 示例窗口（内容丰富，用于验证渲染链路）
        if (show_demo_window) {
            ImGui::ShowDemoWindow(&show_demo_window);
        }

        // 显示一个简单的悬浮窗测试窗口，便于确认窗口可见
        if (show_test_window) {
            ImGui::SetNextWindowPos(ImVec2(80, 200), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(420, 240), ImGuiCond_FirstUseEver);
            ImGui::Begin("悬浮窗测试", &show_test_window);
            ImGui::Text("Hello, ImGui on Android 16!");
            ImGui::Text("Window: %d x %d", native_window_screen_x, native_window_screen_y);
            if (ImGui::Button("关闭本窗口")) {
                show_test_window = false;
            }
            ImGui::End();
        }
        }
       
        }
         
        
        
         
        
  
    
    
        
    
            
    