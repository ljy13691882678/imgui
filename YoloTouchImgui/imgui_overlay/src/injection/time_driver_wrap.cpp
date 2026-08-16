// time_driver_wrap.cpp — 内核驱动（TimeDriver）C 接口封装实现
#include "time_driver_wrap.h"
#include "time_driver.h"

#include <stdio.h>

#ifdef ANDROID
#include <android/log.h>
#define LOG_TAG "KernelDrv"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) do { fprintf(stderr, "D/" LOG_TAG ": " __VA_ARGS__); fputc('\n', stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "E/" LOG_TAG ": " __VA_ARGS__); fputc('\n', stderr); } while(0)
#endif

static bool g_inited = false;
static bool g_connected = false;

bool kdrv_init(void) {
    if (g_connected) return true;
    if (!TIME_Driver) {
        LOGE("kdrv init FAILED: TIME_Driver is null (linker error)");
        return false;
    }
    bool ok = TIME_Driver->Init();
    g_inited = true;
    g_connected = ok && TIME_Driver->IsConnected();
    uint32_t ver = TIME_Driver->Get_Version();
    LOGD("kdrv init ok=%d connected=%d version=%u (sdk_expected=%u)",
         ok, (int)g_connected, ver, TIME_SDK_EXPECTED_DRIVER_VERSION);
    if (!ok) {
        LOGE("kdrv init FAILED: Init() returned false — version mismatch or driver not loaded");
    }
    return g_connected;
}

bool kdrv_connected(void) { return g_connected && TIME_Driver && TIME_Driver->IsConnected(); }

uint32_t kdrv_version(void) {
    return TIME_Driver ? TIME_Driver->Get_Version() : 0;
}

void kdrv_exit(void) {
    if (g_inited && TIME_Driver) {
        TIME_Driver_Exit();
        LOGD("kdrv exited");
    }
    g_inited = false;
    g_connected = false;
}

// ─── 内核触摸 ───
bool kdrv_touch_init(int width, int height, int orientation) {
    if (!kdrv_init()) return false;
    bool ok = TIME_Driver->Touch_Init(width, height, orientation);
    LOGD("kdrv touch init %dx%d rot=%d ok=%d", width, height, orientation, (int)ok);
    return ok;
}

void kdrv_touch_down(int id, int x, int y) {
    if (kdrv_connected()) TIME_Driver->Touch_Down(id, x, y);
}

void kdrv_touch_move(int id, int x, int y) {
    if (kdrv_connected()) TIME_Driver->Touch_Move(id, x, y);
}

void kdrv_touch_up(int id) {
    if (kdrv_connected()) TIME_Driver->Touch_Up(id);
}

void kdrv_touch_cleanup(void) {
    if (kdrv_connected()) TIME_Driver->Touch_Cleanup();
}

void kdrv_touch_disable(void) {
    if (kdrv_connected()) TIME_Driver->Touch_Disable();
}

// ─── 内核陀螺仪 ───
bool kdrv_gyro_init(void) {
    if (!kdrv_init()) return false;
    bool ok = TIME_Driver->Gyro_Init();
    LOGD("kdrv gyro init ok=%d", (int)ok);
    return ok;
}

void kdrv_gyro_set(bool enable, float pitch, float yaw,
                   uint32_t orientation, uint32_t consume_n, uint32_t type_mask) {
    if (kdrv_connected())
        TIME_Driver->Gyro_Set(enable, pitch, yaw, orientation, consume_n, type_mask);
}

void kdrv_gyro_stop(uint32_t orientation) {
    if (kdrv_connected())
        TIME_Driver->Gyro_Set(false, 0.0f, 0.0f, orientation, 1, TIME_GYRO_MASK_ALL);
}

void kdrv_gyro_disable(void) {
    if (kdrv_connected()) TIME_Driver->Gyro_Disable();
}
