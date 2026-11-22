/* SPDX-License-Identifier: MIT */
/* Async double-buffered XComponent HWC renderer with OH_NativeVSync on-demand
 * pacing: decouples hwcomposer.redroid.so frame ingestion ("ok" ACK) from
 * OHNativeWindow presentation and pauses callbacks when idle or hidden. */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <native_window/external_window.h>
#include <string>
#include <thread>
#include <vector>

struct OH_NativeVSync;

class HwcRenderer {
public:
    static HwcRenderer& Instance();

    void SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH);
    void ClearWindow();
    void SetVisible(bool visible);

    void Start(const std::string& hwcSockPath, int32_t frameW, int32_t frameH);
    void Stop();

    bool IsConnected() const { return connected_.load(); }
    uint64_t FrameCount() const { return frames_.load(); }
    int32_t FrameWidth() const { return frameW_.load(); }
    int32_t FrameHeight() const { return frameH_.load(); }
    int32_t Fps() const { return fps_.load(); }
    bool UsingNativeVsync() const { return usingVsync_.load(); }

private:
    HwcRenderer() = default;
    void RecvLoop(std::string sockPath, int32_t w, int32_t h);
    void PresentLatest();
    void RequestVsyncFrame();
    static void OnVsyncFrame(long long timestamp, void* data);

    bool PresentRgbaFrame(const uint8_t* srcRgba, int32_t srcW, int32_t srcH);

    std::mutex winMu_;
    OHNativeWindow* window_ = nullptr;
    int32_t cfgW_ = 0;
    int32_t cfgH_ = 0;
    bool configured_ = false;

    // Double-buffered latest-frame staging to decouple recv from present
    std::mutex bufMu_;
    std::vector<uint8_t> staging_[2];
    int frontIdx_ = 0;
    int32_t stagedW_ = 0;
    int32_t stagedH_ = 0;
    bool hasPendingFrame_ = false;

    std::mutex vsyncMu_;
    OH_NativeVSync* vsync_ = nullptr;
    bool vsyncArmed_ = false;
    std::condition_variable presentCv_;
    std::thread fallbackPresentThread_;

    std::atomic<bool> stop_{true};
    std::atomic<bool> visible_{true};
    std::atomic<bool> connected_{false};
    std::atomic<bool> usingVsync_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<int32_t> frameW_{0};
    std::atomic<int32_t> frameH_{0};
    std::atomic<int32_t> fps_{0};
    std::thread worker_;
};
