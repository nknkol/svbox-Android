/* SPDX-License-Identifier: MIT */
#include "android_runtime.h"
#include "hwc_renderer.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <string>

#undef LOG_TAG
#define LOG_TAG "SvboxNapi"

namespace {

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

void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    HwcRenderer::Instance().SetWindow(
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    HwcRenderer::Instance().SetWindow(
        static_cast<OHNativeWindow*>(window),
        static_cast<int32_t>(w),
        static_cast<int32_t>(h));
}

void OnSurfaceDestroyedCB(OH_NativeXComponent*, void*) {
    HwcRenderer::Instance().ClearWindow();
}

void DispatchTouchEventCB(OH_NativeXComponent*, void*) {
    // Input forwarding via --vdev /dev/input/event0 or /ipc/uinputd.sock planned in v0.2
}

static OH_NativeXComponent_Callback g_xcCallback = {
    OnSurfaceCreatedCB,
    OnSurfaceChangedCB,
    OnSurfaceDestroyedCB,
    DispatchTouchEventCB,
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
    int32_t w = GetInt32Arg(env, args[4], 480);
    int32_t h = GetInt32Arg(env, args[5], 854);

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
    int32_t w = argc > 1 ? GetInt32Arg(env, args[1], 480) : 480;
    int32_t h = argc > 2 ? GetInt32Arg(env, args[2], 854) : 854;
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

napi_value GetStatusNapi(napi_env env, napi_callback_info) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);

    napi_value vRunning = nullptr, vConn = nullptr, vFrames = nullptr, vW = nullptr, vH = nullptr, vFps = nullptr, vText = nullptr;
    napi_get_boolean(env, AndroidRuntime::Instance().IsRunning(), &vRunning);
    napi_get_boolean(env, HwcRenderer::Instance().IsConnected(), &vConn);
    napi_create_int64(env, static_cast<int64_t>(HwcRenderer::Instance().FrameCount()), &vFrames);
    napi_create_int32(env, HwcRenderer::Instance().FrameWidth(), &vW);
    napi_create_int32(env, HwcRenderer::Instance().FrameHeight(), &vH);
    napi_create_int32(env, HwcRenderer::Instance().Fps(), &vFps);
    std::string st = AndroidRuntime::Instance().StatusText();
    napi_create_string_utf8(env, st.c_str(), NAPI_AUTO_LENGTH, &vText);

    napi_set_named_property(env, obj, "running", vRunning);
    napi_set_named_property(env, obj, "connected", vConn);
    napi_set_named_property(env, obj, "frames", vFrames);
    napi_set_named_property(env, obj, "width", vW);
    napi_set_named_property(env, obj, "height", vH);
    napi_set_named_property(env, obj, "fps", vFps);
    napi_set_named_property(env, obj, "statusText", vText);
    return obj;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "startContainer", nullptr, StartContainerNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "attachHwcSocket", nullptr, AttachHwcSocketNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stopContainer", nullptr, StopContainerNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getStatus", nullptr, GetStatusNapi, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // Register XComponent callbacks when loaded via XComponent({ libraryname: 'entry' })
    napi_value xcVal = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcVal) == napi_ok && xcVal) {
        OH_NativeXComponent* nativeXC = nullptr;
        if (napi_unwrap(env, xcVal, reinterpret_cast<void**>(&nativeXC)) == napi_ok && nativeXC) {
            OH_NativeXComponent_RegisterCallback(nativeXC, &g_xcCallback);
            OH_LOG_INFO(LOG_APP, "Registered OH_NativeXComponent callbacks");
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
