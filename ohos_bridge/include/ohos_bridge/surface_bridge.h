/* SPDX-License-Identifier: MIT */
/* Async double-buffered XComponent OHNativeWindow presenter with OH_NativeVSync
 * on-demand pacing. Supports both prime-FD HWC stream clients (e.g. Android
 * hwcomposer) and generic RGBA framebuffer clients (e.g. Linux GUI/SDL/Wayland
 * apps via SubmitRgbaFrame or StartFramebufferServer). */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef OHOS_BRIDGE_HEADLESS
#include <native_window/external_window.h>
struct OH_NativeVSync;
#else
struct NativeWindow;
using OHNativeWindow = NativeWindow;
struct OH_NativeVSync;
#endif

class SurfaceBridge {
public:
    static SurfaceBridge& Instance();

    void SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH);
    void ClearWindow();
    void SetVisible(bool visible);

    // Connect-mode prime-FD HWC socket (used by Android hwcomposer).
    void Start(const std::string& hwcSockPath, int32_t frameW, int32_t frameH);
    // Listen-mode generic Linux RGBA framebuffer socket ("SVFB" header + raw RGBA8888).
    void StartFramebufferServer(const std::string& fbSockPath, int32_t frameW, int32_t frameH);
    void Stop();

    // Direct RGBA8888 frame submission from any host or bridge source.
    void SubmitRgbaFrame(const uint8_t* rgba, int32_t w, int32_t h);

    bool IsConnected() const { return connected_.load(); }
    uint64_t FrameCount() const { return frames_.load(); }
    int32_t FrameWidth() const { return frameW_.load(); }
    int32_t FrameHeight() const { return frameH_.load(); }
    int32_t Fps() const { return fps_.load(); }
    bool UsingNativeVsync() const { return usingVsync_.load(); }
    // Snapshot last staged RGBA frame (useful for headless testing/verification).
    bool CopyLatestFrame(std::vector<uint8_t>& outRgba, int32_t& outW, int32_t& outH);

private:
    SurfaceBridge() = default;
    void StartCommon(int32_t frameW, int32_t frameH);
    void RecvLoop(std::string sockPath, int32_t w, int32_t h);
    void FbServerLoop(std::string sockPath, int32_t w, int32_t h);
    void PresentLatest();
    void RequestVsyncFrame();
    static void OnVsyncFrame(long long timestamp, void* data);

    bool PresentRgbaFrame(const uint8_t* srcRgba, int32_t srcW, int32_t srcH);

    std::mutex winMu_;
    OHNativeWindow* window_ = nullptr;
    int32_t cfgW_ = 0;
    int32_t cfgH_ = 0;
    bool configured_ = false;

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
    int fbListenFd_ = -1;
    std::string fbSockPath_;
    std::thread worker_;
};

using HwcRenderer = SurfaceBridge;
