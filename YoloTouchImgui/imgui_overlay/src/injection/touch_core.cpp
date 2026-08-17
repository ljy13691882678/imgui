// touch_core.cpp — Core touch injection logic
// Based on native_touch.cpp + reader threads from TouchHelperA
// Shared by JNI (Shizuku) and root_daemon (su)

#include "touch_core.h"
#include "time_driver_wrap.h"
#include "time_driver.h"   // TIME_GYRO_MASK_ALL 等常量
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <array>
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

static constexpr int maxE = 5;
static constexpr int maxF = 10;
static constexpr int UNGRAB = 0;
static constexpr int GRAB = 1;

// ─── Data structures ────────────────────────────────────────────────

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}
    Vec2 operator*(const Vec2& o) const { return {x * o.x, y * o.y}; }
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

struct InputBuffer {
    input_event event[512]{};
};

// ─── Global state ────────────────────────────────────────────────────

static std::vector<Device> g_devices;
static std::array<std::array<bool, maxF>, maxE> g_uploadedFingerDown{};
static InputBuffer g_inputBuffer{};
static Vec2 g_touchScale{1.0f, 1.0f};
static Vec2 g_screenSize{};
static std::mutex g_mutex;
static int g_outputFd = 0;
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

static void genRandomString(char* str, int len) {
    srand(static_cast<unsigned>(time(nullptr)) + len);
    for (int i = 0; i < len - 1; ++i) {
        int flag = rand() % 3;
        if (flag == 0)      str[i] = static_cast<char>('A' + rand() % 26);
        else if (flag == 1) str[i] = static_cast<char>('a' + rand() % 26);
        else                str[i] = static_cast<char>('0' + rand() % 10);
    }
    str[len - 1] = '\0';
}

static void pushEvent(int& count, unsigned short type, unsigned short code, int value) {
    if (count >= static_cast<int>(std::size(g_inputBuffer.event))) return;
    g_inputBuffer.event[count].type = type;
    g_inputBuffer.event[count].code = code;
    g_inputBuffer.event[count].value = value;
    ++count;
}

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
    // Mirror the helper behavior: ignore injected fingers and only judge real fingers.
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

// ─── Upload (from native_touch.cpp) ─────────────────────────────────
// 重要：只注入虚拟手指（自瞄/扳机等合成触摸），不重新注入物理手指。
// 物理触摸事件由 Android 原生输入管线直接送达系统/游戏，reader 线程仅被动读取
// 手指状态用于区域判断/自瞄触发门控，不再重新注入物理触摸。
// 这样避免了“抓取设备→重新注入”的链路延迟与冲突，物理触摸与 ImGui 都能稳定工作。
static void upload() {
    if (g_outputFd <= 0) return;
    int count = 0;
    int activeVirtualCount = 0;
    bool hasActiveVirtualFinger = false;

    // 只遍历虚拟手指槽位：TOUCH_VIRTUAL_SLOT(8), TOUCH_TRIGGER_SLOT(9)
    // 不重新注入物理手指（它们已通过原生输入管线到达系统）
    for (int fi = 0; fi < maxF; ++fi) {
        if (fi != TOUCH_VIRTUAL_SLOT && fi != TOUCH_TRIGGER_SLOT) continue;

        const TouchObj& finger = g_devices[0].fingers[fi];
        bool wasUploaded = g_uploadedFingerDown[0][fi];

        if (finger.isDown) {
            hasActiveVirtualFinger = true;
            ++activeVirtualCount;
            pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            if (!wasUploaded)
                pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, finger.id);
            pushEvent(count, EV_ABS, ABS_MT_POSITION_X, static_cast<int>(finger.pos.x));
            pushEvent(count, EV_ABS, ABS_MT_POSITION_Y, static_cast<int>(finger.pos.y));
            pushEvent(count, EV_ABS, ABS_X, static_cast<int>(finger.pos.x));
            pushEvent(count, EV_ABS, ABS_Y, static_cast<int>(finger.pos.y));
            g_uploadedFingerDown[0][fi] = true;
        } else if (wasUploaded) {
            pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, -1);
            g_uploadedFingerDown[0][fi] = false;
        }
    }

    pushEvent(count, EV_KEY, BTN_TOUCH, hasActiveVirtualFinger ? 1 : 0);
    pushEvent(count, EV_KEY, BTN_TOOL_FINGER, activeVirtualCount == 1 ? 1 : 0);
    pushEvent(count, EV_KEY, BTN_TOOL_DOUBLETAP, activeVirtualCount == 2 ? 1 : 0);
    pushEvent(count, EV_KEY, BTN_TOOL_TRIPLETAP, activeVirtualCount == 3 ? 1 : 0);
    pushEvent(count, EV_KEY, BTN_TOOL_QUADTAP, activeVirtualCount == 4 ? 1 : 0);
    pushEvent(count, EV_KEY, BTN_TOOL_QUINTTAP, activeVirtualCount >= 5 ? 1 : 0);
    pushEvent(count, EV_SYN, SYN_REPORT, 0);
    write(g_outputFd, g_inputBuffer.event, sizeof(input_event) * count);
}

