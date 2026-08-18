// touch_core.h — Shared touch injection API
// Used by both JNI (Shizuku) and root_daemon (su)
// 注：触摸注入统一走内核驱动（TimeDriver），已移除所有 uinput 路径。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dedicated slots for virtual/trigger fingers on device 0
#define TOUCH_VIRTUAL_SLOT  8
#define TOUCH_TRIGGER_SLOT  9
#define TOUCH_VIRTUAL_ID    1000
#define TOUCH_TRIGGER_ID    2000

// Lifecycle（仅做设备扫描/坐标准备 + reader 线程，用于 ImGui 交互/区域判断）
bool touch_init(int screenW, int screenH);
void touch_close(void);
bool touch_is_initialized(void);
int  touch_device_count(void);

// 内核触摸（TimeDriver，唯一触摸注入路径）
bool touch_kernel_touch_init(int w, int h, int orientation);   // 创建内核虚拟触摸设备
void touch_kernel_touch_cleanup(void);                           // 销毁内核虚拟触摸设备

// 内核陀螺仪（TimeDriver，用于自瞄/压枪）
bool     touch_kernel_gyro_init(void);   // 连接驱动 + 初始化陀螺仪 hook（幂等）
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
// 触摸注入统一走 TimeDriver 内核驱动（不再使用 uinput）
bool touch_down(int slot, int id, int screenX, int screenY);
bool touch_move(int slot, int screenX, int screenY);
bool touch_up(int slot);

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

// 通用区域检测：遍历所有手指，只要有任何一个手指在区域内就返回 true
bool touch_is_any_finger_in_zone(int l, int t, int r, int b);

// Lift physical finger in joystick zone
bool touch_lift_joystick_finger(void);

// Query: primary physical finger (screen coords + pressed state)
// 用于悬浮窗 UI 输入（imgui overlay 交互）
// 返回 true 表示当前有物理手指按下；无手指时 *down 置 false 且返回 false
bool touch_get_primary_finger(int* sx, int* sy, bool* down);

#ifdef __cplusplus
}
#endif
