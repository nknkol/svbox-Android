/* SPDX-License-Identifier: Apache-2.0 */
#include "NativeSubWindow.h"

#include <dlfcn.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

namespace {

using FnRef = int32_t (*)(void*);
using FnUnref = int32_t (*)(void*);
using FnHandleOpt = int32_t (*)(OHNativeWindow*, int, ...);

struct NativeWinApi {
    FnRef ref = nullptr;
    FnUnref unref = nullptr;
    FnHandleOpt opt = nullptr;

    NativeWinApi() {
        void* h = dlopen("libnative_window.so", RTLD_LAZY | RTLD_LOCAL);
        if (!h) h = RTLD_DEFAULT;
        ref = reinterpret_cast<FnRef>(dlsym(h, "OH_NativeWindow_NativeObjectReference"));
        unref = reinterpret_cast<FnUnref>(dlsym(h, "OH_NativeWindow_NativeObjectUnreference"));
        opt = reinterpret_cast<FnHandleOpt>(dlsym(h, "OH_NativeWindow_NativeWindowHandleOpt"));
    }
};

const NativeWinApi& Api() {
    static NativeWinApi api;
    return api;
}

} // namespace

EGLNativeWindowType createSubWindow(FBNativeWindowType p_window,
                                    int /*x*/,
                                    int /*y*/,
                                    int width,
                                    int height,
                                    float /*dpr*/,
                                    SubWindowRepaintCallback /*repaint_callback*/,
                                    void* /*repaint_callback_param*/,
                                    int /*hideWindow*/) {
    if (!p_window) {
        return static_cast<EGLNativeWindowType>(0);
    }
    const auto& api = Api();
    if (api.ref) api.ref(p_window);
    if (api.opt) {
        if (width > 0 && height > 0) {
            api.opt(p_window, SET_BUFFER_GEOMETRY, width, height);
        }
        api.opt(p_window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    }
    return reinterpret_cast<EGLNativeWindowType>(p_window);
}

void destroySubWindow(EGLNativeWindowType win) {
    if (!win) {
        return;
    }
    const auto& api = Api();
    if (api.unref) api.unref(reinterpret_cast<OHNativeWindow*>(win));
}

int moveSubWindow(FBNativeWindowType /*p_parent_window*/,
                  EGLNativeWindowType p_sub_window,
                  int /*x*/,
                  int /*y*/,
                  int width,
                  int height,
                  float /*dpr*/) {
    const auto& api = Api();
    if (p_sub_window && api.opt && width > 0 && height > 0) {
        api.opt(reinterpret_cast<OHNativeWindow*>(p_sub_window),
                SET_BUFFER_GEOMETRY, width, height);
    }
    return 1;
}
