#pragma once

#include "inference_engine.h"
#include "qnn_engine.h"
#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/c/common.h>
#include <tensorflow/lite/delegates/gpu/delegate.h>
#include <functional>
#include <string>

class LiteRtEngine : public InferenceEngine {
public:
    LiteRtEngine();
    ~LiteRtEngine() override;

    bool init(const char* model_path) override;
    void release() override;

    std::vector<Detection> detect(
        uint8_t* src,
        int offsetX, int offsetY,
        int regionWidth, int regionHeight,
        int screenWidth, int screenHeight,
        int rowStride, int pixelStride
    ) override;

    std::string getBackendType() const override { return m_backend_type; }
    bool isInitialized() const override { return m_initialized; }
    // 后端选择/失败诊断信息
    std::string getDiag() const override { return m_diag; }

private:
    // GPU delegate builder (universal, stays here)
    TfLiteDelegate* buildGpuDelegate();
    void deleteDelegate();

    // Delegate builders (independent modules)
    QnnEngine m_qnn_engine;      // Qualcomm HTP
    // NeuronEngine m_neuron_engine;  // MediaTek Neuron - TODO

    TfLiteModel* m_model = nullptr;
    TfLiteInterpreter* m_interpreter = nullptr;
    TfLiteDelegate* m_delegate = nullptr;
    std::string m_backend_type = "LiteRT";
    bool m_initialized = false;
    std::string m_diag;               // 初始化诊断信息
};
