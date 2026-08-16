// paradise_wrap.h — C bridge for the paradise_driver kernel touch module.
// Wraps the C++ paradise_driver class so touch_core (C) can use kernel touch
// injection without depending on the C++ class directly.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 连接内核驱动（构造 paradise_driver 并尝试建立驱动 fd）。
// 驱动通过 syscall 142 + 魔数连接，失败时返回 false。
bool paradise_connect(void);

// 断开驱动（销毁驱动对象）
void paradise_disconnect(void);

// 驱动是否已连接（fd 有效）
bool paradise_is_connected(void);

// 初始化触摸注入。坐标约定：X 使用短边域，Y 使用长边域（竖屏 portrait 坐标系）。
bool paradise_touch_init(int abs_screen_x, int abs_screen_y);

// 手指按下（slot 槽位，x/y 为短边域/长边域坐标）
bool paradise_touch_down(int slot, int x, int y);

// 抬起指定 slot
bool paradise_touch_up(int slot);

#ifdef __cplusplus
}
#endif
