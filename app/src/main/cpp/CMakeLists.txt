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
    src/Yolo/YoloDetector.cpp
    src/Yolo/ScreenCapture.cpp
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
    include/Yolo
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

# ---- YOLO 推理：TensorFlow Lite + NNAPI delegate（可选）----
# 仅在 GitHub 云端编译时通过环境变量注入 TFLite_ROOT/TFLITE_LIB；
# 本地不带 TFLite 的构建会跳过，保证原有工程仍可编译。
set(TFLITE_ROOT_ENV "$ENV{TFLITE_ROOT}")
set(TFLITE_LIB_ENV "$ENV{TFLITE_LIB}")
if(TFLITE_ROOT_ENV AND TFLITE_LIB_ENV)
    message(STATUS "YOLO: enabling TFLite inference")
    target_compile_definitions(imgui_overlay PRIVATE ENABLE_YOLO)
    target_include_directories(imgui_overlay PRIVATE "${TFLITE_ROOT_ENV}")
    target_link_libraries(imgui_overlay PRIVATE
        "${TFLITE_LIB_ENV}"
        z
        dl
        m
        neuralnetworks
    )
endif ()

# 裁剪符号表，减小打包体积
target_link_options(imgui_overlay PRIVATE -s)