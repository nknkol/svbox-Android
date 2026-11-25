/* SPDX-License-Identifier: MIT */
#include "android_runtime.h"
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/gpu_bridge.h"
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/usb_bridge.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <mutex>
#include <napi/native_api.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "SvboxNapi"

namespace {

// Linux input-event-codes.h constants (avoid macro collision with OH_NativeXComponent_KeyCode)
constexpr uint16_t kLinuxKeyBack = 158;
constexpr uint16_t kLinuxKeyHomepage = 172;
constexpr uint16_t kLinuxKeyAppSelect = 580;
constexpr uint16_t kLinuxKeyVolUp = 115;
constexpr uint16_t kLinuxKeyVolDown = 114;
constexpr uint16_t kLinuxKeyPower = 116;

std::string GetStringArg(napi_env env, napi_value val) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, val, nullptr, 0, &len) != napi_ok) {
        return "";
    }
    std::string out(len, '\0');
    napi_get_value_string_utf8(env, val, out.data(), len + 1, &len);
    return out;
}

int32_t GetInt32Arg(napi_env env, napi_value val, int32_t fallback) {
    int32_t out = fallback;
    if (napi_get_value_int32(env, val, &out) != napi_ok) {
        return fallback;
    }
    return out;
}

double GetDoubleArg(napi_env env, napi_value val, double fallback) {
    double out = fallback;
    if (napi_get_value_double(env, val, &out) != napi_ok) {
        return fallback;
    }
    return out;
}

bool GetBoolArg(napi_env env, napi_value val, bool fallback) {
    bool out = fallback;
    if (napi_get_value_bool(env, val, &out) != napi_ok) {
        return fallback;
    }
    return out;
}

std::mutex g_xcMu;
OH_NativeXComponent* g_xcBySlot[SVBOX_GFX_MAX_DISPLAYS] = {};
int32_t g_slotHz[SVBOX_GFX_MAX_DISPLAYS] = { 60, 60, 60, 60, 60, 60, 60, 60, 60 };

int32_t DisplaySlotFromXC(OH_NativeXComponent* component) {
    if (!component) return 0;
    char idBuf[OH_XCOMPONENT_ID_LEN_MAX + 1] = {};
    uint64_t idSize = OH_XCOMPONENT_ID_LEN_MAX + 1;
    if (OH_NativeXComponent_GetXComponentId(component, idBuf, &idSize) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return 0;
    }
    const char* prefix = "svbox_display_";
    const size_t plen = std::strlen(prefix);
    if (std::strncmp(idBuf, prefix, plen) == 0) {
        int32_t slot = std::atoi(idBuf + plen);
        if (slot >= 0 && slot < SVBOX_GFX_MAX_DISPLAYS) return slot;
    }
    return 0;
}

