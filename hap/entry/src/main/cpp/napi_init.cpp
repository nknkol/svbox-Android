/* SPDX-License-Identifier: MIT */
#include "android_runtime.h"
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/gpu_bridge.h"
#include "ohos_bridge/input_bridge.h"
#include "ohos_bridge/sched_qos.h"
#include "ohos_bridge/surface_bridge.h"
#include "ohos_bridge/usb_bridge.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <cstdio>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <string>
#include <thread>
#include <unistd.h>

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

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    InputBridge::Instance().SetSurfaceSize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    AndroidRuntime::Instance().OnSurfaceChanged(
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    InputBridge::Instance().SetSurfaceSize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    AndroidRuntime::Instance().OnSurfaceChanged(
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceDestroyedCB(OH_NativeXComponent*, void* window) {
    AndroidRuntime::Instance().OnSurfaceDestroyed(static_cast<OHNativeWindow*>(window));
}

void DispatchTouchEventCB(OH_NativeXComponent* component, void* window) {
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
    InputBridge::Instance().InjectTouch(ev.id, action, ev.x, ev.y);
}

void DispatchMouseEventCB(OH_NativeXComponent* component, void* window) {
    OH_NativeXComponent_MouseEvent ev = {};
    if (OH_NativeXComponent_GetMouseEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    InputBridge::Instance().InjectMouse(
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
    std::string gpuMode = argc > 0 ? GetStringArg(env, args[0]) : "gles";
    AndroidRuntime::Instance().SetGpuMode(gpuMode);
    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
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
        { "setGpuMode", nullptr, SetGpuModeNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
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
