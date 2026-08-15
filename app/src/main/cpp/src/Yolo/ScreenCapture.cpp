#include "Yolo/ScreenCapture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "stb_image.h"  // 回退 PNG 路径使用；实现已在 stb_image.cpp 编译，此处仅声明

// 读取固定 n 字节，只有读满才返回 true
static bool ReadAll(FILE *fp, void *dst, size_t n) {
    return fread(dst, 1, n, fp) == n;
}

// 通过 root 的 screencap 采集当前屏幕并解码为 RGBA。
// 优先使用 raw 格式（无 PNG 编码/解码，速度快得多）：
//   screencap 不加 -p 时输出 12 字节头（width/height/pixelformat，均为 uint32 LE）+ 原始像素。
//   pixelformat: 1=RGBA_8888, 2=RGBX_8888（均为 4 字节/像素，第 4 字节可忽略）。
//   其它格式或读取失败时回退到 PNG。
bool ScreenCaptureRGBA(CapturedFrame &out) {
    // ---- 优先 raw ----
    FILE *fp = popen("screencap", "r");
    if (fp) {
        uint32_t hdr[3];
        if (ReadAll(fp, hdr, sizeof(hdr))) {
            uint32_t w = hdr[0], h = hdr[1], fmt = hdr[2];
            if (fmt == 1 || fmt == 2) {  // RGBA_8888 / RGBX_8888
                size_t bytes = (size_t)w * h * 4;
                out.rgba.resize(bytes);
                if (ReadAll(fp, out.rgba.data(), bytes)) {
                    out.width = (int)w;
                    out.height = (int)h;
                    pclose(fp);
                    return true;
                }
            }
        }
        pclose(fp);
    }

    // ---- 回退：PNG ----
    FILE *pngfp = popen("screencap -p", "r");
    if (!pngfp) return false;

    std::vector<unsigned char> png;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pngfp)) > 0) {
        png.insert(png.end(), buf, buf + n);
    }
    int rc = pclose(pngfp);
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