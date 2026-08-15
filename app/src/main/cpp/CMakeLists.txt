cmake_minimum_required(VERSION 3.22.1)

project(imgui_overlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# 纯 C/C++ 原生可执行文件（需要 root 运行）
# 与原始 AIDE(Android.mk) 的 Vulkan 路径保持一致：
#   LOCAL_MODULE := imgui_chain_1_3_vk  (OPENGL_DRAW = 0)
add_executable(imgui_overlay
    src/main.cpp
    src/Android_draw/draw.cpp
    src/Android_touch/TouchHelperA.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_demo.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_android.cpp
    src/ImGui/backends/imgui_impl_vulkan.cpp
    src/My_Utils/stb_image.cpp
    src/Android_vulkan/vulkan_wrapper.cpp
    src/Android_vulkan/VulkanUtils.cpp
)

target_include_directories(imgui_overlay PRIVATE
    include
    include/My_Utils
    include/Android_vulkan
    include/ImGui
    include/ImGui/backends
)

target_compile_definitions(imgui_overlay PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
)

# 与原始 Android.mk 的 LOCAL_CFLAGS 保持一致
target_compile_options(imgui_overlay PRIVATE -fvisibility=hidden)

# 与原始 Android.mk 的 LOCAL_LDLIBS 保持一致
target_link_libraries(imgui_overlay PRIVATE
    log
    android
    EGL
    GLESv3
)

# 裁剪符号表，减小打包体积
target_link_options(imgui_overlay PRIVATE -s)