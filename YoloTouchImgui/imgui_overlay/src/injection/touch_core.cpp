// touch_core.cpp — Core touch injection logic
// 注：触摸注入统一走 TimeDriver 内核驱动，已移除所有 uinput 相关代码。
// 设备 reader 线程仅用于读取物理手指状态供区域判断/自瞄触发门控。

#include "touch_core.h"
#include "time_driver_wrap.h"
#include "time_driver.h"   // TIME_GYRO_MASK_ALL 等常量
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

#ifdef ANDROID
#include <android/log.h>
#define LOG_TAG "TouchCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) do { fprintf(stderr, "D/" LOG_TAG ": " __VA_ARGS__); fputc('\n', stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "E/" LOG_TAG ": " __VA_ARGS__); fputc('\n', stderr); } while(0)
#endif

// ─── Constants ───────────────────────────────────────────────────────

static constexpr int maxF = 10;

// ─── Data structures ────────────────────────────────────────────────

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}
};

struct TouchObj {
    Vec2 pos{};
    int id = 0;
    bool isDown = false;
};

struct Device {
    int fd = 0;
    float s2tx = 1.0f;
    float s2ty = 1.0f;
    input_absinfo absX{};
    input_absinfo absY{};
    TouchObj fingers[maxF]{};
};

struct Zone {
    int l = 0, t = 0, r = 0, b = 0;
    volatile int finger_inside = 0;
};

// ─── Global state ────────────────────────────────────────────────────

static std::vector<Device> g_devices;
static Vec2 g_touchScale{1.0f, 1.0f};
static Vec2 g_screenSize{};
static std::mutex g_mutex;
static bool g_initialized = false;

// Screen params
static int g_screen_w = 0, g_screen_h = 0;
static int g_rotation = 0;

// Reader threads
static std::vector<pthread_t> g_reader_threads;
static volatile bool g_running = false;

// Detection zones
static Zone g_trigger_zone;
static Zone g_ads_zone;
static Zone g_fire_zone;
static Zone g_joystick_zone;

// ─── Helpers ─────────────────────────────────────────────────────────

static bool pointInZone(const Zone& z, int sx, int sy) {
    return z.l < z.r && z.t < z.b &&
           sx >= z.l && sx <= z.r && sy >= z.t && sy <= z.b;
}

static void touchToScreen(float devX, float devY, int touchMaxX, int touchMaxY, int& sx, int& sy);

static bool isTrackedPhysicalFinger(size_t deviceIndex, int fingerIndex) {
    if (deviceIndex >= g_devices.size() || fingerIndex < 0 || fingerIndex >= maxF) {
        return false;
    }
    const TouchObj& finger = g_devices[deviceIndex].fingers[fingerIndex];
    if (!finger.isDown) {
        return false;
    }
    if (deviceIndex == 0 && (fingerIndex == TOUCH_VIRTUAL_SLOT || fingerIndex == TOUCH_TRIGGER_SLOT)) {
        return false;
    }
    return true;
}

static bool isAnyPhysicalFingerInZoneLocked(const Zone& zone) {
    if (!g_initialized || g_devices.empty()) {
        return false;
    }
    if (zone.l >= zone.r || zone.t >= zone.b) {
        return false;
    }

    const int touchMaxX = std::max(1, g_devices[0].absX.maximum);
    const int touchMaxY = std::max(1, g_devices[0].absY.maximum);

    for (size_t deviceIndex = 0; deviceIndex < g_devices.size(); ++deviceIndex) {
        for (int fingerIndex = 0; fingerIndex < maxF; ++fingerIndex) {
            if (!isTrackedPhysicalFinger(deviceIndex, fingerIndex)) {
                continue;
            }

            const TouchObj& finger = g_devices[deviceIndex].fingers[fingerIndex];
            int sx = 0;
            int sy = 0;
            touchToScreen(finger.pos.x, finger.pos.y, touchMaxX, touchMaxY, sx, sy);
            if (pointInZone(zone, sx, sy)) {
                return true;
            }
        }
    }
    return false;
}

static int normalizeRotation(int rotation) {
    int normalized = rotation % 4;
    if (normalized < 0) normalized += 4;
    return normalized;
}

// Screen → portrait touch coords (4-way rotation + scale)
static void screenToTouch(int sx, int sy, float& tx, float& ty) {
    float px = static_cast<float>(sx);
    float py = static_cast<float>(sy);
    switch (normalizeRotation(g_rotation)) {
    case 1: // Surface.ROTATION_90
        px = static_cast<float>(g_screen_h - sy);
        py = static_cast<float>(sx);
        break;
    case 2: // Surface.ROTATION_180
        px = static_cast<float>(g_screen_w - sx);
        py = static_cast<float>(g_screen_h - sy);
        break;
    case 3: // Surface.ROTATION_270
        px = static_cast<float>(sy);
        py = static_cast<float>(g_screen_w - sx);
        break;
    default: // Surface.ROTATION_0
        break;
    }
    tx = px * g_touchScale.x;
    ty = py * g_touchScale.y;
}