// ─── Zone detection ─────────────────────────────────────────────────

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

// ─── uinput device creation ─────────────────────────────────────────

static bool createUinputDevice(int screenX, int screenY, int sourceFd) {
    uinput_user_dev uiDev{};
    g_outputFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_outputFd <= 0) {
        LOGE("open /dev/uinput failed");
        return false;
    }

    char randomName[16]{};
    genRandomString(randomName, sizeof(randomName));
    strncpy(uiDev.name, randomName, UINPUT_MAX_NAME_SIZE);
    uiDev.id.bustype = 0;
    uiDev.id.vendor = rand() % 10 + 5;
    uiDev.id.product = rand() % 10 + 5;
    uiDev.id.version = rand() % 10 + 5;

    ioctl(g_outputFd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    // 不使用 INPUT_PROP_DIRECT，否则游戏会优先处理虚拟触摸，忽略物理触摸
    // 使用 POINTER 属性，让虚拟手指被识别为指针设备，不干扰物理手指
    ioctl(g_outputFd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_X);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_Y);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_QUINTTAP);
    ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOUCH);

    char randomPhys[16]{};
    genRandomString(randomPhys, sizeof(randomPhys));
    ioctl(g_outputFd, UI_SET_PHYS, randomPhys);

    input_id id{};
    if (ioctl(sourceFd, EVIOCGID, &id) == 0) uiDev.id = id;

    uint8_t* bits = nullptr;
    ssize_t bitsSize = 0;
    int res = 0;
    while (true) {
        res = ioctl(sourceFd, EVIOCGBIT(EV_KEY, bitsSize), bits);
        if (res < bitsSize) break;
        bitsSize = res + 16;
        bits = static_cast<uint8_t*>(realloc(bits, bitsSize * 2));
    }
    for (int j = 0; j < res; ++j) {
        for (int k = 0; k < 8; ++k) {
            int code = j * 8 + k;
            if (bits[j] & (1 << k)) {
                if (code == BTN_TOUCH || code == BTN_TOOL_FINGER) continue;
                ioctl(g_outputFd, UI_SET_KEYBIT, code);
            }
        }
    }
    free(bits);

    uiDev.absmin[ABS_MT_SLOT] = 0;
    uiDev.absmax[ABS_MT_SLOT] = maxE * maxF - 1;
    uiDev.absmin[ABS_MT_POSITION_X] = 0;
    uiDev.absmax[ABS_MT_POSITION_X] = screenX;
    uiDev.absmin[ABS_MT_POSITION_Y] = 0;
    uiDev.absmax[ABS_MT_POSITION_Y] = screenY;
    uiDev.absmin[ABS_X] = 0;
    uiDev.absmax[ABS_X] = screenX;
    uiDev.absmin[ABS_Y] = 0;
    uiDev.absmax[ABS_Y] = screenY;
    uiDev.absmin[ABS_MT_TRACKING_ID] = 0;
    uiDev.absmax[ABS_MT_TRACKING_ID] = 65535;
    write(g_outputFd, &uiDev, sizeof(uiDev));

    if (ioctl(g_outputFd, UI_DEV_CREATE)) {
        LOGE("UI_DEV_CREATE failed");
        close(g_outputFd);
        g_outputFd = 0;
        return false;
    }
    return true;
}

