/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/gpu_bridge.h"
#include "ohos_bridge/log.h"

#include <dlfcn.h>

GfxstreamBridge& GfxstreamBridge::Instance() {
    static GfxstreamBridge inst;
    return inst;
}

bool GfxstreamBridge::EnsureLoadedLocked() {
    if (dlHandle_ && fnInit_ && fnSetWindow_ && fnClearWindow_ && fnStop_ && fnGetStats_) {
        return true;
    }
    dlHandle_ = dlopen("libgfxstream_host.so", RTLD_NOW | RTLD_LOCAL);
    if (!dlHandle_) {
        const char* err = dlerror();
        lastError_ = std::string("dlopen(libgfxstream_host.so) failed: ") + (err ? err : "unknown");
        OB_LOG_ERROR("SvboxGfxBridge", "%s", lastError_.c_str());
        return false;
    }
    fnInit_ = reinterpret_cast<int (*)(int32_t, int32_t, int, const char*)>(
        dlsym(dlHandle_, "svbox_gfx_init"));
    fnSetWindow_ = reinterpret_cast<int (*)(void*, int32_t, int32_t)>(
        dlsym(dlHandle_, "svbox_gfx_set_window"));
    fnClearWindow_ = reinterpret_cast<void (*)(void)>(
        dlsym(dlHandle_, "svbox_gfx_clear_window"));
    fnRepaint_ = reinterpret_cast<void (*)(void)>(
        dlsym(dlHandle_, "svbox_gfx_repaint"));
    fnSetVisible_ = reinterpret_cast<void (*)(int)>(
        dlsym(dlHandle_, "svbox_gfx_set_visible"));
    fnSetVsyncHz_ = reinterpret_cast<void (*)(int32_t)>(
        dlsym(dlHandle_, "svbox_gfx_set_vsync_hz"));
    fnStop_ = reinterpret_cast<void (*)(void)>(
        dlsym(dlHandle_, "svbox_gfx_stop"));
    fnGetStats_ = reinterpret_cast<void (*)(SvboxGfxStats*)>(
        dlsym(dlHandle_, "svbox_gfx_get_stats"));
    fnSetDisplayWindow_ = reinterpret_cast<int (*)(int32_t, void*, int32_t, int32_t)>(
        dlsym(dlHandle_, "svbox_gfx_set_display_window"));
    fnClearDisplayWindow_ = reinterpret_cast<void (*)(int32_t)>(
        dlsym(dlHandle_, "svbox_gfx_clear_display_window"));
    fnSetDisplayVisible_ = reinterpret_cast<void (*)(int32_t, int)>(
        dlsym(dlHandle_, "svbox_gfx_set_display_visible"));
    fnGetDisplayStats_ = reinterpret_cast<void (*)(int32_t, SvboxGfxDisplayStats*)>(
        dlsym(dlHandle_, "svbox_gfx_get_display_stats"));
    fnReadDisplayRgba_ = reinterpret_cast<int (*)(int32_t, void*, uint32_t, int32_t*, int32_t*)>(
        dlsym(dlHandle_, "svbox_gfx_read_display_rgba"));

    if (!fnInit_ || !fnSetWindow_ || !fnClearWindow_ || !fnStop_ || !fnGetStats_) {
        lastError_ = "Missing svbox_gfx_* symbols in libgfxstream_host.so";
        OB_LOG_ERROR("SvboxGfxBridge", "%s", lastError_.c_str());
        dlclose(dlHandle_);
        dlHandle_ = nullptr;
        return false;
    }
    OB_LOG_INFO("SvboxGfxBridge", "Loaded libgfxstream_host.so successfully");
    return true;
}

bool GfxstreamBridge::Start(const std::string& sockPath, int32_t width, int32_t height,
                            SvboxGfxMode mode) {
    Stop();
    std::lock_guard<std::mutex> lk(mu_);
    lastError_.clear();
    if (!EnsureLoadedLocked()) {
        return false;
    }
    fbW_ = width;
    fbH_ = height;

    if (pendingWindow_) {
        fnSetWindow_(pendingWindow_, pendingW_ > 0 ? pendingW_ : width,
                     pendingH_ > 0 ? pendingH_ : height);
    }
    if (fnSetVsyncHz_) {
        fnSetVsyncHz_(targetHz_);
    }

    int rc = fnInit_(width, height, static_cast<int>(mode), sockPath.c_str());
    if (rc != 0) {
        lastError_ = "svbox_gfx_init failed rc=" + std::to_string(rc);
        OB_LOG_ERROR("SvboxGfxBridge", "%s (mode=%d sock=%s)",
                     lastError_.c_str(), static_cast<int>(mode), sockPath.c_str());
        return false;
    }

    active_.store(true);
    SvboxGfxStats st{};
    fnGetStats_(&st);
    OB_LOG_INFO("SvboxGfxBridge", "gfxstream started: mode=%d renderer=%s version=%s",
                static_cast<int>(mode), st.renderer, st.version);
    return true;
}

