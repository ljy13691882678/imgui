#include "qnn_engine.h"
#include "common.h"
#include <qnn/TFLiteDelegate/QnnTFLiteDelegate.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <sys/stat.h>

// 逐级创建目录（QNN context binary cache 需要真实存在的可写目录）
static void ensureDir(const char* path) {
    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    mkdir(buf, 0755);
}

//==============================================================================
//  Helper: Get native library directory
//==============================================================================
static const char* getNativeLibDir() {
    static char dir[512] = {0};
    if (dir[0]) return dir;
    Dl_info info;
    if (dladdr((void*)getNativeLibDir, &info)) {
        std::string libPath(info.dli_fname);
        size_t pos = libPath.find_last_of('/');
        if (pos != std::string::npos) {
            strncpy(dir, libPath.substr(0, pos).c_str(), sizeof(dir) - 1);
        }
    }
    return dir;
}

// QNN HTP skel 库（libQnnHtpV*Skel.so）所在目录。
// 优先取 LD_LIBRARY_PATH（APK 启动时注入 .so 目录），该路径才是真正的
// 库目录；dladdr 拿到的是可执行文件所在目录（<filesDir>/bin），不含 skel 库。
static const char* getSkelLibDir() {
    static char dir[512] = {0};
    if (dir[0]) return dir;

    const char* ld = getenv("LD_LIBRARY_PATH");
    if (ld && ld[0]) {
        // LD_LIBRARY_PATH 可能为冒号分隔的多目录，QNN 只接受单个目录，取第一个
        size_t len = strcspn(ld, ":");
        if (len > 0 && len < sizeof(dir)) {
            memcpy(dir, ld, len);
            dir[len] = '\0';
            return dir;
        }
    }
    strncpy(dir, getNativeLibDir(), sizeof(dir) - 1);
    return dir;
}

//==============================================================================
//  Hardware Detection
//==============================================================================
bool QnnEngine::isQualcommSnapdragon() {
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "Qualcomm") || strstr(line, "qcom") || strstr(line, "Snapdragon")) {
                fclose(f);
                return true;
            }
        }
        fclose(f);
    }
    FILE* p = popen("getprop ro.hardware", "r");
    if (p) {
        char line[128];
        if (fgets(line, sizeof(line), p)) {
            if (strstr(line, "qcom")) {
                pclose(p);
                return true;
            }
        }
        pclose(p);
    }
    return false;
}

//==============================================================================
//  Lifecycle
//==============================================================================
QnnEngine::QnnEngine() = default;

QnnEngine::~QnnEngine() {
    deleteDelegate();
}

