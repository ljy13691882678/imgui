// time_driver_wrap.h — 内核驱动（TimeDriver）C 接口封装
// 把 C++ 类 TimeDriver 封装成 C 函数，供 touch_core / main 调用。
// 触摸：内核触摸注入（替代 uinput）
// 陀螺仪：内核陀螺仪 hook（pitch/yaw 旋转注入，用于自瞄转向）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 生命周期
bool     kdrv_init(void);              // 初始化驱动，返回是否已连接
bool     kdrv_connected(void);         // 是否已连接
uint32_t kdrv_version(void);           // 驱动版本号
void     kdrv_exit(void);              // 退出驱动

// 内核触摸（屏幕坐标，驱动内部按 orientation 处理旋转/缩放）
bool kdrv_touch_init(int width, int height, int orientation);
void kdrv_touch_down(int id, int x, int y);
void kdrv_touch_move(int id, int x, int y);
void kdrv_touch_up(int id);
void kdrv_touch_cleanup(void);
void kdrv_touch_disable(void);

// 内核陀螺仪
bool kdrv_gyro_init(void);                             // 初始化陀螺仪 hook
void kdrv_gyro_set(bool enable, float pitch, float yaw,
                   uint32_t orientation,
                   uint32_t consume_n,
                   uint32_t type_mask);                // 注入旋转（pitch/yaw 度）
void kdrv_gyro_stop(uint32_t orientation);             // 停止注入（enable=false）
void kdrv_gyro_disable(void);

#ifdef __cplusplus
}
#endif
