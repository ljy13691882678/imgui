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
// 内核触摸模式：触摸注入走内核驱动（TimeDriver），仅保留物理手指读取用于区域判断。
// 默认 false=uinput 注入；置 true 后 touch_init 将不再创建 uinput，注入函数路由到内核驱动。
void touch_set_kernel_mode(bool en);
bool touch_kernel_mode(void);
bool touch_kernel_connected(void);  // 内核驱动是否已连接（仅内核模式有效）
bool touch_init(int screenW, int screenH, int rotation);
void touch_close(void);
bool touch_is_initialized(void);
int  touch_get_output_fd(void);

// Reader threads (for zone detection)
void touch_start_readers(void);
void touch_stop_readers(void);

// Configuration
void touch_set_screen_params(int w, int h, int rotation);

// Injection (screen coordinates — 4-way rotation handled internally)
void touch_down(int slot, int id, int screenX, int screenY);
void touch_move(int slot, int screenX, int screenY);
void touch_up(int slot);

// 内核陀螺仪：自瞄旋转注入（仅内核模式可用；uinput 模式下调用无效果）。
// pitch/yaw 单位为度，rotation 为屏幕方向（0/1/2/3）。
bool touch_kernel_gyro_init(void);
void touch_gyro_apply(bool enable, float pitch, float yaw);
void touch_gyro_stop(void);

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
