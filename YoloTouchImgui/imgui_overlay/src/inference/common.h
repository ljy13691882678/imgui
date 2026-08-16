#pragma once

#include <vector>
#include <algorithm>
#include <memory>
#include <cstring>
#include <cmath>
#include <sys/time.h>
#include <android/log.h>

#define LOG_TAG "Aimbot"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

struct Detection {
    float x1, y1, x2, y2, score, classId;
};

inline long long getTimeUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

inline std::vector<Detection> nms(std::vector<Detection>& boxes, float iouThreshold) {
    if (boxes.empty()) return {};

    std::sort(boxes.begin(), boxes.end(),
        [](const Detection& a, const Detection& b) {
            return a.score > b.score;
        });

    auto suppressed = std::make_unique<uint8_t[]>(boxes.size());
    memset(suppressed.get(), 0, boxes.size());
    std::vector<Detection> result;
    result.reserve(boxes.size());

    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(boxes[i]);

        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;

            float x1a = boxes[i].x1, y1a = boxes[i].y1;
            float x2a = boxes[i].x2, y2a = boxes[i].y2;
            float x1b = boxes[j].x1, y1b = boxes[j].y1;
            float x2b = boxes[j].x2, y2b = boxes[j].y2;

            float interW = std::max(0.0f, std::min(x2a, x2b) - std::max(x1a, x1b));
            float interH = std::max(0.0f, std::min(y2a, y2b) - std::max(y1a, y1b));
            float interArea = interW * interH;
            float unionArea = (x2a - x1a) * (y2a - y1a) + (x2b - x1b) * (y2b - y1b) - interArea;

            if (unionArea > 0 && interArea / unionArea > iouThreshold &&
                boxes[i].classId == boxes[j].classId) {
                suppressed[j] = 1;
            }
        }
    }

    return result;
}
