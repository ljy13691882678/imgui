// time_driver_wrap.h — 内核驱动（TimeDriver）C 接口封装
// 把 C++ 类 TimeDriver 封装成 C 函数，供 touch_core / main 调用。
// 只对接内核陀螺仪（pitch/yaw 旋转注入，用于自瞄转向与压枪）。
// 不对外暴露内核触摸注入：触摸统一走 uinput，连接驱动后立即 Touch_Disable
// 关闭驱动触摸接管，避免拦截真实触摸。
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

#ifdef __cplusplus
}
#endif
