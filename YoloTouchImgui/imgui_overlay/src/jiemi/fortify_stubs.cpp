// fortify_stubs.cpp — 提供 __fread_chk 兼容实现
// 预编译 libunicorn.a 的 grand.c.o 在 Bionic __FORTIFY_SOURCE 下把 fread 重定向为
// __fread_chk，而 CI 链接的 API-21 libc.so 不导出该符号（仅在更高 API 才导出），
// 导致 ld: undefined symbol: __fread_chk。这里补一个等价转发，避免链接失败。
// 该函数仅在被解密引擎触发时才会被调用，日常通常不会走到。
#include <stdio.h>

extern "C" size_t __fread_chk(void *ptr, size_t size, size_t nmemb, FILE *stream,
                              size_t bufsz)
{
    (void)bufsz;
    return fread(ptr, size, nmemb, stream);
}