// ─── Close ──────────────────────────────────────────────────────────

static void closeTouchLocked() {
    if (!g_initialized) return;
    for (auto& device : g_devices) {
        close(device.fd);
        device.fd = 0;
    }
    if (g_outputFd > 0) {
        ioctl(g_outputFd, UI_DEV_DESTROY);
        close(g_outputFd);
        g_outputFd = 0;
    }
    memset(g_inputBuffer.event, 0, sizeof(g_inputBuffer.event));
    g_uploadedFingerDown = {};
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

            if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
                upload();
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
            // 不再使用 EVIOCGRAB 抓取触摸设备。
            // 抓取会阻断 Android 原生输入管线，需要重新注入物理触摸，
            // 带来延迟/丢帧，导致“物理触摸有时不工作”甚至与 ImGui 冲突。
            // 改为被动监听（open 读模式）：物理触摸自然流向系统/游戏，
            // reader 线程仅用于读取手指状态供区域判断/自瞄触发门控，
            // 虚拟手指（aim/trigger）仍走 uinput 注入。
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

    // touch_init 只做设备扫描与坐标准备（供 ImGui 交互/区域判断），不创建 uinput
    // 注入设备——需要自瞄/扳机/压枪时由面板调用 touch_inject_init。
    // 触摸注入统一走 uinput（真实手指自然透传系统，合成手指经 uinput 注入）。

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
int  touch_get_output_fd(void)   { return g_outputFd; }
int  touch_device_count(void)    { return static_cast<int>(g_devices.size()); }

// ─── uinput 注入设备生命周期 ───
// uinput 注入设备是否已就绪（触摸注入可用）
bool touch_inject_ready(void) { return g_initialized && g_outputFd > 0; }

// 按需创建 uinput 注入设备（触摸注入就绪）。幂等：已就绪直接返回 true。
// 触摸注入统一走 uinput；内核陀螺仪模式下不调用本函数（屏蔽 uinput 初始化）。
bool touch_inject_init(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || g_devices.empty()) return false;
    if (g_outputFd > 0) return true;  // 已就绪
    const int touchMaxX = std::max(1, g_devices[0].absX.maximum);
    const int touchMaxY = std::max(1, g_devices[0].absY.maximum);
    if (!createUinputDevice(touchMaxX, touchMaxY, g_devices[0].fd)) {
        LOGE("touch inject init failed (need root + /dev/uinput)");
        return false;
    }
    LOGD("touch inject ready (uinput)");
    return true;
}

// 销毁 uinput 注入设备（停止触摸注入）
void touch_inject_close(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_outputFd > 0) {
        ioctl(g_outputFd, UI_DEV_DESTROY);
        close(g_outputFd);
        g_outputFd = 0;
    }
    g_uploadedFingerDown = {};
    LOGD("touch inject closed");
}

// ─── 内核陀螺仪（TimeDriver，仅陀螺仪；触摸注入统一走 uinput） ───
bool touch_kernel_gyro_init(void) {
    // 惰性连接驱动 + 关闭触摸接管 + 初始化陀螺仪 hook（内部已做幂等）
    return kdrv_gyro_init();
}

void touch_gyro_apply(bool enable, float pitch, float yaw) {
    // 重要：陀螺仪注入【不依赖 uinput】。陀螺仪模式正是要屏蔽 uinput 的触摸及初始化，
    // 若此处以 g_initialized（uinput 初始化状态）门控，uinput 未初始化时注入会被静默
    // 跳过，导致“对接了也没效果”。这里只依赖驱动连接状态。
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

void touch_down(int slot, int id, int screenX, int screenY) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || g_devices.empty()) return;
    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_devices[0].fingers[slot].id = id;
    g_devices[0].fingers[slot].pos = Vec2(tx, ty);
    g_devices[0].fingers[slot].isDown = true;
    upload();
}

void touch_move(int slot, int screenX, int screenY) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || g_devices.empty()) return;
    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_devices[0].fingers[slot].pos = Vec2(tx, ty);
    upload();
}

void touch_up(int slot) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || g_devices.empty()) return;
    g_devices[0].fingers[slot].isDown = false;
    upload();
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

    if (lifted) upload();
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