static void touchToScreen(float devX, float devY, int touchMaxX, int touchMaxY, int& sx, int& sy) {
    (void)touchMaxX;
    (void)touchMaxY;

    const float scaleX = std::max(g_touchScale.x, 0.0001f);
    const float scaleY = std::max(g_touchScale.y, 0.0001f);
    const float px = devX / scaleX;
    const float py = devY / scaleY;

    float rawScreenX = px;
    float rawScreenY = py;
    switch (normalizeRotation(g_rotation)) {
    case 1: // Surface.ROTATION_90
        rawScreenX = py;
        rawScreenY = static_cast<float>(g_screen_h) - px;
        break;
    case 2: // Surface.ROTATION_180
        rawScreenX = static_cast<float>(g_screen_w) - px;
        rawScreenY = static_cast<float>(g_screen_h) - py;
        break;
    case 3: // Surface.ROTATION_270
        rawScreenX = static_cast<float>(g_screen_w) - py;
        rawScreenY = px;
        break;
    default: // Surface.ROTATION_0
        break;
    }

    sx = std::clamp(static_cast<int>(std::lround(rawScreenX)), 0, std::max(0, g_screen_w));
    sy = std::clamp(static_cast<int>(std::lround(rawScreenY)), 0, std::max(0, g_screen_h));
}

// ─── Device scanning ────────────────────────────────────────────────

static bool checkDeviceIsTouch(int fd) {
    uint8_t* bits = nullptr;
    ssize_t bitsSize = 0;
    int res = 0;
    bool hasSlot = false, hasX = false, hasY = false;
    input_absinfo abs{};
    while (true) {
        res = ioctl(fd, EVIOCGBIT(EV_ABS, bitsSize), bits);
        if (res < bitsSize) break;
        bitsSize = res + 16;
        bits = static_cast<uint8_t*>(realloc(bits, bitsSize * 2));
    }
    for (int j = 0; j < res; ++j) {
        for (int k = 0; k < 8; ++k) {
            int code = j * 8 + k;
            if ((bits[j] & (1 << k)) && ioctl(fd, EVIOCGABS(code), &abs) == 0) {
                if (code == ABS_MT_SLOT) hasSlot = true;
                if (code == ABS_MT_POSITION_X) hasX = true;
                if (code == ABS_MT_POSITION_Y) hasY = true;
            }
        }
    }
    free(bits);
    return hasSlot && hasX && hasY;
}

// ─── Close ──────────────────────────────────────────────────────────

static void closeTouchLocked() {
    if (!g_initialized) return;
    for (auto& device : g_devices) {
        close(device.fd);
        device.fd = 0;
    }
    g_initialized = false;
    g_devices.clear();
}

// ─── Reader thread ──────────────────────────────────────────────────

static void* deviceReader(void* arg) {
    int devIdx = static_cast<int>(reinterpret_cast<long>(arg));
    Device& dev = g_devices[devIdx];

    int curSlot = 0;
    input_event batch[64];

    while (g_running) {
        ssize_t n = read(dev.fd, batch, sizeof(batch));
        if (n <= 0 || n % sizeof(input_event) != 0) continue;

        size_t count = n / sizeof(input_event);
        std::lock_guard<std::mutex> guard(g_mutex);

        for (size_t j = 0; j < count; j++) {
            auto& ie = batch[j];

            if (ie.type == EV_ABS) {
                switch (ie.code) {
                case ABS_MT_SLOT:
                    curSlot = ie.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (curSlot >= 0 && curSlot < maxF) {
                        if (ie.value == -1)
                            dev.fingers[curSlot].isDown = false;
                        else {
                            dev.fingers[curSlot].isDown = true;
                            dev.fingers[curSlot].id =
                                static_cast<int>((devIdx * 2 + 1) * maxF + curSlot);
                        }
                    }
                    break;
                case ABS_MT_POSITION_X:
                    if (curSlot >= 0 && curSlot < maxF) {
                        dev.fingers[curSlot].pos.x = ie.value * dev.s2tx;
                        dev.fingers[curSlot].isDown = true;
                    }
                    break;
                case ABS_MT_POSITION_Y:
                    if (curSlot >= 0 && curSlot < maxF) {
                        dev.fingers[curSlot].pos.y = ie.value * dev.s2ty;
                        dev.fingers[curSlot].isDown = true;
                    }
                    break;
                }
            }
        }
    }

    LOGD("Reader[%d]: stopped", devIdx);
    return nullptr;
}

