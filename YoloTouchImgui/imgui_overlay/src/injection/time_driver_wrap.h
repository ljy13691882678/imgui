// time_driver_wrap.h — 内核驱动（TimeDriver）C 接口封装
// 把 C++ 类 TimeDriver 封装成 C 函数，供 touch_core / main 调用。
// 支持内核陀螺仪 + 内核触摸（可选）两种注入模式。
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

// 内核陀螺仪
bool kdrv_gyro_init(void);                             // 连接驱动 + 关闭触摸接管 + 初始化陀螺仪 hook
void kdrv_gyro_set(bool enable, float pitch, float yaw,
                   uint32_t orientation,
                   uint32_t consume_n,
                   uint32_t type_mask);                // 注入旋转（pitch/yaw 度）
void kdrv_gyro_stop(uint32_t orientation);             // 停止注入（enable=false）
void kdrv_gyro_disable(void);

// 内核触摸（TimeDriver 触摸注入，替代 uinput）
// 调用 Touch_Init 创建虚拟触摸设备，随后 Touch_Disable 关闭触摸接管，
// 物理触摸不受影响，仅虚拟手指走内核驱动注入。
bool kdrv_touch_init(int width, int height, int orientation);  // 创建虚拟触摸设备 + 关闭触摸接管
void kdrv_touch_cleanup(void);                                  // 销毁虚拟触摸设备
bool kdrv_touch_down(int id, int x, int y);
bool kdrv_touch_move(int id, int x, int y);
bool kdrv_touch_up(int id);

#ifdef __cplusplus
}
#endif