void ApplyExpectedFrameRateLocked(OH_NativeXComponent* component, int32_t hz) {
    if (!component) return;
    if (hz != 60 && hz != 90 && hz != 120) hz = 60;
    OH_NativeXComponent_ExpectedRateRange range = { 60, hz, hz };
    int32_t rc = OH_NativeXComponent_SetExpectedFrameRateRange(component, &range);
    OH_LOG_INFO(LOG_APP, "OH_NativeXComponent_SetExpectedFrameRateRange(60..%d, exp=%d) -> %d",
                hz, hz, rc);
}

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    int32_t slot = DisplaySlotFromXC(component);
    {
        std::lock_guard<std::mutex> lk(g_xcMu);
        g_xcBySlot[slot] = component;
        int32_t hz = (slot == 0) ? AndroidRuntime::Instance().RefreshRate() : g_slotHz[slot];
        ApplyExpectedFrameRateLocked(component, hz);
    }
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    InputBridge::Instance().SetDisplaySurfaceSize(slot, static_cast<int32_t>(w), static_cast<int32_t>(h));
    AndroidRuntime::Instance().OnDisplaySurfaceChanged(
        slot,
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    int32_t slot = DisplaySlotFromXC(component);
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    InputBridge::Instance().SetDisplaySurfaceSize(slot, static_cast<int32_t>(w), static_cast<int32_t>(h));
    AndroidRuntime::Instance().OnDisplaySurfaceChanged(
        slot,
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window) {
    int32_t slot = DisplaySlotFromXC(component);
    {
        std::lock_guard<std::mutex> lk(g_xcMu);
        if (g_xcBySlot[slot] == component) {
            g_xcBySlot[slot] = nullptr;
        }
    }
    AndroidRuntime::Instance().OnDisplaySurfaceDestroyed(slot, static_cast<OHNativeWindow*>(window));
}

void DispatchTouchEventCB(OH_NativeXComponent* component, void* window) {
    int32_t slot = DisplaySlotFromXC(component);
    OH_NativeXComponent_TouchEvent ev = {};
    if (OH_NativeXComponent_GetTouchEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    int action = 1;
    switch (ev.type) {
        case OH_NATIVEXCOMPONENT_DOWN: action = 0; break;
        case OH_NATIVEXCOMPONENT_MOVE: action = 1; break;
        case OH_NATIVEXCOMPONENT_UP: action = 2; break;
        case OH_NATIVEXCOMPONENT_CANCEL: action = 3; break;
        default: return;
    }
    InputBridge::Instance().InjectTouchForDisplay(slot, ev.id, action, ev.x, ev.y);
}

void DispatchMouseEventCB(OH_NativeXComponent* component, void* window) {
    int32_t slot = DisplaySlotFromXC(component);
    OH_NativeXComponent_MouseEvent ev = {};
    if (OH_NativeXComponent_GetMouseEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    InputBridge::Instance().InjectMouseForDisplay(
        slot,
        static_cast<int>(ev.action),
        static_cast<int>(ev.button),
        ev.x,
        ev.y);
}

void DispatchHoverEventCB(OH_NativeXComponent*, bool) {}

bool DispatchKeyEventCB(OH_NativeXComponent* component, void*) {
    OH_NativeXComponent_KeyEvent* kev = nullptr;
    if (OH_NativeXComponent_GetKeyEvent(component, &kev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS || !kev) {
        return false;
    }
    OH_NativeXComponent_KeyAction act = OH_NATIVEXCOMPONENT_KEY_ACTION_UNKNOWN;
    OH_NativeXComponent_KeyCode code = KEY_UNKNOWN;
    OH_NativeXComponent_GetKeyEventAction(kev, &act);
    OH_NativeXComponent_GetKeyEventCode(kev, &code);
    if (act == OH_NATIVEXCOMPONENT_KEY_ACTION_DOWN || act == OH_NATIVEXCOMPONENT_KEY_ACTION_UP) {
        InputBridge::Instance().InjectOhosKey(
            static_cast<int32_t>(code),
            act == OH_NATIVEXCOMPONENT_KEY_ACTION_DOWN);
        return true;
    }
    return false;
}

static OH_NativeXComponent_Callback g_xcCallback = {
    OnSurfaceCreatedCB,
    OnSurfaceChangedCB,
    OnSurfaceDestroyedCB,
    DispatchTouchEventCB,
};

static OH_NativeXComponent_MouseEvent_Callback g_mouseCallback = {
    DispatchMouseEventCB,
    DispatchHoverEventCB,
};

napi_value StartContainerNapi(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 6) {
        napi_value err = nullptr;
        napi_create_string_utf8(env, "Expected 6 arguments", NAPI_AUTO_LENGTH, &err);
        return err;
    }
    std::string tempDir = GetStringArg(env, args[0]);
    std::string svboxPath = GetStringArg(env, args[1]);
    std::string androidDir = GetStringArg(env, args[2]);
    std::string mode = GetStringArg(env, args[3]);
    int32_t w = GetInt32Arg(env, args[4], 720);
    int32_t h = GetInt32Arg(env, args[5], 1280);

    std::string res = AndroidRuntime::Instance().Start(tempDir, svboxPath, androidDir, mode, w, h);
    napi_value out = nullptr;
    napi_create_string_utf8(env, res.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value AttachHwcSocketNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string sockPath = argc > 0 ? GetStringArg(env, args[0]) : "";
    int32_t w = argc > 1 ? GetInt32Arg(env, args[1], 720) : 720;
    int32_t h = argc > 2 ? GetInt32Arg(env, args[2], 1280) : 1280;
    HwcRenderer::Instance().Start(sockPath, w, h);
    napi_value out = nullptr;
    napi_create_string_utf8(env, "Attached", NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value StopContainerNapi(napi_env env, napi_callback_info) {
    std::string res = AndroidRuntime::Instance().Stop();
    napi_value out = nullptr;
    napi_create_string_utf8(env, res.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value SendNavKeyNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string key = argc > 0 ? GetStringArg(env, args[0]) : "";
    if (key == "back") InputBridge::Instance().TapLinuxKey(kLinuxKeyBack);
    else if (key == "home") InputBridge::Instance().TapLinuxKey(kLinuxKeyHomepage);
    else if (key == "recents") InputBridge::Instance().TapLinuxKey(kLinuxKeyAppSelect);
    else if (key == "vol_up") InputBridge::Instance().TapLinuxKey(kLinuxKeyVolUp);
    else if (key == "vol_down") InputBridge::Instance().TapLinuxKey(kLinuxKeyVolDown);
    else if (key == "power") InputBridge::Instance().TapLinuxKey(kLinuxKeyPower);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SendScrollNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    float x = argc > 0 ? static_cast<float>(GetDoubleArg(env, args[0], 360.0)) : 360.0f;
    float y = argc > 1 ? static_cast<float>(GetDoubleArg(env, args[1], 640.0)) : 640.0f;
    float dy = argc > 2 ? static_cast<float>(GetDoubleArg(env, args[2], 0.0)) : 0.0f;
    InputBridge::Instance().InjectScroll(x, y, dy);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetResourceConfigNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string profile = argc > 0 ? GetStringArg(env, args[0]) : "perf";
    std::string cpuset = argc > 1 ? GetStringArg(env, args[1]) : "";
    int32_t memMb = argc > 2 ? GetInt32Arg(env, args[2], 4096) : 4096;
    AndroidRuntime::Instance().SetResourceConfig(profile, cpuset, static_cast<uint32_t>(memMb > 0 ? memMb : 4096));
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetGpuModeNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string gpuMode = argc > 0 ? GetStringArg(env, args[0]) : "vulkan";
    AndroidRuntime::Instance().SetGpuMode(gpuMode);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetRefreshRateNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t hz = argc > 0 ? GetInt32Arg(env, args[0], 60) : 60;
    if (hz != 60 && hz != 90 && hz != 120) hz = 60;
    AndroidRuntime::Instance().SetRefreshRate(hz);
    {
        std::lock_guard<std::mutex> lk(g_xcMu);
        g_slotHz[0] = hz;
        ApplyExpectedFrameRateLocked(g_xcBySlot[0], hz);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetDisplayRefreshRateNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = argc > 0 ? GetInt32Arg(env, args[0], 0) : 0;
    int32_t hz = argc > 1 ? GetInt32Arg(env, args[1], 60) : 60;
    if (hz != 60 && hz != 90 && hz != 120) hz = 60;
    if (slot == 0) {
        AndroidRuntime::Instance().SetRefreshRate(hz);
    }
    if (slot >= 0 && slot < SVBOX_GFX_MAX_DISPLAYS) {
        int32_t maxHz = hz;
        {
            std::lock_guard<std::mutex> lk(g_xcMu);
            g_slotHz[slot] = hz;
            ApplyExpectedFrameRateLocked(g_xcBySlot[slot], hz);
            for (int i = 0; i < SVBOX_GFX_MAX_DISPLAYS; ++i) {
                if (g_xcBySlot[i] && g_slotHz[i] > maxHz) maxHz = g_slotHz[i];
            }
        }
        // Keep host gfxstream VSync at least as high as the fastest active window
        if (maxHz > AndroidRuntime::Instance().RefreshRate()) {
            AndroidRuntime::Instance().SetRefreshRate(maxHz);
        }
    }
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SetDisplayVisibleNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = argc > 0 ? GetInt32Arg(env, args[0], 0) : 0;
    bool visible = argc > 1 ? GetBoolArg(env, args[1], true) : true;
    AndroidRuntime::Instance().SetDisplayVisible(slot, visible);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value SendDisplayScrollNapi(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = argc > 0 ? GetInt32Arg(env, args[0], 0) : 0;
    float x = argc > 1 ? static_cast<float>(GetDoubleArg(env, args[1], 360.0)) : 360.0f;
    float y = argc > 2 ? static_cast<float>(GetDoubleArg(env, args[2], 640.0)) : 640.0f;
    float dy = argc > 3 ? static_cast<float>(GetDoubleArg(env, args[3], 0.0)) : 0.0f;
    InputBridge::Instance().InjectScrollForDisplay(slot, x, y, dy);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value IsWmAgentReadyNapi(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_get_boolean(env, AndroidRuntime::Instance().IsWmAgentReady(), &out);
    return out;
}

napi_value GetIconsDirNapi(napi_env env, napi_callback_info) {
    std::string dir = AndroidRuntime::Instance().IconsHostDir();
    napi_value out = nullptr;
    napi_create_string_utf8(env, dir.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value WmRpcNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string cmd = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string res = AndroidRuntime::Instance().WmRpc(cmd, 15000);
    napi_value out = nullptr;
    napi_create_string_utf8(env, res.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

struct WmRpcAsyncCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string cmd;
    std::string result;
};

void WmRpcAsyncExecute(napi_env, void* data) {
    auto* ctx = static_cast<WmRpcAsyncCtx*>(data);
    ctx->result = AndroidRuntime::Instance().WmRpc(ctx->cmd, 25000);
}

void WmRpcAsyncComplete(napi_env env, napi_status, void* data) {
    auto* ctx = static_cast<WmRpcAsyncCtx*>(data);
    napi_value val = nullptr;
    napi_create_string_utf8(env, ctx->result.c_str(), NAPI_AUTO_LENGTH, &val);
    napi_resolve_deferred(env, ctx->deferred, val);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

napi_value WmRpcAsyncNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto* ctx = new WmRpcAsyncCtx();
    ctx->cmd = argc > 0 ? GetStringArg(env, args[0]) : "";
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resName = nullptr;
    napi_create_string_utf8(env, "svbox_wm_rpc", NAPI_AUTO_LENGTH, &resName);
    napi_create_async_work(env, nullptr, resName, WmRpcAsyncExecute, WmRpcAsyncComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value AllocDisplaySlotNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string pkg = argc > 0 ? GetStringArg(env, args[0]) : "";
    int32_t slot = AndroidRuntime::Instance().AllocDisplaySlotForPkg(pkg);
    napi_value out = nullptr;
    napi_create_int32(env, slot, &out);
    return out;
}

napi_value FindDisplaySlotNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string pkg = argc > 0 ? GetStringArg(env, args[0]) : "";
    int32_t slot = AndroidRuntime::Instance().FindDisplaySlotForPkg(pkg);
    napi_value out = nullptr;
    napi_create_int32(env, slot, &out);
    return out;
}

napi_value ReleaseDisplaySlotNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = argc > 0 ? GetInt32Arg(env, args[0], -1) : -1;
    AndroidRuntime::Instance().ReleaseDisplaySlot(slot);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value PollWmEventsNapi(napi_env env, napi_callback_info) {
    std::string json = AndroidRuntime::Instance().PollWmEventsJson();
    napi_value out = nullptr;
    napi_create_string_utf8(env, json.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value GetDisplayStatsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t slot = argc > 0 ? GetInt32Arg(env, args[0], 0) : 0;
    auto ds = AndroidRuntime::Instance().GetDisplayStats(slot);
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    auto setI32 = [&](const char* k, int32_t v) {
        napi_value nv = nullptr;
        napi_create_int32(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    auto setI64 = [&](const char* k, int64_t v) {
        napi_value nv = nullptr;
        napi_create_int64(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    setI32("displaySlot", ds.displaySlot);
    setI32("attached", ds.attached);
    setI32("visible", ds.visible);
    setI32("width", ds.width);
    setI32("height", ds.height);
    setI64("framesPosted", static_cast<int64_t>(ds.framesPosted));
    setI32("fps", ds.fps);
    setI64("topBarColor", static_cast<int64_t>(ds.topBarColor));
    return obj;
}

napi_value SetPausedNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool paused = argc > 0 ? GetBoolArg(env, args[0], false) : false;
    std::string res = AndroidRuntime::Instance().SetPaused(paused);
    napi_value out = nullptr;
    napi_create_string_utf8(env, res.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value SaveRawSnapshotNapi(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool ok = false;
    if (argc >= 4) {
        std::string path = GetStringArg(env, args[0]);
        uint32_t w = static_cast<uint32_t>(GetInt32Arg(env, args[1], 0));
        uint32_t h = static_cast<uint32_t>(GetInt32Arg(env, args[2], 0));
        void* data = nullptr;
        size_t len = 0;
        if (!path.empty() && w > 0 && h > 0 &&
            napi_get_arraybuffer_info(env, args[3], &data, &len) == napi_ok &&
            data && len >= static_cast<size_t>(w) * h * 4) {
            FILE* fp = fopen(path.c_str(), "wb");
            if (fp) {
                uint32_t hdr[2] = { w, h };
                fwrite(hdr, sizeof(uint32_t), 2, fp);
                fwrite(data, 1, static_cast<size_t>(w) * h * 4, fp);
                fclose(fp);
                ok = true;
            }
        }
    }
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

napi_value SetVisibleNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool visible = argc > 0 ? GetBoolArg(env, args[0], true) : true;
    AndroidRuntime::Instance().SetVisible(visible);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value RunGuestCmdNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string cmd = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string outPath = argc > 1 ? GetStringArg(env, args[1]) : "";
    bool ok = AndroidRuntime::Instance().RunGuestCmd(cmd, outPath);
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

napi_value DumpProcsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string outPath = argc > 0 ? GetStringArg(env, args[0]) : "";
    bool ok = AndroidRuntime::Instance().DumpProcs(outPath);
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

napi_value InjectTapNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    float x = argc > 0 ? static_cast<float>(GetDoubleArg(env, args[0], 0.0)) : 0.0f;
    float y = argc > 1 ? static_cast<float>(GetDoubleArg(env, args[1], 0.0)) : 0.0f;
    std::thread([x, y]() {
        InputBridge::Instance().InjectTouch(0, 0, x, y);
        usleep(45000);
        InputBridge::Instance().InjectTouch(0, 2, x, y);
    }).detach();
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value InjectGamepadButtonNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t btn = argc > 0 ? GetInt32Arg(env, args[0], 0) : 0;
    bool down = argc > 1 ? GetBoolArg(env, args[1], false) : false;
    if (btn > 0 && btn < 65536) {
        InputBridge::Instance().InjectGamepadButton(static_cast<uint16_t>(btn), down);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value InjectGamepadAxisNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t axis = argc > 0 ? GetInt32Arg(env, args[0], -1) : -1;
    int32_t val = argc > 1 ? GetInt32Arg(env, args[1], 0) : 0;
    if (axis >= 0 && axis < 64) {
        InputBridge::Instance().InjectGamepadAxis(static_cast<uint16_t>(axis), val);
    }
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

// --- Multi-Instance Storage & Lifecycle Helpers ---
bool IsSafeSubdir(const std::string& s) {
    if (s.empty() || s[0] == '/') return false;
    if (s.find("..") != std::string::npos) return false;
    return true;
}

bool MkdirPLocal(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    return mkdir(cur.c_str(), 0755) == 0 || errno == EEXIST;
}

uint64_t SumDirBytes(const std::string& root) {
    uint64_t total = 0;
    std::vector<std::string> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        std::string cur = std::move(stack.back());
        stack.pop_back();
        struct stat st = {};
        if (lstat(cur.c_str(), &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            total += static_cast<uint64_t>(st.st_size);
        } else if (S_ISDIR(st.st_mode)) {
            DIR* d = opendir(cur.c_str());
            if (!d) continue;
            while (struct dirent* e = readdir(d)) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
                stack.push_back(cur + "/" + e->d_name);
            }
            closedir(d);
        }
    }
    return total;
}

uint64_t RemoveTreeBytes(const std::string& path, bool keepRootDir = false) {
    struct stat st = {};
    if (lstat(path.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        uint64_t sz = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
        unlink(path.c_str());
        return sz;
    }
    uint64_t freed = 0;
    DIR* d = opendir(path.c_str());
    if (d) {
        while (struct dirent* e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
            freed += RemoveTreeBytes(path + "/" + e->d_name, false);
        }
        closedir(d);
    }
    if (!keepRootDir) {
        rmdir(path.c_str());
    }
    return freed;
}

bool CopyFileRaw(const std::string& src, const std::string& dst, mode_t mode) {
    int inFd = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (inFd < 0) return false;
    int outFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode & 0777);
    if (outFd < 0) {
        close(inFd);
        return false;
    }
    char buf[131072];
    ssize_t n = 0;
    bool ok = true;
    while ((n = read(inFd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(outFd, buf + off, static_cast<size_t>(n - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    close(outFd);
    close(inFd);
    return ok;
}

bool CopyTreeRecursive(const std::string& src, const std::string& dst) {
    struct stat st = {};
    if (lstat(src.c_str(), &st) != 0) return false;
    if (S_ISLNK(st.st_mode)) {
        char linkBuf[4096];
        ssize_t ln = readlink(src.c_str(), linkBuf, sizeof(linkBuf) - 1);
        if (ln >= 0) {
            linkBuf[ln] = '\0';
            unlink(dst.c_str());
            symlink(linkBuf, dst.c_str());
        }
        return true;
    }
    if (S_ISREG(st.st_mode)) {
        return CopyFileRaw(src, dst, st.st_mode);
    }
    if (S_ISDIR(st.st_mode)) {
        if (!MkdirPLocal(dst)) return false;
        DIR* d = opendir(src.c_str());
        if (!d) return false;
        while (struct dirent* e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
            CopyTreeRecursive(src + "/" + e->d_name, dst + "/" + e->d_name);
        }
        closedir(d);
        return true;
    }
    return true;
}

int RunTarCmd(const std::vector<std::string>& args) {
    const char* tarBin = (access("/usr/bin/tar", X_OK) == 0) ? "/usr/bin/tar"
                       : (access("/bin/tar", X_OK) == 0 ? "/bin/tar" : "/system/bin/tar");
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>(tarBin));
        for (const auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, 0);
            dup2(devNull, 1);
            dup2(devNull, 2);
            if (devNull > 2) close(devNull);
        }
        execv(tarBin, cargs.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

napi_value SetInstanceConfigNapi(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value args[8] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string id = argc > 0 ? GetStringArg(env, args[0]) : "inst_0";
    std::string subdir = argc > 1 ? GetStringArg(env, args[1]) : "userdata14";
    int32_t dpi = argc > 2 ? GetInt32Arg(env, args[2], 0) : 0;
    bool audio = argc > 3 ? GetBoolArg(env, args[3], true) : true;
    bool root = argc > 4 ? GetBoolArg(env, args[4], true) : true;
    std::string brand = argc > 5 ? GetStringArg(env, args[5]) : "";
    std::string model = argc > 6 ? GetStringArg(env, args[6]) : "";
    int32_t adbPort = argc > 7 ? GetInt32Arg(env, args[7], 5555) : 5555;
    AndroidRuntime::Instance().SetInstanceConfig(id, subdir, dpi, audio, root, brand, model, adbPort);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value GetActiveInstanceIdNapi(napi_env env, napi_callback_info) {
    std::string id = AndroidRuntime::Instance().ActiveInstanceId();
    napi_value out = nullptr;
    napi_create_string_utf8(env, id.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value GetInstanceDiskMbNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string subdir = argc > 1 ? GetStringArg(env, args[1]) : "";
    double mb = 0.0;
    if (!baseDir.empty() && IsSafeSubdir(subdir)) {
        uint64_t bytes = SumDirBytes(baseDir + "/" + subdir);
        mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    }
    napi_value out = nullptr;
    napi_create_double(env, mb, &out);
    return out;
}

napi_value CleanInstanceCacheNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string subdir = argc > 1 ? GetStringArg(env, args[1]) : "";
    double freedMb = 0.0;
    if (!baseDir.empty() && IsSafeSubdir(subdir)) {
        auto& rt = AndroidRuntime::Instance();
        if (rt.IsRunning() && rt.DataSubdir() == subdir) {
            napi_value out = nullptr;
            napi_create_double(env, -1.0, &out);
            return out;
        }
        std::string root = baseDir + "/" + subdir;
        uint64_t freed = 0;
        freed += RemoveTreeBytes(root + "/data/tombstones", true);
        freed += RemoveTreeBytes(root + "/data/anr", true);
        freed += RemoveTreeBytes(root + "/data/system/dropbox", true);
        // Clean per-app cache & code_cache dirs under /data/data (preserve /data/cache system dir)
        DIR* d = opendir((root + "/data/data").c_str());
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string pkgDir = root + "/data/data/" + e->d_name;
                freed += RemoveTreeBytes(pkgDir + "/cache", true);
                freed += RemoveTreeBytes(pkgDir + "/code_cache", true);
            }
            closedir(d);
        }
        freedMb = static_cast<double>(freed) / (1024.0 * 1024.0);
    }
    napi_value out = nullptr;
    napi_create_double(env, freedMb, &out);
    return out;
}

napi_value DeleteInstanceDataNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string subdir = argc > 1 ? GetStringArg(env, args[1]) : "";
    bool ok = false;
    if (!baseDir.empty() && IsSafeSubdir(subdir)) {
        auto& rt = AndroidRuntime::Instance();
        if (!rt.IsRunning() || rt.DataSubdir() != subdir) {
            RemoveTreeBytes(baseDir + "/" + subdir, false);
            ok = true;
        }
    }
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

struct CloneAsyncCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string srcPath;
    std::string dstPath;
    bool ok = false;
};

void CloneAsyncExec(napi_env, void* data) {
    auto* ctx = static_cast<CloneAsyncCtx*>(data);
    struct stat st = {};
    if (stat(ctx->srcPath.c_str(), &st) != 0) {
        // Source hasn't been booted yet — just create empty dir
        ctx->ok = MkdirPLocal(ctx->dstPath);
    } else {
        ctx->ok = CopyTreeRecursive(ctx->srcPath, ctx->dstPath);
    }
}

void CloneAsyncDone(napi_env env, napi_status, void* data) {
    auto* ctx = static_cast<CloneAsyncCtx*>(data);
    napi_value v = nullptr;
    napi_get_boolean(env, ctx->ok, &v);
    napi_resolve_deferred(env, ctx->deferred, v);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

napi_value CloneInstanceAsyncNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::string srcSub = argc > 1 ? GetStringArg(env, args[1]) : "";
    std::string dstSub = argc > 2 ? GetStringArg(env, args[2]) : "";
    auto* ctx = new CloneAsyncCtx();
    if (!baseDir.empty() && IsSafeSubdir(srcSub) && IsSafeSubdir(dstSub)) {
        ctx->srcPath = baseDir + "/" + srcSub;
        ctx->dstPath = baseDir + "/" + dstSub;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value name = nullptr;
    napi_create_string_utf8(env, "svbox_clone_inst", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(env, nullptr, name, CloneAsyncExec, CloneAsyncDone, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

struct BackupAsyncCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    bool isRestore = false;
    bool compress = true;
    std::string baseDir;
    std::string subdir;
    std::string tarPath;
    std::string result;
};

void BackupAsyncExec(napi_env, void* data) {
    auto* ctx = static_cast<BackupAsyncCtx*>(data);
    if (ctx->baseDir.empty() || !IsSafeSubdir(ctx->subdir) || ctx->tarPath.empty()) {
        ctx->result = "ERR:参数无效";
        return;
    }
    if (!ctx->isRestore) {
        std::string fullDir = ctx->baseDir + "/" + ctx->subdir;
        struct stat st = {};
        if (stat(fullDir.c_str(), &st) != 0) {
            ctx->result = "ERR:该实例尚未初始化数据目录";
            return;
        }
        auto slash = ctx->tarPath.find_last_of('/');
        if (slash != std::string::npos) {
            MkdirPLocal(ctx->tarPath.substr(0, slash));
        }
        std::string flags = ctx->compress ? "-czf" : "-cf";
        int rc = RunTarCmd({ flags, ctx->tarPath, "-C", fullDir, "." });
        ctx->result = (rc == 0) ? ("OK:" + ctx->tarPath) : ("ERR:tar 退出码 " + std::to_string(rc));
    } else {
        struct stat st = {};
        if (stat(ctx->tarPath.c_str(), &st) != 0) {
            ctx->result = "ERR:备份文件不存在";
            return;
        }
        std::string fullDir = ctx->baseDir + "/" + ctx->subdir;
        RemoveTreeBytes(fullDir, false);
        MkdirPLocal(fullDir);
        bool isGz = (ctx->tarPath.size() >= 3 &&
                     ctx->tarPath.compare(ctx->tarPath.size() - 3, 3, ".gz") == 0);
        int rc = RunTarCmd({ isGz ? "-xzf" : "-xf", ctx->tarPath, "-C", fullDir });
        ctx->result = (rc == 0) ? "OK" : ("ERR:还原失败 (tar=" + std::to_string(rc) + ")");
    }
}

void BackupAsyncDone(napi_env env, napi_status, void* data) {
    auto* ctx = static_cast<BackupAsyncCtx*>(data);
    napi_value v = nullptr;
    napi_create_string_utf8(env, ctx->result.c_str(), NAPI_AUTO_LENGTH, &v);
    napi_resolve_deferred(env, ctx->deferred, v);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

napi_value BackupInstanceAsyncNapi(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto* ctx = new BackupAsyncCtx();
    ctx->isRestore = false;
    ctx->baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    ctx->subdir = argc > 1 ? GetStringArg(env, args[1]) : "";
    ctx->tarPath = argc > 2 ? GetStringArg(env, args[2]) : "";
    ctx->compress = argc > 3 ? GetBoolArg(env, args[3], true) : true;
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value name = nullptr;
    napi_create_string_utf8(env, "svbox_backup_inst", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(env, nullptr, name, BackupAsyncExec, BackupAsyncDone, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value RestoreInstanceAsyncNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto* ctx = new BackupAsyncCtx();
    ctx->isRestore = true;
    ctx->baseDir = argc > 0 ? GetStringArg(env, args[0]) : "";
    ctx->subdir = argc > 1 ? GetStringArg(env, args[1]) : "";
    ctx->tarPath = argc > 2 ? GetStringArg(env, args[2]) : "";
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value name = nullptr;
    napi_create_string_utf8(env, "svbox_restore_inst", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(env, nullptr, name, BackupAsyncExec, BackupAsyncDone, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value ListBackupFilesNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string dir = argc > 0 ? GetStringArg(env, args[0]) : "";
    std::vector<std::string> files;
    if (!dir.empty()) {
        DIR* d = opendir(dir.c_str());
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string fn(e->d_name);
                if (fn.find(".tar") != std::string::npos) {
                    files.push_back(fn);
                }
            }
            closedir(d);
            std::sort(files.begin(), files.end(), std::greater<std::string>());
        }
    }
    napi_value arr = nullptr;
    napi_create_array_with_length(env, files.size(), &arr);
    for (size_t i = 0; i < files.size(); ++i) {
        napi_value s = nullptr;
        napi_create_string_utf8(env, files[i].c_str(), NAPI_AUTO_LENGTH, &s);
        napi_set_element(env, arr, static_cast<uint32_t>(i), s);
    }
    return arr;
}

napi_value EnsureDirNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string dir = argc > 0 ? GetStringArg(env, args[0]) : "";
    bool ok = !dir.empty() && MkdirPLocal(dir);
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

napi_value GetStatusNapi(napi_env env, napi_callback_info) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    auto setBool = [&](const char* k, bool v) {
        napi_value nv = nullptr;
        napi_get_boolean(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    auto setI32 = [&](const char* k, int32_t v) {
        napi_value nv = nullptr;
        napi_create_int32(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    auto setI64 = [&](const char* k, int64_t v) {
        napi_value nv = nullptr;
        napi_create_int64(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    auto setDouble = [&](const char* k, double v) {
        napi_value nv = nullptr;
        napi_create_double(env, v, &nv);
        napi_set_named_property(env, obj, k, nv);
    };
    auto setStr = [&](const char* k, const std::string& v) {
        napi_value nv = nullptr;
        napi_create_string_utf8(env, v.c_str(), NAPI_AUTO_LENGTH, &nv);
        napi_set_named_property(env, obj, k, nv);
    };

    auto& rt = AndroidRuntime::Instance();
    auto& hwc = HwcRenderer::Instance();
    auto& gfx = GfxstreamBridge::Instance();
    auto& inp = InputBridge::Instance();
    auto& aud = AudioBridge::Instance();
    auto tel = rt.GetTelemetry();
    auto topo = SchedQos::GetCoreTopology();
    const bool useGfx = rt.UseGfxstream();

    setBool("running", rt.IsRunning());
    setBool("paused", rt.IsPaused());
    setBool("connected", useGfx ? gfx.IsConnected() : hwc.IsConnected());
    setBool("wmAgentReady", rt.IsWmAgentReady());
    setBool("vsyncActive", useGfx ? gfx.IsActive() : hwc.UsingNativeVsync());
    setBool("inputReady", inp.HasTouchClient() || inp.HasKbdClient());
    setBool("gamepadReady", inp.HasGamepadClient());
    setBool("gameControllerKit", inp.GameControllerKitActive());
    setI64("usbTransfers", static_cast<int64_t>(UsbBridge::Instance().UsbTransfers()));
    setBool("audioActive", aud.IsActive());
    setI64("frames", static_cast<int64_t>(useGfx ? gfx.FrameCount() : hwc.FrameCount()));
    setI32("width", useGfx ? gfx.FrameWidth() : hwc.FrameWidth());
    setI32("height", useGfx ? gfx.FrameHeight() : hwc.FrameHeight());
    setI32("fps", useGfx ? gfx.Fps() : hwc.Fps());
    setI32("refreshRate", rt.RefreshRate());
    setDouble("cpuPercent", tel.systemCpuPercent);
    setI32("memMb", static_cast<int32_t>(tel.containerRssMb));
    setI32("memLimitMb", static_cast<int32_t>(rt.MemLimitMb()));
    setI32("activeProcs", static_cast<int32_t>(tel.activeProcs));
    setStr("profile", rt.Profile());
    setStr("gpuMode", rt.GpuMode());
    setStr("gpuRenderer", useGfx ? gfx.RendererName() : "SwiftShader (CPU)");
    setStr("cpuset", rt.ActiveCpuset());
    setStr("topology", topo.summary);
    setStr("statusText", rt.StatusText());
    setStr("activeInstanceId", rt.ActiveInstanceId());
    setStr("shareDir", rt.ShareHostDir());
    return obj;
}

napi_value Init(napi_env env, napi_value exports) {
    SchedQos::StartCoreTopologyProbeAsync();

    napi_property_descriptor desc[] = {
        { "startContainer", nullptr, StartContainerNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "attachHwcSocket", nullptr, AttachHwcSocketNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stopContainer", nullptr, StopContainerNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sendNavKey", nullptr, SendNavKeyNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sendScroll", nullptr, SendScrollNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setResourceConfig", nullptr, SetResourceConfigNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setInstanceConfig", nullptr, SetInstanceConfigNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getActiveInstanceId", nullptr, GetActiveInstanceIdNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getInstanceDiskMb", nullptr, GetInstanceDiskMbNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "cleanInstanceCache", nullptr, CleanInstanceCacheNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "cloneInstanceAsync", nullptr, CloneInstanceAsyncNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "deleteInstanceData", nullptr, DeleteInstanceDataNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "backupInstanceAsync", nullptr, BackupInstanceAsyncNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "restoreInstanceAsync", nullptr, RestoreInstanceAsyncNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "listBackupFiles", nullptr, ListBackupFilesNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "ensureDir", nullptr, EnsureDirNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setGpuMode", nullptr, SetGpuModeNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRefreshRate", nullptr, SetRefreshRateNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setDisplayRefreshRate", nullptr, SetDisplayRefreshRateNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setDisplayVisible", nullptr, SetDisplayVisibleNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sendDisplayScroll", nullptr, SendDisplayScrollNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isWmAgentReady", nullptr, IsWmAgentReadyNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getIconsDir", nullptr, GetIconsDirNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "wmRpc", nullptr, WmRpcNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "wmRpcAsync", nullptr, WmRpcAsyncNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "allocDisplaySlot", nullptr, AllocDisplaySlotNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "findDisplaySlot", nullptr, FindDisplaySlotNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "releaseDisplaySlot", nullptr, ReleaseDisplaySlotNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pollWmEvents", nullptr, PollWmEventsNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getDisplayStats", nullptr, GetDisplayStatsNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setPaused", nullptr, SetPausedNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setVisible", nullptr, SetVisibleNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "saveRawSnapshot", nullptr, SaveRawSnapshotNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "runGuestCmd", nullptr, RunGuestCmdNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "dumpProcs", nullptr, DumpProcsNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "injectTap", nullptr, InjectTapNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "injectGamepadButton", nullptr, InjectGamepadButtonNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "injectGamepadAxis", nullptr, InjectGamepadAxisNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getStatus", nullptr, GetStatusNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    napi_value xcVal = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcVal) == napi_ok && xcVal) {
        OH_NativeXComponent* nativeXC = nullptr;
        if (napi_unwrap(env, xcVal, reinterpret_cast<void**>(&nativeXC)) == napi_ok && nativeXC) {
            OH_NativeXComponent_RegisterCallback(nativeXC, &g_xcCallback);
            OH_NativeXComponent_RegisterMouseEventCallback(nativeXC, &g_mouseCallback);
            OH_NativeXComponent_RegisterKeyEventCallbackWithResult(nativeXC, DispatchKeyEventCB);
            OH_LOG_INFO(LOG_APP, "Registered OH_NativeXComponent Surface + Touch + Mouse + Key callbacks");
        }
    }
    return exports;
}

static napi_module g_entryModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = { 0 },
};

} // namespace

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&g_entryModule);
}