// ═════════════════════════════════════════════════════════════════════
//  Public API (touch_core.h)
// ═════════════════════════════════════════════════════════════════════

bool touch_init(int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) {
        LOGE("touch_init: invalid screen size %dx%d", screenW, screenH);
        return false;
    }
    std::lock_guard<std::mutex> guard(g_mutex);
    closeTouchLocked();

    Vec2 size(static_cast<float>(screenW), static_cast<float>(screenH));
    g_screenSize = size.x > size.y ? size : Vec2(size.y, size.x);
    g_screen_w = screenW;
    g_screen_h = screenH;

    DIR* dir = opendir("/dev/input/");
    if (!dir) { LOGE("open /dev/input failed"); return false; }

    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!strstr(entry->d_name, "event")) continue;
        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;
        if (!checkDeviceIsTouch(fd)) { close(fd); continue; }

        Device device{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &device.absX) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &device.absY) == 0) {
            device.fd = fd;
            // 被动监听（open 读模式）：物理触摸自然流向系统/游戏，
            // reader 线程仅用于读取手指状态供区域判断/自瞄触发门控。
            g_devices.push_back(device);
            LOGD("touch device %s max=%d,%d (passive monitor)", path, device.absX.maximum, device.absY.maximum);
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (g_devices.empty()) { LOGE("no touch device found"); return false; }

    int touchMaxX = g_devices[0].absX.maximum;
    int touchMaxY = g_devices[0].absY.maximum;

    for (auto& device : g_devices) {
        device.s2tx = static_cast<float>(touchMaxX) / std::max(1, device.absX.maximum);
        device.s2ty = static_cast<float>(touchMaxY) / std::max(1, device.absY.maximum);
    }

    Vec2 logical = size;
    if (logical.x > logical.y) std::swap(logical.x, logical.y);
    g_touchScale.x = static_cast<float>(touchMaxX) / std::max(1.0f, logical.x);
    g_touchScale.y = static_cast<float>(touchMaxY) / std::max(1.0f, logical.y);
    g_initialized = true;
    LOGD("touch ready scale=%.3f,%.3f", g_touchScale.x, g_touchScale.y);
    return true;
}

void touch_close(void) {
    touch_stop_readers();
    std::lock_guard<std::mutex> guard(g_mutex);
    closeTouchLocked();
}

bool touch_is_initialized(void) { return g_initialized; }
int  touch_device_count(void)    { return static_cast<int>(g_devices.size()); }

// ─── 内核触摸（TimeDriver，唯一触摸注入路径） ───

bool touch_kernel_touch_init(int w, int h, int orientation) {
    return kdrv_touch_init(w, h, orientation);
}

void touch_kernel_touch_cleanup(void) {
    kdrv_touch_cleanup();
}

// ─── 内核陀螺仪（TimeDriver，用于自瞄/压枪） ───

bool touch_kernel_gyro_init(void) {
    return kdrv_gyro_init();
}

void touch_gyro_apply(bool enable, float pitch, float yaw) {
    if (!kdrv_connected() && !touch_kernel_gyro_init()) return;
    kdrv_gyro_set(enable, pitch, yaw, (uint32_t)g_rotation, 1, TIME_GYRO_MASK_ALL);
}

void touch_gyro_stop(void) {
    kdrv_gyro_stop((uint32_t)g_rotation);
}

void touch_gyro_disable(void) {
    kdrv_gyro_disable();
}

bool touch_kernel_connected(void) { return kdrv_connected(); }

uint32_t touch_kernel_version(void) { return kdrv_version(); }

void touch_start_readers(void) {
    if (g_running) return;
    if (!g_initialized) return;

    g_running = true;
    g_reader_threads.resize(g_devices.size());
    for (size_t i = 0; i < g_devices.size(); i++) {
        if (pthread_create(&g_reader_threads[i], nullptr, deviceReader,
                           reinterpret_cast<void*>(i)) != 0) {
            LOGE("pthread_create failed for device %zu", i);
            g_running = false;
            g_reader_threads.resize(i);
            return;
        }
    }
    LOGD("Started %zu reader threads", g_devices.size());
}

void touch_stop_readers(void) {
    if (!g_running) return;
    g_running = false;
    for (auto& t : g_reader_threads)
        pthread_join(t, nullptr);
    g_reader_threads.clear();
    LOGD("Stopped all readers");
}

void touch_set_screen_params(int w, int h, int rotation) {
    g_screen_w = w;
    g_screen_h = h;
    g_rotation = normalizeRotation(rotation);
}

