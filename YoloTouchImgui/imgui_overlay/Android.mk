LOCAL_PATH := $(call my-dir)

# ===================== Capstone 反汇编库（坐标解密 ring-service 发现） =====================
include $(CLEAR_VARS)
LOCAL_MODULE := jiemi_capstone
LOCAL_C_INCLUDES += $(LOCAL_PATH)/3rd/capstone/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/3rd/capstone
LOCAL_CFLAGS := -DCAPSTONE_HAS_ARM64 -DCAPSTONE_USE_SYS_DYN_MEM -DCAPSTONE_AARCH64_COMPAT_HEADER
LOCAL_SRC_FILES := \
    3rd/capstone/cs.c \
    3rd/capstone/utils.c \
    3rd/capstone/MCInst.c \
    3rd/capstone/MCInstrDesc.c \
    3rd/capstone/MCRegisterInfo.c \
    3rd/capstone/Mapping.c \
    3rd/capstone/SStream.c \
    3rd/capstone/arch/AArch64/AArch64BaseInfo.c \
    3rd/capstone/arch/AArch64/AArch64Disassembler.c \
    3rd/capstone/arch/AArch64/AArch64InstPrinter.c \
    3rd/capstone/arch/AArch64/AArch64Mapping.c \
    3rd/capstone/arch/AArch64/AArch64Module.c
include $(BUILD_STATIC_LIBRARY)

# ===================== Unicorn 模拟器库（坐标解密执行） =====================
include $(CLEAR_VARS)
LOCAL_MODULE := jiemi_unicorn
LOCAL_SRC_FILES := 3rd/unicorn/lib/libunicorn.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
# 使用 opengl 绘制（1=opengl，0=vulkan）
OPENGL_DRAW = 0

LOCAL_MODULE := imgui

LOCAL_CFLAGS := -std=c++17
LOCAL_CPPFLAGS := -std=c++17

# T3 验证 SDK 依赖异常与 RTTI
LOCAL_CPPFLAGS += -fexceptions -frtti

LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR

# 坐标解密（jiemi）：启用 Unicorn 解密执行 + Capstone ARM64 反汇编
LOCAL_CFLAGS += -DCAPSTONE_HAS_ARM64 -DCAPSTONE_USE_SYS_DYN_MEM -DCAPSTONE_AARCH64_COMPAT_HEADER
LOCAL_CPPFLAGS += -DCAPSTONE_HAS_ARM64 -DCAPSTONE_USE_SYS_DYN_MEM -DCAPSTONE_AARCH64_COMPAT_HEADER
LOCAL_CPPFLAGS += -DJIEMI_WITH_UNICORN

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
LOCAL_C_INCLUDES += $(LOCAL_PATH)/driver
LOCAL_C_INCLUDES += $(LOCAL_PATH)/3rd/capstone/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/3rd/capstone
LOCAL_C_INCLUDES += $(LOCAL_PATH)/3rd/unicorn

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
    src/injection/time_driver_wrap.cpp \
    src/injection/stderr_shim.cpp \
    src/memory/memory_esp.cpp \
    src/t3sdk/t3sdk.cpp \
    src/auth/t3auth.cpp

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

# 内核驱动（TimeDriver）静态库：arm64-v8a 预编译 .a，提供内核陀螺仪 hook（触摸仍走 uinput）
LOCAL_LDLIBS += -L$(LOCAL_PATH)/driver/arm64-v8a
LOCAL_LDLIBS += -ltime_driver

include $(BUILD_EXECUTABLE)
