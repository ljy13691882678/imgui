#ifdef ENABLE_YOLO
#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/c_api_types.h"
#include "tensorflow/lite/delegates/nnapi/nnapi_delegate_c.h"
#endif

#include "Yolo/YoloDetector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>

YoloDetector::YoloDetector() = default;
YoloDetector::~YoloDetector() { Unload(); }

void YoloDetector::Unload() {
#ifdef ENABLE_YOLO
    if (interpreter_) TfLiteInterpreterDelete((TfLiteInterpreter *)interpreter_);
    if (model_) TfLiteModelDelete((TfLiteModel *)model_);
    interpreter_ = nullptr;
    model_ = nullptr;
#endif
}

bool YoloDetector::LoadModel(const std::string &path) {
#ifdef ENABLE_YOLO
    Unload();

    TfLiteModel *model = TfLiteModelCreateFromFile(path.c_str());
    if (!model) {
        lastError_ = "TfLiteModelCreateFromFile failed: " + path;
        return false;
    }

    TfLiteInterpreterOptions *opts = TfLiteInterpreterOptionsCreate();
    if (!opts) {
        TfLiteModelDelete(model);
        lastError_ = "TfLiteInterpreterOptionsCreate failed";
        return false;
    }
    TfLiteInterpreterOptionsSetNumThreads(opts, 2);

    // 启用 NNAPI delegate：优先走骁龙 Hexagon NPU，不支持的算子自动回落 CPU
    TfLiteDelegate *nnapi = TfLiteGetNNAPIDelegate();
    if (nnapi) {
        TfLiteInterpreterOptionsAddDelegate(opts, nnapi);
        printf("[yolo] NNAPI delegate enabled\n");
    } else {
        printf("[yolo] NNAPI delegate unavailable, fallback CPU\n");
    }

    TfLiteInterpreter *interp = TfLiteInterpreterCreate(model, opts);
    TfLiteInterpreterOptionsDelete(opts);
    if (!interp) {
        TfLiteModelDelete(model);
        lastError_ = "TfLiteInterpreterCreate failed";
        return false;
    }
    if (TfLiteInterpreterAllocateTensors(interp) != kTfLiteOk) {
        TfLiteInterpreterDelete(interp);
        TfLiteModelDelete(model);
        lastError_ = "AllocateTensors failed";
        return false;
    }

    // 读取输入尺寸
    const TfLiteTensor *in = TfLiteInterpreterGetInputTensor(interp, 0);
    if (!in || TfLiteTensorType(in) != kTfLiteUInt8) {
        // 输入必须是 uint8 int8 量化
        TfLiteInterpreterDelete(interp);
        TfLiteModelDelete(model);
        lastError_ = "input tensor not uint8";
        return false;
    }
    int32_t ndims = TfLiteTensorNumDims(in);
    if (ndims >= 4) {
        inputH = TfLiteTensorDim(in, 1);
        inputW = TfLiteTensorDim(in, 2);
    } else {
        inputH = inputW = 0;
    }

    // 输出形状推断：YOLOv8 常见 [1, 4+nc, N] 或 [1, N, 4+nc]
    const TfLiteTensor *out = TfLiteInterpreterGetOutputTensor(interp, 0);
    if (out) {
        int32_t od = TfLiteTensorNumDims(out);
        if (od >= 2) {
            int32_t d1 = TfLiteTensorDim(out, 1);
            int32_t d2 = TfLiteTensorDim(out, 2);
            int small = std::min(d1, d2);
            int big = std::max(d1, d2);
            if (small > 4) numClass_ = small - 4;
        }
    }

    model_ = model;
    interpreter_ = interp;
    inputBuf_.resize((size_t)inputW * inputH * 3);
    printf("[yolo] model loaded: %dx%d, numClass=%d\n", inputW, inputH, numClass_);
    return true;
#else
    (void)path;
    lastError_ = "YOLO not compiled in (ENABLE_YOLO off)";
    return false;
#endif
}

