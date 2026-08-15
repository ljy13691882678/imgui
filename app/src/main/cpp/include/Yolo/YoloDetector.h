#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 单个检测框（像素坐标，相对屏幕左上角）
struct Detection {
    float x1, y1, x2, y2;   // 左上/右下角（像素）
    float score;
    int   classId;
};

// YOLO 推理封装：TensorFlow Lite C API + NNAPI delegate（骁龙 Hexagon NPU 加速）
// 模型格式：int8 量化 .tflite
class YoloDetector {
public:
    YoloDetector();
    ~YoloDetector();

    bool LoadModel(const std::string &path);
    void Unload();
    bool IsLoaded() const { return interpreter_ != nullptr; }

    // 输入 RGBA 屏幕帧(rgba, 4 字节), 输出像素坐标检测框
    std::vector<Detection> Detect(const uint8_t *rgba, int screenW, int screenH);

    void SetConfidence(float c) { conf_ = c; }
    void SetIou(float iou) { iou_ = iou; }
    float Confidence() const { return conf_; }
    float Iou() const { return iou_; }
    void SetNumClass(int n) { numClass_ = n; }
    const char *LastError() const { return lastError_.c_str(); }

    // 统计
    double lastInferMs = 0.0;   // 单次推理耗时
    double lastTotalMs = 0.0;   // 含采集+预处理+后处理总耗时
    int inputW = 0;
    int inputH = 0;

private:
    std::vector<Detection> PostProcess(const float *out, int total, int N, bool transposed);

    void    *model_ = nullptr;        // TfLiteModel*
    void    *interpreter_ = nullptr;  // TfLiteInterpreter*
    std::string lastError_;
    std::vector<uint8_t> inputBuf_;
    float conf_ = 0.25f;
    float iou_ = 0.45f;
    int   numClass_ = 80;
};