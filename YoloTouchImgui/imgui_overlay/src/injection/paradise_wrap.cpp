// paradise_wrap.cpp — C bridge for the paradise_driver kernel touch module.
//
// The paradise_driver kernel module connects through a custom syscall
// (142 + magic numbers) and exposes touch injection via ioctls:
//   - touch_init(abs_screen_x, abs_screen_y)  -> ioctl(0x4008571e, {x, y})
//   - touch_down(slot, x, y) / touch_up(slot) -> ioctl(0x4018571f, {op,x,y,slot})
// touch_down 的 X 使用短边域，Y 使用长边域；驱动没有 touch_move 接口，
// 移动由 touch_core 用 抬起+重按下 模拟。

#include "paradise_wrap.h"

#include "paradise_api.h"

#include <mutex>

namespace {

std::mutex g_drvMutex;
paradise_driver* g_drv = nullptr;  // 持有驱动实例（构造时自动连接）
bool g_touchInited = false;        // touch_init 是否已成功调用

}  // namespace

bool paradise_connect(void) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    if (g_drv) return true;  // 已连接
    paradise_driver* drv = new (std::nothrow) paradise_driver();
    if (!drv) return false;
    // 构造函数内会尝试建立驱动 fd；失败则 fd 无效，接口全部返回 false
    g_drv = drv;
    g_touchInited = false;
    return true;
}

void paradise_disconnect(void) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    delete g_drv;
    g_drv = nullptr;
    g_touchInited = false;
}

bool paradise_is_connected(void) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    return g_drv != nullptr;
}

bool paradise_touch_init(int abs_screen_x, int abs_screen_y) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    if (!g_drv) return false;
    if (abs_screen_x <= 0 || abs_screen_y <= 0) return false;
    if (g_drv->touch_init(abs_screen_x, abs_screen_y)) {
        g_touchInited = true;
        return true;
    }
    return false;
}

bool paradise_touch_down(int slot, int x, int y) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    if (!g_drv || !g_touchInited) return false;
    return g_drv->touch_down(slot, x, y);
}

bool paradise_touch_up(int slot) {
    std::lock_guard<std::mutex> guard(g_drvMutex);
    if (!g_drv || !g_touchInited) return false;
    return g_drv->touch_up(slot);
}
