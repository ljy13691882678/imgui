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

private:
    static bool isQualcommSnapdragon();

    TfLiteDelegate* m_delegate = nullptr;
    bool m_preloaded = false;
    char m_native_lib_dir[512] = {0};
};
