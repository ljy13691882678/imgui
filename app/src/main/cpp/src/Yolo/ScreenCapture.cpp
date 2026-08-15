#include "Yolo/ScreenCapture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stb_image.h"  // 实现已在 stb_image.cpp 编译，此处仅声明

// 通过 root 的 screencap 输出 PNG 到 stdout，再解码为 RGBA
bool ScreenCaptureRGBA(CapturedFrame &out) {
    FILE *fp = popen("screencap -p", "r");
    if (!fp) return false;

    std::vector<unsigned char> png;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        png.insert(png.end(), buf, buf + n);
    }
    int rc = pclose(fp);
    if (rc != 0 || png.empty()) return false;

    int w = 0, h = 0, ch = 0;
    unsigned char *px = stbi_load_from_memory(png.data(), (int)png.size(),
                                              &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) return false;

    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}