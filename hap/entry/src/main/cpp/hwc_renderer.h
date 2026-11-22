/* SPDX-License-Identifier: MIT */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <native_window/external_window.h>

class HwcRenderer {
public:
    static HwcRenderer& Instance();

    void SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH);
    void ClearWindow();

    void Start(const std::string& hwcSockPath, int32_t frameW, int32_t frameH);
    void Stop();

    bool IsConnected() const { return connected_.load(); }
    uint64_t FrameCount() const { return frames_.load(); }
    int32_t Fps() const { return fps_.load(); }
    int32_t FrameWidth() const { return frameW_.load(); }
    int32_t FrameHeight() const { return frameH_.load(); }

private:
    HwcRenderer() = default;
    void RenderLoop(std::string sockPath, int32_t w, int32_t h);
    bool PresentRgbaFrame(const uint8_t* srcRgba, int32_t srcW, int32_t srcH);

    std::mutex winMu_;
    OHNativeWindow* window_ = nullptr;
    bool configured_ = false;
    int32_t cfgW_ = 0;
    int32_t cfgH_ = 0;

    std::atomic<bool> stop_{true};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<int32_t> fps_{0};
    std::atomic<int32_t> frameW_{480};
    std::atomic<int32_t> frameH_{854};
    std::thread worker_;
};