void GfxstreamBridge::Stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_.exchange(false)) {
        return;
    }
    pendingWindow_ = nullptr;
    pendingW_ = 0;
    pendingH_ = 0;
    if (fnStop_) fnStop_();
    OB_LOG_INFO("SvboxGfxBridge", "gfxstream stopped");
}

void GfxstreamBridge::SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH) {
    std::lock_guard<std::mutex> lk(mu_);
    pendingWindow_ = window;
    pendingW_ = surfaceW;
    pendingH_ = surfaceH;
    if (active_.load() && fnSetWindow_ && window) {
        int rc = fnSetWindow_(window, surfaceW > 0 ? surfaceW : fbW_,
                              surfaceH > 0 ? surfaceH : fbH_);
        OB_LOG_INFO("SvboxGfxBridge", "gfxstream SetWindow(%p, %dx%d) -> %d",
                    static_cast<void*>(window), surfaceW, surfaceH, rc);
    }
}

void GfxstreamBridge::ClearWindow() {
    std::lock_guard<std::mutex> lk(mu_);
    pendingWindow_ = nullptr;
    pendingW_ = 0;
    pendingH_ = 0;
    if (active_.load() && fnClearWindow_) fnClearWindow_();
}

void GfxstreamBridge::SetVisible(bool visible) {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_.load() && fnSetVisible_) fnSetVisible_(visible ? 1 : 0);
}

void GfxstreamBridge::SetRefreshRate(int32_t hz) {
    if (hz != 60 && hz != 90 && hz != 120) hz = 60;
    std::lock_guard<std::mutex> lk(mu_);
    targetHz_ = hz;
    if (active_.load() && fnSetVsyncHz_) {
        fnSetVsyncHz_(hz);
    }
}

void GfxstreamBridge::SetDisplayWindow(int32_t displaySlot, OHNativeWindow* window,
                                       int32_t surfaceW, int32_t surfaceH) {
    if (displaySlot == 0) {
        SetWindow(window, surfaceW, surfaceH);
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!EnsureLoadedLocked() || !fnSetDisplayWindow_) return;
    int rc = fnSetDisplayWindow_(displaySlot, window, surfaceW, surfaceH);
    OB_LOG_INFO("SvboxGfxBridge", "gfxstream SetDisplayWindow(slot=%d, %p, %dx%d) -> %d",
                displaySlot, static_cast<void*>(window), surfaceW, surfaceH, rc);
}

void GfxstreamBridge::ClearDisplayWindow(int32_t displaySlot) {
    if (displaySlot == 0) {
        ClearWindow();
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (fnClearDisplayWindow_) {
        fnClearDisplayWindow_(displaySlot);
    }
}

void GfxstreamBridge::SetDisplayVisible(int32_t displaySlot, bool visible) {
    if (displaySlot == 0) {
        SetVisible(visible);
        return;
    }
    if (displaySlot < 1 || displaySlot >= SVBOX_GFX_MAX_DISPLAYS) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (active_.load() && fnSetDisplayVisible_) {
        fnSetDisplayVisible_(displaySlot, visible ? 1 : 0);
    }
}

SvboxGfxDisplayStats GfxstreamBridge::GetDisplayStats(int32_t displaySlot) const {
    SvboxGfxDisplayStats st{};
    st.displaySlot = displaySlot;
    if (!active_.load()) return st;
    std::lock_guard<std::mutex> lk(mu_);
    if (fnGetDisplayStats_) {
        fnGetDisplayStats_(displaySlot, &st);
    }
    return st;
}

bool GfxstreamBridge::ReadDisplayRgba(int32_t displaySlot, void* outRgba, uint32_t bufBytes,
                                      int32_t* outW, int32_t* outH) const {
    if (!active_.load() || !outRgba) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnReadDisplayRgba_) return false;
    return fnReadDisplayRgba_(displaySlot, outRgba, bufBytes, outW, outH) == 0;
}

bool GfxstreamBridge::IsConnected() const {
    if (!active_.load()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnGetStats_) return false;
    SvboxGfxStats st{};
    fnGetStats_(&st);
    return st.initialized != 0 && (st.framesPosted > 0 || st.activeChannels > 0);
}

uint64_t GfxstreamBridge::FrameCount() const {
    if (!active_.load()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnGetStats_) return 0;
    SvboxGfxStats st{};
    fnGetStats_(&st);
    return st.framesPosted;
}

int32_t GfxstreamBridge::Fps() const {
    if (!active_.load()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnGetStats_) return 0;
    SvboxGfxStats st{};
    fnGetStats_(&st);
    return st.fps;
}

int32_t GfxstreamBridge::FrameWidth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fbW_;
}

int32_t GfxstreamBridge::FrameHeight() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fbH_;
}

std::string GfxstreamBridge::RendererName() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnGetStats_ || !active_.load()) return "";
    SvboxGfxStats st{};
    fnGetStats_(&st);
    return st.renderer;
}

std::string GfxstreamBridge::LastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastError_;
}
