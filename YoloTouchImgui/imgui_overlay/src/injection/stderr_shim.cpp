// stderr_shim.cpp — 为预编译内核驱动（libtime_driver.a）提供 stderr 数据符号
//
// 该 .a 在 glibc 系环境编译，其 TimeDriver::Init() 内部用 fprintf(stderr, ...)
// 打错误日志，引用了外部数据符号 stderr。Android bionic 不导出 stderr 动态符号
// （stdin/stdout/stderr 在 stdio.h 中是宏或仅在运行时提供），NDK 链接时
// 会因 --no-undefined 报 "undefined symbol: stderr"。
//
// 这里显式定义 stderr 符号，并在构造函数里指向 /dev/null 的可写流：
// 驱动对 stderr 的写入被静默丢弃，既满足链接又不会崩溃。
//
// 注意：不能 include <stdio.h>，因为其 #define stderr (&__sF[2]) 在 API<23
// 目标下会生成对已移除符号 __sF 的引用，反而引入新的链接错误。
#include <fcntl.h>

extern "C" {
    struct __sFILE;
    typedef struct __sFILE FILE;
    // 定义符号本身（BSS，初始为 NULL，由构造函数赋值）
    FILE* stderr;
    // 手声明所需 libc 函数（避免 <stdio.h> 的 stderr/stdout 宏干扰）
    extern FILE* fopen(const char* path, const char* mode);
}

__attribute__((constructor))
static void initStderrShim(void) {
    FILE* sink = fopen("/dev/null", "w");
    if (sink) stderr = sink;
}
