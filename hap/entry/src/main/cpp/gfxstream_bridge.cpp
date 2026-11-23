/* SPDX-License-Identifier: MIT */
#include "gfxstream_bridge.h"

#include <chrono>
#include <dlfcn.h>
#include <hilog/log.h>

#undef LOG_TAG
#define LOG_TAG "SvboxGfxBridge"

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
        OH_LOG_ERROR(LOG_APP, "%{public}s", lastError_.c_str());
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
    fnStop_ = reinterpret_cast<void (*)(void)>(
        dlsym(dlHandle_, "svbox_gfx_stop"));
    fnGetStats_ = reinterpret_cast<void (*)(SvboxGfxStats*)>(
        dlsym(dlHandle_, "svbox_gfx_get_stats"));

    if (!fnInit_ || !fnSetWindow_ || !fnClearWindow_ || !fnStop_ || !fnGetStats_) {
        lastError_ = "Missing svbox_gfx_* symbols in libgfxstream_host.so";
        OH_LOG_ERROR(LOG_APP, "%{public}s", lastError_.c_str());
        dlclose(dlHandle_);
        dlHandle_ = nullptr;
        return false;
    }
    OH_LOG_INFO(LOG_APP, "Loaded libgfxstream_host.so successfully");
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

    // Pre-register XComponent OHNativeWindow if already available
    if (pendingWindow_) {
        fnSetWindow_(pendingWindow_, pendingW_ > 0 ? pendingW_ : width,
                     pendingH_ > 0 ? pendingH_ : height);
    }

    int rc = fnInit_(width, height, static_cast<int>(mode), sockPath.c_str());
    if (rc != 0) {
        lastError_ = "svbox_gfx_init failed rc=" + std::to_string(rc);
        OH_LOG_ERROR(LOG_APP, "%{public}s (mode=%{public}d sock=%{public}s)",
                     lastError_.c_str(), static_cast<int>(mode), sockPath.c_str());
        return false;
    }

    active_.store(true);
    SvboxGfxStats st{};
    fnGetStats_(&st);
    OH_LOG_INFO(LOG_APP, "gfxstream started: mode=%{public}d renderer=%{public}s version=%{public}s",
                static_cast<int>(mode), st.renderer, st.version);
    return true;
}

void GfxstreamBridge::Stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_.exchange(false)) {
        return;
    }
    if (fnClearWindow_) {
        fnClearWindow_();
    }
    if (fnStop_) {
        fnStop_();
    }
    OH_LOG_INFO(LOG_APP, "gfxstream stopped");
}

void GfxstreamBridge::SetWindow(OHNativeWindow* window, int32_t surfaceW, int32_t surfaceH) {
    std::lock_guard<std::mutex> lk(mu_);
    pendingWindow_ = window;
    pendingW_ = surfaceW;
    pendingH_ = surfaceH;
    if (active_.load() && fnSetWindow_ && window) {
        int rc = fnSetWindow_(window, surfaceW > 0 ? surfaceW : fbW_,
                              surfaceH > 0 ? surfaceH : fbH_);
        OH_LOG_INFO(LOG_APP, "gfxstream SetWindow(%p, %{public}dx%{public}d) -> %{public}d",
                    window, surfaceW, surfaceH, rc);
    }
}

void GfxstreamBridge::ClearWindow() {
    std::lock_guard<std::mutex> lk(mu_);
    pendingWindow_ = nullptr;
    pendingW_ = 0;
    pendingH_ = 0;
    if (active_.load() && fnClearWindow_) {
        fnClearWindow_();
    }
}

void GfxstreamBridge::SetVisible(bool visible) {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_.load() && fnSetVisible_) {
        fnSetVisible_(visible ? 1 : 0);
    }
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
    if (!active_.load()) return "";
    std::lock_guard<std::mutex> lk(mu_);
    if (!fnGetStats_) return "";
    SvboxGfxStats st{};
    fnGetStats_(&st);
    if (st.renderer[0] != '\0') {
        return std::string(st.renderer);
    }
    return "Maleoon 916";
}

std::string GfxstreamBridge::LastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastError_;
}
