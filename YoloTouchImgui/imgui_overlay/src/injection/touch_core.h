// touch_core.h — Shared touch injection API
// Used by both JNI (Shizuku) and root_daemon (su)

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dedicated slots for virtual/trigger fingers on device 0
#define TOUCH_VIRTUAL_SLOT  8
#define TOUCH_TRIGGER_SLOT  9
#define TOUCH_VIRTUAL_ID    1000
#define TOUCH_TRIGGER_ID    2000

// Lifecycle
bool touch_init(int screenW, int screenH);
void touch_close(void);
bool touch_is_initialized(void);
int  touch_get_output_fd(void);
int  touch_device_count(void);

// uinput 注入设备生命周期（触摸注入统一走 uinput）
// touch_init 只做设备扫描/坐标准备（供 ImGui 交互、区域判断），不创建 uinput；
// 需要自瞄/扳机/压枪时调用 touch_inject_init 创建注入设备。
// 内核陀螺仪模式下屏蔽 uinput 注入及初始化（不调用 touch_inject_init）。
bool touch_inject_ready(void);   // uinput 注入设备是否就绪
bool touch_inject_init(void);    // 按需创建 uinput 注入设备（幂等）
void touch_inject_close(void);   // 销毁 uinput 注入设备

// 内核陀螺仪（TimeDriver，仅陀螺仪；触摸注入统一走 uinput）
bool     touch_kernel_gyro_init(void);   // 连接驱动 + 关闭触摸接管 + 初始化陀螺仪 hook（幂等）
bool     touch_kernel_connected(void);   // 驱动是否已连接
uint32_t touch_kernel_version(void);     // 驱动版本号
void     touch_gyro_apply(bool enable, float pitch, float yaw);  // 注入旋转（pitch/yaw 度）
void     touch_gyro_stop(void);          // 停止注入（enable=false）
void     touch_gyro_disable(void);       // 关闭陀螺仪 hook

// Reader threads (for zone detection)
void touch_start_readers(void);
void touch_stop_readers(void);

// Configuration
void touch_set_screen_params(int w, int h, int rotation);

// Injection (screen coordinates — 4-way rotation handled internally)
void touch_down(int slot, int id, int screenX, int screenY);
void touch_move(int slot, int screenX, int screenY);
void touch_up(int slot);

// Zone configuration (screen coordinates)
void touch_set_trigger_zone(int l, int t, int r, int b);
void touch_set_ads_zone(int l, int t, int r, int b);
void touch_set_fire_zone(int l, int t, int r, int b);
void touch_set_joystick_zone(int l, int t, int r, int b);

// Zone queries
bool touch_is_finger_in_trigger_zone(void);
bool touch_is_finger_in_ads_zone(void);
bool touch_is_finger_in_fire_zone(void);
bool touch_is_finger_in_joystick_zone(void);

// Lift physical finger in joystick zone
bool touch_lift_joystick_finger(void);

// Query: primary physical finger (screen coords + pressed state)
// 用于悬浮窗 UI 输入（imgui overlay 交互）
// 返回 true 表示当前有物理手指按下；无手指时 *down 置 false 且返回 false
bool touch_get_primary_finger(int* sx, int* sy, bool* down);

#ifdef __cplusplus
}
#endif