// 双线性插值 RGBA -> RGB 并缩放到模型输入尺寸
static void Preprocess(const uint8_t *rgba, int sw, int sh, int dw, int dh, uint8_t *out) {
    for (int y = 0; y < dh; y++) {
        float sy = (sh > 1) ? (y + 0.5f) * sh / dh - 0.5f : 0.0f;
        sy = std::max(0.0f, std::min(sy, (float)(sh - 1)));
        int y0 = (int)sy;
        int y1 = std::min(y0 + 1, sh - 1);
        float fy = sy - y0;
        for (int x = 0; x < dw; x++) {
            float sx = (sw > 1) ? (x + 0.5f) * sw / dw - 0.5f : 0.0f;
            sx = std::max(0.0f, std::min(sx, (float)(sw - 1)));
            int x0 = (int)sx;
            int x1 = std::min(x0 + 1, sw - 1);
            float fx = sx - x0;
            for (int c = 0; c < 3; c++) {
                float v = (1 - fy) * (1 - fx) * rgba[(y0 * sw + x0) * 4 + c]
                        + (1 - fy) * fx      * rgba[(y0 * sw + x1) * 4 + c]
                        + fy      * (1 - fx) * rgba[(y1 * sw + x0) * 4 + c]
                        + fy      * fx       * rgba[(y1 * sw + x1) * 4 + c];
                out[(y * dw + x) * 3 + c] = (uint8_t)std::lround(v);
            }
        }
    }
}

std::vector<Detection> YoloDetector::Detect(const uint8_t *rgba, int screenW, int screenH) {
#ifdef ENABLE_YOLO
    if (!interpreter_ || inputW <= 0)
        return {};

    double t0 = (double)clock() / CLOCKS_PER_SEC;

    Preprocess(rgba, screenW, screenH, inputW, inputH, inputBuf_.data());

    const TfLiteTensor *in = TfLiteInterpreterGetInputTensor((TfLiteInterpreter *)interpreter_, 0);
    // C API 的输入张量访问器返回 const，写入需要 const_cast
    TfLiteTensorCopyFromBuffer(const_cast<TfLiteTensor *>(in), inputBuf_.data(), inputBuf_.size());

    double t1 = (double)clock() / CLOCKS_PER_SEC;
    if (TfLiteInterpreterInvoke((TfLiteInterpreter *)interpreter_) != kTfLiteOk) {
        printf("[yolo] invoke failed\n");
        return {};
    }
    double t2 = (double)clock() / CLOCKS_PER_SEC;
    lastInferMs = (t2 - t1) * 1000.0;

    const TfLiteTensor *out = TfLiteInterpreterGetOutputTensor((TfLiteInterpreter *)interpreter_, 0);
    if (!out) return {};
    int32_t total = TfLiteTensorByteSize(out) / TfLiteTypeSize(TfLiteTensorType(out));
    const void *raw = TfLiteTensorData(out);
    if (!raw) return {};

    // 输出可能是 float32，也可能是 int8/uint8 量化输出，需反量化
    std::vector<float> outBuf;
    const float *data = nullptr;
    TfLiteType otype = TfLiteTensorType(out);
    if (otype == kTfLiteFloat32) {
        data = (const float *)raw;
    } else if (otype == kTfLiteInt8 || otype == kTfLiteUInt8) {
        float scale = TfLiteTensorQuantizationParams(out).scale;
        int zp = TfLiteTensorQuantizationParams(out).zero_point;
        outBuf.resize(total);
        if (otype == kTfLiteInt8) {
            const int8_t *p = (const int8_t *)raw;
            for (int i = 0; i < total; i++) outBuf[i] = ((float)p[i] - zp) * scale;
        } else {
            const uint8_t *p = (const uint8_t *)raw;
            for (int i = 0; i < total; i++) outBuf[i] = ((float)p[i] - zp) * scale;
        }
        data = outBuf.data();
    } else {
        printf("[yolo] unsupported output type %d\n", (int)otype);
        return {};
    }

    int32_t od = TfLiteTensorNumDims(out);
    int d1 = od >= 2 ? TfLiteTensorDim(out, 1) : 0;
    int d2 = od >= 3 ? TfLiteTensorDim(out, 2) : 0;
    int small = std::min(d1, d2);
    int big = std::max(d1, d2);
    int nc = (small > 4) ? (small - 4) : numClass_;
    int N = big;
    bool transposed = (d2 == small);   // [1,N,4+nc] 时 last dim 为小维度

    std::vector<Detection> boxes = PostProcess(data, total, N, transposed);

    // 归一化/网格坐标 -> 屏幕像素
    for (auto &b : boxes) {
        b.x1 = b.x1 * screenW;
        b.x2 = b.x2 * screenW;
        b.y1 = b.y1 * screenH;
        b.y2 = b.y2 * screenH;
    }

    double t3 = (double)clock() / CLOCKS_PER_SEC;
    lastTotalMs = (t3 - t0) * 1000.0;
    return boxes;
#else
    (void)rgba; (void)screenW; (void)screenH;
    return {};
#endif
}

