#pragma once

#include <tensorflow/lite/c/common.h>
#include <string>

// QNN HTP delegate builder for Qualcomm Hexagon NPU
class QnnEngine {
public:
    QnnEngine();
    ~QnnEngine();

    // Build QNN HTP delegate, returns nullptr if not available
    TfLiteDelegate* buildDelegate();

    // Delete the delegate
    void deleteDelegate();

    // Get backend name
    std::string getBackendName() const { return "QNN HTP"; }

    // 诊断信息（初始化失败原因等）
    const std::string& getDiag() const { return m_diag; }

private:
    static bool isQualcommSnapdragon();

    TfLiteDelegate* m_delegate = nullptr;
    bool m_preloaded = false;
    char m_native_lib_dir[512] = {0};
    std::string m_diag;
};
