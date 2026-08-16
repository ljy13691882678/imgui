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
// 触摸注入始终走 uinput（真实手指自然透传系统，合成手指经 uinput 注入）。
bool touch_init(int screenW, int screenH, int rotation);
void touch_close(void);
bool touch_is_initialized(void);
int  touch_get_output_fd(void);
int  touch_device_count(void);      // 已打开的触摸设备数（面板诊断用）

// Reader threads (for zone detection)
void touch_start_readers(void);
void touch_stop_readers(void);

// Configuration
void touch_set_screen_params(int w, int h, int rotation);

// Injection (screen coordinates — 4-way rotation handled internally)
void touch_down(int slot, int id, int screenX, int screenY);
void touch_move(int slot, int screenX, int screenY);
void touch_up(int slot);

// 触摸注入按需初始化：touch_init 只做设备扫描/坐标准备并启动 reader
// （供 ImGui 交互与区域判断），默认不创建注入通道。
// 需要自瞄/扳机/压枪注入时，先调用 touch_inject_init()。
// 注入模式：0=uinput（/dev/uinput 合成触摸），1=内核驱动 paradise（syscall 连接驱动）。
bool touch_inject_init(void);   // 创建注入通道（触摸注入就绪）
void touch_inject_close(void);  // 关闭注入通道（停止触摸注入）
bool touch_inject_ready(void);  // 注入是否已就绪

// 注入模式选择/查询（面板切换；0=uinput，1=内核驱动 paradise）
#define TOUCH_MODE_UINPUT 0
#define TOUCH_MODE_KERNEL 1
void touch_set_inject_mode(int mode);
int  touch_get_inject_mode(void);
// 内核驱动是否已连接（驱动 fd 有效，面板诊断用）
bool touch_kernel_connected(void);

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