std::vector<Detection> YoloDetector::PostProcess(const float *out, int total, int N, bool transposed) {
    int nc = numClass_;
    int stride = 4 + nc;
    std::vector<Detection> dets;

    // 先收集所有高于阈值的框
    struct Tmp { float x1,y1,x2,y2,score; int cls; };
    std::vector<Tmp> cand;
    cand.reserve(N);
    for (int i = 0; i < N; i++) {
        float cx, cy, w, h;
        const float *base;
        if (transposed) {
            base = out + (size_t)i * stride;
            cx = base[0]; cy = base[1]; w = base[2]; h = base[3];
        } else {
            cx = out[i]; cy = out[(size_t)N + i]; w = out[(size_t)2 * N + i]; h = out[(size_t)3 * N + i];
        }
        // 找最佳类别
        int bestC = -1;
        float bestS = 0.0f;
        for (int c = 0; c < nc; c++) {
            float s = transposed ? base[4 + c] : out[(size_t)(4 + c) * N + i];
            if (s > bestS) { bestS = s; bestC = c; }
        }
        if (bestC < 0 || bestS < conf_) continue;
        // 归一化坐标（推理输出通常已是 0..1）
        float x1 = (cx - w * 0.5f);
        float y1 = (cy - h * 0.5f);
        float x2 = (cx + w * 0.5f);
        float y2 = (cy + h * 0.5f);
        cand.push_back({x1,y1,x2,y2,bestS,bestC});
    }

    // NMS
    std::sort(cand.begin(), cand.end(), [](const Tmp &a, const Tmp &b){ return a.score > b.score; });
    std::vector<bool> suppressed(cand.size(), false);
    for (size_t i = 0; i < cand.size(); i++) {
        if (suppressed[i]) continue;
        dets.push_back({cand[i].x1, cand[i].y1, cand[i].x2, cand[i].y2, cand[i].score, cand[i].cls});
        for (size_t j = i + 1; j < cand.size(); j++) {
            if (suppressed[j]) continue;
            if (cand[i].cls != cand[j].cls) continue;
            float ix = std::max(0.0f, std::min(cand[i].x2, cand[j].x2) - std::max(cand[i].x1, cand[j].x1));
            float iy = std::max(0.0f, std::min(cand[i].y2, cand[j].y2) - std::max(cand[i].y1, cand[j].y1));
            float inter = ix * iy;
            float ai = (cand[i].x2 - cand[i].x1) * (cand[i].y2 - cand[i].y1);
            float aj = (cand[j].x2 - cand[j].x1) * (cand[j].y2 - cand[j].y1);
            float u = ai + aj - inter;
            if (u <= 0) continue;
            if (inter / u > iou_) suppressed[j] = true;
        }
    }
    (void)total;
    return dets;
}