//==============================================================================
//  Build Delegate
//==============================================================================
TfLiteDelegate* QnnEngine::buildDelegate() {
    m_diag.clear();

    // dlopen 测试关键 QNN 库：任一加载失败都会导致 HTP backend 初始化失败
    auto appendDiag = [this](const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        m_diag += buf;
        m_diag += "\n";
    };

    if (!isQualcommSnapdragon()) {
        LOGW("Non-Qualcomm CPU detected, skipping QNN HTP");
        appendDiag("[QNN] 非高通芯片，跳过 HTP");
        return nullptr;
    }
    appendDiag("[QNN] 检测到高通芯片");

    // fastRPC 用户态库（HTP 与 DSP 通信必需，系统库）
    if (!m_preloaded) {
        // libcdsprpc/libadsprpc 为系统库：先按名（LD_LIBRARY_PATH），
        // 再按常见系统绝对路径加载。成功后保持加载（不 dlclose），
        // 供后续 stub 库依赖解析使用。
        auto preloadSystemLib = [&](const char* name) {
            void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                LOGD("%s preloaded", name);
                appendDiag("[QNN] %s 加载 OK（按名）", name);
                return;
            }
            const char* e1 = dlerror();
            const char* dirs[] = {
                "/vendor/lib64/", "/system/lib64/",
                "/vendor/lib64/cdsp/", "/system/lib64/cdsp/", "/lib64/",
            };
            for (const char* d : dirs) {
                std::string full = std::string(d) + name;
                h = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (h) {
                    LOGD("%s preloaded (%s)", name, full.c_str());
                    appendDiag("[QNN] %s 加载 OK（%s）", name, full.c_str());
                    return;
                }
                const char* e = dlerror();
                appendDiag("[QNN] %s 尝试 %s 失败: %s", name, full.c_str(), e ? e : "(null)");
            }
            appendDiag("[QNN] %s 按名失败: %s", name, e1 ? e1 : "(null)");
        };
        preloadSystemLib("libcdsprpc.so");
        preloadSystemLib("libadsprpc.so");

        strncpy(m_native_lib_dir, getNativeLibDir(), sizeof(m_native_lib_dir) - 1);
        LOGD("Native lib dir: %s", m_native_lib_dir);
        m_preloaded = true;
    }

    // QNN HTP context binary cache 目录必须存在，否则 backend 初始化失败
    const char* cachePath = "/data/data/com.yolotouch.imgui/cache/qnn";
    ensureDir(cachePath);
    appendDiag("[QNN] cache 目录: %s", cachePath);

    const char* skelDir = getSkelLibDir();
    appendDiag("[QNN] skel 库目录: %s", skelDir);

    // 用绝对路径预加载本包内的 stub/skel 库并保持加载（不 dlclose）：
    // QNN 内部按名 dlopen stub 时能命中已加载句柄，避免因按名查找
    // 或依赖解析失败导致 HTP backend 初始化失败。
    const char* stubLibs[] = {
        "libQnnHtpV69Stub.so", "libQnnHtpV69Skel.so",
        "libQnnHtpV73Stub.so", "libQnnHtpV73Skel.so",
        "libQnnHtpV75Stub.so", "libQnnHtpV75Skel.so",
        "libQnnHtpV79Stub.so", "libQnnHtpV79Skel.so",
        "libQnnHtpV81Stub.so", "libQnnHtpV81Skel.so",
    };
    for (const char* lib : stubLibs) {
        std::string full = std::string(skelDir) + "/" + lib;
        void* h = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h) {
            LOGD("%s preloaded", lib);
            appendDiag("[QNN] %s 预加载 OK", lib);
        } else {
            const char* e = dlerror();
            LOGW("%s dlopen failed: %s", full.c_str(), e ? e : "(null)");
            appendDiag("[QNN] %s 预加载失败: %s", lib, e ? e : "(null)");
        }
    }

    // 基础 QNN 库按名确认可加载
    const char* qnnLibs[] = {
        "libQnnHtp.so",
        "libQnnSystem.so",
        "libQnnTFLiteDelegate.so",
    };
    for (const char* lib : qnnLibs) {
        void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            appendDiag("[QNN] %s 加载 OK", lib);
            dlclose(h);
        } else {
            const char* e = dlerror();
            LOGW("%s dlopen failed: %s", lib, e ? e : "(null)");
            appendDiag("[QNN] %s 加载失败: %s", lib, e ? e : "(null)");
        }
    }

    TfLiteQnnDelegateOptions qnn_options = TfLiteQnnDelegateOptionsDefault();
    qnn_options.backend_type = kHtpBackend;
    qnn_options.skel_library_dir = skelDir;
    qnn_options.cache_dir = cachePath;
    qnn_options.model_token = "yolov8n_htp_v1";
    // 打开 QNN delegate/backend 日志，便于定位 HTP 初始化失败原因
    qnn_options.log_level = kLogLevelWarn;
    // 默认模型为 int8 量化，HTP 用量化精度（默认 kHtpFp16 对量化模型不友好）
    qnn_options.htp_options.precision = kHtpQuantized;
    qnn_options.htp_options.performance_mode = kHtpSustainedHighPerformance;

    // HTP 运行时能力检测：1=支持 0=不支持
    TfLiteQnnDelegateCapabilityStatus capFp16 = TfLiteQnnDelegateHasCapability(kCapHtpRuntimeFp16);
    TfLiteQnnDelegateCapabilityStatus capQuant = TfLiteQnnDelegateHasCapability(kCapHtpRuntimeQuant);
    LOGD("QNN HTP capability fp16=%d quant=%d (1=supported)", capFp16, capQuant);
    appendDiag("[QNN] HTP 能力 fp16=%d quant=%d (1=支持)", capFp16, capQuant);

    m_delegate = TfLiteQnnDelegateCreate(&qnn_options);
    if (m_delegate) {
        LOGD("QNN HTP delegate created");
        appendDiag("[QNN] HTP delegate 创建成功");
        // 高性能投票（默认 kHtpPerfCtrlManual 策略下立即生效）
        TfLiteQnnDelegateSetPerf(m_delegate, kPerformanceVote);
    } else {
        LOGW("QNN HTP delegate creation failed");
        appendDiag("[QNN] HTP delegate 创建失败！(原因见 QNN backend 日志)");
    }
    return m_delegate;
}

void QnnEngine::deleteDelegate() {
    if (m_delegate) {
        TfLiteQnnDelegateDelete(m_delegate);
        m_delegate = nullptr;
    }
}

