LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
# 使用 opengl 绘制（1=opengl，0=vulkan）
OPENGL_DRAW = 0

LOCAL_MODULE := imgui

LOCAL_CFLAGS := -std=c++17
LOCAL_CPPFLAGS := -std=c++17

LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR

ifeq ($(OPENGL_DRAW), 1)
    LOCAL_CFLAGS += -DUSE_OPENGL
    LOCAL_CPPFLAGS += -DUSE_OPENGL
endif

# 头文件
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/My_Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Android_vulkan
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/tensorflow
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/qnn
LOCAL_C_INCLUDES += $(LOCAL_PATH)/src
LOCAL_C_INCLUDES += $(LOCAL_PATH)/src/inference
LOCAL_C_INCLUDES += $(LOCAL_PATH)/src/injection
LOCAL_C_INCLUDES += $(LOCAL_PATH)/paradise

# 源码
LOCAL_SRC_FILES := \
    src/main.cpp \
    src/Android_draw/draw.cpp \
    src/Android_touch/TouchHelperA.cpp \
    src/ImGui/imgui.cpp \
    src/ImGui/imgui_demo.cpp \
    src/ImGui/imgui_draw.cpp \
    src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp \
    src/ImGui/backends/imgui_impl_android.cpp \
    src/My_Utils/stb_image.cpp \
    src/inference/litert_engine.cpp \
    src/inference/qnn_engine.cpp \
    src/injection/touch_core.cpp \
    src/injection/paradise_wrap.cpp

ifeq ($(OPENGL_DRAW), 1)
    LOCAL_SRC_FILES += src/ImGui/backends/imgui_impl_opengl3.cpp
    LOCAL_SRC_FILES += src/My_Utils/imgui_image.cpp
else
    LOCAL_SRC_FILES += src/ImGui/backends/imgui_impl_vulkan.cpp
    LOCAL_SRC_FILES += src/Android_vulkan/vulkan_wrapper.cpp
    LOCAL_SRC_FILES += src/Android_vulkan/VulkanUtils.cpp
endif

# 链接系统库
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -ldl

# 预编译 TFLite/QNN 库（链接时定位；运行时由 APK 注入 LD_LIBRARY_PATH 指向可执行文件目录）
LOCAL_LDLIBS += -L$(LOCAL_PATH)/lib/arm64-v8a
LOCAL_LDLIBS += -ltensorflowlite_jni
LOCAL_LDLIBS += -lQnnTFLiteDelegate -lQnnHtp -lQnnSystem

# paradise 内核驱动静态库（paradise_wrap 桥接层使用；ARM64 预编译）
LOCAL_LDLIBS += -L$(LOCAL_PATH)/paradise
LOCAL_LDLIBS += -lparadise_api

include $(BUILD_EXECUTABLE)
