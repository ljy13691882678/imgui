#pragma once

#include <cstdint>
#include <vector>

struct CapturedFrame {
    std::vector<uint8_t> rgba;   // RGBA8
    int width = 0;
    int height = 0;
};

// 通过 root 的 screencap 采集当前屏幕并解码为 RGBA。
// 失败返回 false，out 不保证有效。
bool ScreenCaptureRGBA(CapturedFrame &out);