// ─── 触摸注入（统一走 TimeDriver 内核驱动） ───
// 注：与之前 uinput 实现不同，这里 screen 坐标直接交给 TimeDriver，
// 由驱动内部按 orientation 处理坐标变换。
// slot → id 映射：down 时使用的 tracking id，move/up 必须使用相同 id。

static int slot_to_id(int slot) {
    if (slot == TOUCH_VIRTUAL_SLOT) return TOUCH_VIRTUAL_ID;
    if (slot == TOUCH_TRIGGER_SLOT) return TOUCH_TRIGGER_ID;
    return slot;
}

bool touch_down(int slot, int id, int screenX, int screenY) {
    (void)slot;
    return kdrv_touch_down(id, screenX, screenY);
}

bool touch_move(int slot, int screenX, int screenY) {
    return kdrv_touch_move(slot_to_id(slot), screenX, screenY);
}

bool touch_up(int slot) {
    return kdrv_touch_up(slot_to_id(slot));
}

void touch_set_trigger_zone(int l, int t, int r, int b)  { g_trigger_zone = {l, t, r, b, 0}; }
void touch_set_ads_zone(int l, int t, int r, int b)      { g_ads_zone = {l, t, r, b, 0}; }
void touch_set_fire_zone(int l, int t, int r, int b)     { g_fire_zone = {l, t, r, b, 0}; }
void touch_set_joystick_zone(int l, int t, int r, int b) { g_joystick_zone = {l, t, r, b, 0}; }

bool touch_is_finger_in_trigger_zone(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return isAnyPhysicalFingerInZoneLocked(g_trigger_zone);
}

bool touch_is_finger_in_ads_zone(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return isAnyPhysicalFingerInZoneLocked(g_ads_zone);
}

bool touch_is_finger_in_fire_zone(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return isAnyPhysicalFingerInZoneLocked(g_fire_zone);
}

bool touch_is_finger_in_joystick_zone(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return isAnyPhysicalFingerInZoneLocked(g_joystick_zone);
}

// 通用区域检测：遍历所有手指，只要有任何一个手指在区域内就返回 true
bool touch_is_any_finger_in_zone(int l, int t, int r, int b) {
    std::lock_guard<std::mutex> guard(g_mutex);
    Zone zone;
    zone.l = l; zone.t = t; zone.r = r; zone.b = b;
    return isAnyPhysicalFingerInZoneLocked(zone);
}

bool touch_lift_joystick_finger(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || g_devices.empty()) return false;

    bool lifted = false;
    int touchMaxX = g_devices[0].absX.maximum;
    int touchMaxY = g_devices[0].absY.maximum;

    for (size_t d = 0; d < g_devices.size(); d++) {
        for (int f = 0; f < maxF; f++) {
            if (!g_devices[d].fingers[f].isDown) continue;
            if (d == 0 && (f == TOUCH_VIRTUAL_SLOT || f == TOUCH_TRIGGER_SLOT)) continue;

            float devX = g_devices[d].fingers[f].pos.x;
            float devY = g_devices[d].fingers[f].pos.y;
            int sx, sy;
            touchToScreen(devX, devY, touchMaxX, touchMaxY, sx, sy);

            if (pointInZone(g_joystick_zone, sx, sy)) {
                g_devices[d].fingers[f].isDown = false;
                lifted = true;
                LOGD("liftJoystickFinger: dev%zu finger%d at (%d,%d)", d, f, sx, sy);
            }
        }
    }
    return lifted;
}

// ─── Overlay UI input (physical finger → screen coords) ─────────────

bool touch_get_primary_finger(int* sx, int* sy, bool* down) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (sx) *sx = 0;
    if (sy) *sy = 0;
    if (down) *down = false;
    if (!g_initialized || g_devices.empty()) return false;

    const int touchMaxX = std::max(1, g_devices[0].absX.maximum);
    const int touchMaxY = std::max(1, g_devices[0].absY.maximum);

    for (size_t d = 0; d < g_devices.size(); d++) {
        for (int f = 0; f < maxF; f++) {
            if (!g_devices[d].fingers[f].isDown) continue;
            // 跳过注入的虚拟手指，只报告真实物理手指
            if (d == 0 && (f == TOUCH_VIRTUAL_SLOT || f == TOUCH_TRIGGER_SLOT)) continue;

            const TouchObj& finger = g_devices[d].fingers[f];
            int screenX = 0, screenY = 0;
            touchToScreen(finger.pos.x, finger.pos.y, touchMaxX, touchMaxY, screenX, screenY);
            if (sx) *sx = screenX;
            if (sy) *sy = screenY;
            if (down) *down = true;
            return true;
        }
    }
    return false;
}
