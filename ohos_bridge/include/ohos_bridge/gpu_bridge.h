/* SPDX-License-Identifier: MIT */
/* HarmonyOS GPU / gfxstream host bridge: dynamically loads libgfxstream_host.so
 * and binds an OHNativeWindow for hardware-accelerated Vulkan/GLES presentation. */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#ifndef OHOS_BRIDGE_HEADLESS
#include <native_window/external_window.h>
#else
struct NativeWindow;
using OHNativeWindow = NativeWindow;
#endif

#include "svbox_gfxstream_capi.h"

class GfxstreamBridge {
public:
    static GfxstreamBridge& Instance();

    bool Start(const std::string& sockPath, int32_t width, int32_t height, SvboxGfxMode mode);
    void Stop();

    void SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH);
    void ClearWindow();
    void SetVisible(bool visible);
    void SetRefreshRate(int32_t hz);

    bool IsActive() const { return active_.load(); }
    bool IsConnected() const;
    uint64_t FrameCount() const;
    int32_t Fps() const;
    int32_t FrameWidth() const;
    int32_t FrameHeight() const;
    std::string RendererName() const;
    std::string LastError() const;

private:
    GfxstreamBridge() = default;
    bool EnsureLoadedLocked();

    mutable std::mutex mu_;
    void* dlHandle_ = nullptr;
    int (*fnInit_)(int32_t, int32_t, int, const char*) = nullptr;
    int (*fnSetWindow_)(void*, int32_t, int32_t) = nullptr;
    void (*fnClearWindow_)(void) = nullptr;
    void (*fnRepaint_)(void) = nullptr;
    void (*fnSetVisible_)(int) = nullptr;
    void (*fnSetVsyncHz_)(int32_t) = nullptr;
    void (*fnStop_)(void) = nullptr;
    void (*fnGetStats_)(SvboxGfxStats*) = nullptr;

    std::atomic<bool> active_{false};
    OHNativeWindow* pendingWindow_ = nullptr;
    int32_t pendingW_ = 0;
    int32_t pendingH_ = 0;
    int32_t fbW_ = 720;
    int32_t fbH_ = 1280;
    int32_t targetHz_ = 60;
    std::string lastError_;
};
