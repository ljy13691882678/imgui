#include "qnn_engine.h"
#include "common.h"
#include <qnn/TFLiteDelegate/QnnTFLiteDelegate.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <set>
#include <vector>
#include <elf.h>
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

// 拷贝文件（用于把 vendor 系统库拷到 app 库目录，绕过 linker namespace 限制）
static bool copyFile(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    fclose(out);
    return ok;
}

// 文件是否存在且可读
static bool fileReadable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// 将 ELF 虚拟地址映射为文件偏移（扫描 PT_LOAD 段）
static bool elfVaddrToOffset(FILE* f, const Elf64_Ehdr& ehdr, uint64_t vaddr, uint64_t& out) {
    for (int i = 0; i < ehdr.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (fseek(f, (long)(ehdr.e_phoff + i * ehdr.e_phentsize), SEEK_SET) != 0) continue;
        if (fread(&ph, sizeof(ph), 1, f) != 1) continue;
        if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && vaddr < ph.p_vaddr + ph.p_filesz) {
            out = ph.p_offset + (vaddr - ph.p_vaddr);
            return true;
        }
    }
    return false;
}

// 读取 ELF64 的 DT_NEEDED 依赖名（通过 PT_DYNAMIC 段，不依赖可能被裁剪的节表）
static bool readElfNeeded(const char* path, std::set<std::string>& deps) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    Elf64_Ehdr ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, f) == 1 &&
        memcmp(ehdr.e_ident, ELFMAG, SELFMAG) == 0 &&
        ehdr.e_ident[EI_CLASS] == ELFCLASS64) {
        uint64_t dynOff = 0, dynSize = 0;
        for (int i = 0; i < ehdr.e_phnum; ++i) {
            Elf64_Phdr ph;
            if (fseek(f, (long)(ehdr.e_phoff + i * ehdr.e_phentsize), SEEK_SET) != 0) break;
            if (fread(&ph, sizeof(ph), 1, f) != 1) break;
            if (ph.p_type == PT_DYNAMIC) {
                if (elfVaddrToOffset(f, ehdr, ph.p_vaddr, dynOff)) dynSize = ph.p_filesz;
                break;
            }
        }
        if (dynOff) {
            std::vector<Elf64_Dyn> dyn(dynSize / sizeof(Elf64_Dyn));
            if (fseek(f, (long)dynOff, SEEK_SET) == 0 &&
                fread(dyn.data(), sizeof(Elf64_Dyn), dyn.size(), f) == dyn.size()) {
                uint64_t strOff = 0;
                for (size_t i = 0; i < dyn.size() && dyn[i].d_tag != DT_NULL; ++i) {
                    if (dyn[i].d_tag == DT_STRTAB) {
                        elfVaddrToOffset(f, ehdr, dyn[i].d_un.d_ptr, strOff);
                        break;
                    }
                }
                if (strOff) {
                    for (size_t i = 0; i < dyn.size() && dyn[i].d_tag != DT_NULL; ++i) {
                        if (dyn[i].d_tag != DT_NEEDED) continue;
                        char name[256];
                        if (fseek(f, (long)(strOff + dyn[i].d_un.d_val), SEEK_SET) == 0 &&
                            fread(name, 1, sizeof(name) - 1, f) > 0) {
                            name[sizeof(name) - 1] = '\0';
                            deps.insert(name);
                        }
                    }
                    ok = true;
                }
            }
        }
    }
    fclose(f);
    return ok;
}

// 递归把 name 及其所有缺失的系统依赖拷贝到 libDir，绕过 linker namespace 隔离。
// 已在默认/app 命名空间可按名加载的库（系统公共库、APK 自带库）跳过。
static void ensureLibDeps(const char* name, const std::string& libDir,
                          const char* const* sysDirs, size_t sysDirCount,
                          std::set<std::string>& visited) {
    if (!visited.insert(name).second) return;
    std::string dst = libDir + "/" + name;
    if (!fileReadable(dst.c_str())) {
        for (size_t i = 0; i < sysDirCount; ++i) {
            std::string src = std::string(sysDirs[i]) + name;
            if (fileReadable(src.c_str()) && copyFile(src.c_str(), dst.c_str())) break;
        }
    }
    if (!fileReadable(dst.c_str())) return;
    std::set<std::string> deps;
    if (!readElfNeeded(dst.c_str(), deps)) return;
    for (const auto& dep : deps) {
        void* h = dlopen(dep.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h) { dlclose(h); continue; }
        ensureLibDeps(dep.c_str(), libDir, sysDirs, sysDirCount, visited);
    }
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
        std::string libDir(getSkelLibDir());
        // libcdsprpc/libadsprpc 无法直接 dlopen（linker namespace 隔离）。
        // 把它们连同 ELF 依赖（如 vendor.qti.hardware.dsp-V1-ndk.so）从
        // vendor/system 拷贝到 app 库目录，再由同一目录加载，依赖即可解析。
        const char* rpcLibs[] = { "libcdsprpc.so", "libadsprpc.so" };
        const char* sysDirs[] = {
            "/vendor/lib64/", "/vendor/lib64/cdsp/",
            "/system/lib64/", "/system/lib64/cdsp/",
            "/odm/lib64/", "/vendor/lib64/hw/",
        };
        for (const char* name : rpcLibs) {
            std::set<std::string> visited;
            ensureLibDeps(name, libDir, sysDirs,
                          sizeof(sysDirs) / sizeof(sysDirs[0]), visited);
            std::string dst = libDir + "/" + name;
            if (!fileReadable(dst.c_str())) {
                appendDiag("[QNN] %s 未找到可拷贝的系统库，加载失败", name);
                continue;
            }
            void* h = dlopen(dst.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (h) {
                LOGD("%s preloaded (deps copied to %s)", name, libDir.c_str());
                appendDiag("[QNN] %s 依赖已拷贝到 %s 并加载 OK", name, libDir.c_str());
            } else {
                const char* e = dlerror();
                LOGW("%s dlopen failed: %s", dst.c_str(), e ? e : "(null)");
                appendDiag("[QNN] %s 加载失败: %s", name, e ? e : "(null)");
            }
        }

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

    // 预加载本包内的 stub 库（绝对路径）并保持加载：stub 依赖
    // libcdsprpc.so，cdsprpc 已在上方拷贝到同一目录，依赖可解析。
    // skel 库无需 dlopen（由 QNN 通过 fastRPC 传给 DSP）。
    const char* stubLibs[] = {
        "libQnnHtpV69Stub.so",
        "libQnnHtpV73Stub.so",
        "libQnnHtpV75Stub.so",
        "libQnnHtpV79Stub.so",
        "libQnnHtpV81Stub.so",
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

