#!/usr/bin/env python3
"""Idempotently apply minimal HarmonyOS (__OHOS__) host patches to gfxstream & aemu."""
import os
import sys


def patch_file(path: str, old: str, new: str, tag: str) -> None:
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    if new in src:
        print(f"[patch] already applied: {tag}")
        return
    if old not in src:
        raise RuntimeError(f"Patch anchor not found for {tag} in {path}")
    src = src.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    print(f"[patch] applied: {tag}")


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tp = os.path.join(root, "third_party")
    gfx = os.path.join(tp, "gfxstream")

    # 1. render_api_platform_types.h: OHNativeWindow* as FBNativeWindowType
    patch_file(
        os.path.join(gfx, "include/render-utils/render_api_platform_types.h"),
        "#elif defined(ANDROID)\n"
        "#include <android/native_window.h>\n"
        "typedef ANativeWindow*    FBNativeWindowType;",
        "#elif defined(ANDROID)\n"
        "#include <android/native_window.h>\n"
        "typedef ANativeWindow*    FBNativeWindowType;\n"
        "#elif defined(__OHOS__)\n"
        "#include <native_window/external_window.h>\n"
        "typedef OHNativeWindow*   FBNativeWindowType;",
        "render_api_platform_types OHOS",
    )

    # 2. EglGlobalInfo.cpp: force egl2egl on OHOS (no GLX)
    patch_file(
        os.path.join(gfx, "host/gl/glestranslator/EGL/EglGlobalInfo.cpp"),
        "#if defined(ANDROID) || defined(__QNX__)",
        "#if defined(ANDROID) || defined(__QNX__) || defined(__OHOS__)",
        "EglGlobalInfo OHOS egl2egl",
    )

    # 3. EglOsApi_egl.cpp: OHOS headers, libGLESv3.so, no X11/GLX, OHNativeWindow queries
    egl_api = os.path.join(gfx, "host/gl/glestranslator/EGL/EglOsApi_egl.cpp")
    patch_file(
        egl_api,
        "#ifdef __linux__\n"
        "#include \"X11Support.h\"\n"
        "#include \"X11ErrorHandler.h\"\n"
        "#endif",
        "#if defined(__OHOS__)\n"
        "#include <dlfcn.h>\n"
        "#include <native_window/external_window.h>\n"
        "#elif defined(__linux__)\n"
        "#include \"X11Support.h\"\n"
        "#include \"X11ErrorHandler.h\"\n"
        "#endif",
        "EglOsApi_egl OHOS include",
    )
    patch_file(
        egl_api,
        "#elif defined(__linux__)\n\n\n"
        "static const char* kEGLLibName = \"libEGL.so\";\n"
        "static const char* kGLES2LibName = \"libGLESv2.so\";\n\n"
        "static const char* kEGLLibNameAlt = \"libEGL.so.1\";\n"
        "static const char* kGLES2LibNameAlt = \"libGLESv2.so.2\";",
        "#elif defined(__OHOS__)\n"
        "static const char* kEGLLibName = \"libEGL.so\";\n"
        "static const char* kGLES2LibName = \"libGLESv3.so\";\n"
        "static const char* kEGLLibNameAlt = \"libEGL.so\";\n"
        "static const char* kGLES2LibNameAlt = \"libGLESv2.so\";\n"
        "#elif defined(__linux__)\n\n\n"
        "static const char* kEGLLibName = \"libEGL.so\";\n"
        "static const char* kGLES2LibName = \"libGLESv2.so\";\n\n"
        "static const char* kEGLLibNameAlt = \"libEGL.so.1\";\n"
        "static const char* kGLES2LibNameAlt = \"libGLESv2.so.2\";",
        "EglOsApi_egl OHOS libGLESv3",
    )
    patch_file(
        egl_api,
        "#ifdef __linux__\n"
        "    ::Display* mGlxDisplay = nullptr;\n"
        "#endif // __linux__",
        "#if defined(__linux__) && !defined(__OHOS__)\n"
        "    ::Display* mGlxDisplay = nullptr;\n"
        "#endif // __linux__",
        "EglOsApi_egl OHOS no mGlxDisplay",
    )
    patch_file(
        egl_api,
        "#ifdef ANDROID\n"
        "    mGlxDisplay = nullptr;\n"
        "#elif defined(__linux__)\n"
        "    if (mHeadless) mGlxDisplay = nullptr;\n"
        "    else mGlxDisplay = getX11Api()->XOpenDisplay(0);\n"
        "#endif // __linux__",
        "#if defined(ANDROID) || defined(__OHOS__)\n"
        "    // No X11 display on Android/OHOS\n"
        "#elif defined(__linux__)\n"
        "    if (mHeadless) mGlxDisplay = nullptr;\n"
        "    else mGlxDisplay = getX11Api()->XOpenDisplay(0);\n"
        "#endif // __linux__",
        "EglOsApi_egl OHOS no XOpenDisplay",
    )
    patch_file(
        egl_api,
        "EglOsEglDisplay::~EglOsEglDisplay() {\n"
        "#ifdef ANDROID\n"
        "#elif defined(__linux__)\n"
        "    if (mGlxDisplay) getX11Api()->XCloseDisplay(mGlxDisplay);\n"
        "#endif // __linux__\n"
        "}",
        "EglOsEglDisplay::~EglOsEglDisplay() {\n"
        "#if defined(ANDROID) || defined(__OHOS__)\n"
        "#elif defined(__linux__)\n"
        "    if (mGlxDisplay) getX11Api()->XCloseDisplay(mGlxDisplay);\n"
        "#endif // __linux__\n"
        "}",
        "EglOsApi_egl OHOS no XCloseDisplay",
    )
    patch_file(
        egl_api,
        "#elif defined(ANDROID)\n"
        "    return true;\n"
        "#elif defined(__linux__)\n"
        "    Window root;",
        "#elif defined(ANDROID) || defined(__OHOS__)\n"
        "    return win != 0;\n"
        "#elif defined(__linux__)\n"
        "    Window root;",
        "EglOsApi_egl OHOS isValidNativeWin",
    )
    patch_file(
        egl_api,
        "#elif defined(ANDROID)\n"
        "    *width = ANativeWindow_getWidth((ANativeWindow*)win);\n"
        "    *height = ANativeWindow_getHeight((ANativeWindow*)win);\n"
        "    return true;\n"
        "#elif defined(__linux__)",
        "#elif defined(ANDROID)\n"
        "    *width = ANativeWindow_getWidth((ANativeWindow*)win);\n"
        "    *height = ANativeWindow_getHeight((ANativeWindow*)win);\n"
        "    return true;\n"
        "#elif defined(__OHOS__)\n"
        "    using FnHandleOpt = int32_t (*)(OHNativeWindow*, int, ...);\n"
        "    static FnHandleOpt sOpt = []() -> FnHandleOpt {\n"
        "        void* h = dlopen(\"libnative_window.so\", RTLD_LAZY | RTLD_LOCAL);\n"
        "        return reinterpret_cast<FnHandleOpt>(dlsym(h ? h : RTLD_DEFAULT, \"OH_NativeWindow_NativeWindowHandleOpt\"));\n"
        "    }();\n"
        "    int32_t ohH = 0, ohW = 0;\n"
        "    if (sOpt && sOpt((OHNativeWindow*)win, GET_BUFFER_GEOMETRY, &ohH, &ohW) == 0 && ohW > 0 && ohH > 0) {\n"
        "        *width = static_cast<unsigned int>(ohW);\n"
        "        *height = static_cast<unsigned int>(ohH);\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "#elif defined(__linux__)",
        "EglOsApi_egl OHOS checkWindowPixelFormatMatch",
    )
    patch_file(
        egl_api,
        "#ifdef __APPLE__\n"
        "    win = nsGetLayer(win);\n"
        "#endif\n"
        "    EGLSurface surface = mDispatcher.eglCreateWindowSurface(",
        "#ifdef __APPLE__\n"
        "    win = nsGetLayer(win);\n"
        "#endif\n"
        "#ifdef __OHOS__\n"
        "    {\n"
        "        EGLint nativeVisualId = 0;\n"
        "        if (mDispatcher.eglGetConfigAttrib(mDisplay, ((EglOsEglPixelFormat*)pf)->mConfigId,\n"
        "                                           EGL_NATIVE_VISUAL_ID, &nativeVisualId) && nativeVisualId > 0) {\n"
        "            using FnHandleOpt = int32_t (*)(OHNativeWindow*, int, ...);\n"
        "            static FnHandleOpt sOpt = []() -> FnHandleOpt {\n"
        "                void* h = dlopen(\"libnative_window.so\", RTLD_LAZY | RTLD_LOCAL);\n"
        "                return reinterpret_cast<FnHandleOpt>(dlsym(h ? h : RTLD_DEFAULT, \"OH_NativeWindow_NativeWindowHandleOpt\"));\n"
        "            }();\n"
        "            if (sOpt && win) {\n"
        "                sOpt((OHNativeWindow*)win, SET_FORMAT, nativeVisualId);\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "#endif\n"
        "    EGLSurface surface = mDispatcher.eglCreateWindowSurface(",
        "EglOsApi_egl OHOS sync SET_FORMAT with EGL_NATIVE_VISUAL_ID",
    )
    emugl = os.path.join(gfx, "host/gl/EmulationGl.cpp")
    patch_file(
        emugl,
        "    const GLint configAttribs[] = {\n"
        "        EGL_RED_SIZE, kWantedRedSize,             //\n"
        "        EGL_GREEN_SIZE, kWantedGreenSize,         //\n"
        "        EGL_BLUE_SIZE, kWantedBlueSize,           //\n"
        "        EGL_SURFACE_TYPE, surfaceType,            //\n"
        "        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,  //\n"
        "        EGL_NONE,                                 //\n"
        "    };",
        "    const GLint configAttribs[] = {\n"
        "        EGL_RED_SIZE, kWantedRedSize,             //\n"
        "        EGL_GREEN_SIZE, kWantedGreenSize,         //\n"
        "        EGL_BLUE_SIZE, kWantedBlueSize,           //\n"
        "#ifdef __OHOS__\n"
        "        EGL_ALPHA_SIZE, 8,                        //\n"
        "#endif\n"
        "        EGL_SURFACE_TYPE, surfaceType,            //\n"
        "        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,  //\n"
        "        EGL_NONE,                                 //\n"
        "    };",
        "EmulationGl OHOS prefer RGBA8888 config",
    )

    # 4. DisplaySurfaceVk.cpp: VK_OHOS_surface support
    dsvk = os.path.join(gfx, "host/vulkan/DisplaySurfaceVk.cpp")
    patch_file(
        dsvk,
        "#include \"vk_util.h\"",
        "#include \"vk_util.h\"\n"
        "#ifdef __OHOS__\n"
        "#include <vulkan/vulkan_ohos.h>\n"
        "#endif",
        "DisplaySurfaceVk OHOS include",
    )
    patch_file(
        dsvk,
        "#ifdef _WIN32\n"
        "    const VkWin32SurfaceCreateInfoKHR surfaceCi = {\n"
        "        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,\n"
        "        .pNext = nullptr,\n"
        "        .flags = 0,\n"
        "        .hinstance = GetModuleHandle(nullptr),\n"
        "        .hwnd = window,\n"
        "    };\n"
        "    VK_CHECK(vk.vkCreateWin32SurfaceKHR(instance, &surfaceCi, nullptr, &surface));\n"
        "#else",
        "#ifdef _WIN32\n"
        "    const VkWin32SurfaceCreateInfoKHR surfaceCi = {\n"
        "        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,\n"
        "        .pNext = nullptr,\n"
        "        .flags = 0,\n"
        "        .hinstance = GetModuleHandle(nullptr),\n"
        "        .hwnd = window,\n"
        "    };\n"
        "    VK_CHECK(vk.vkCreateWin32SurfaceKHR(instance, &surfaceCi, nullptr, &surface));\n"
        "#elif defined(__OHOS__)\n"
        "    auto createSurfaceOhos = reinterpret_cast<PFN_vkCreateSurfaceOHOS>(\n"
        "        vk.vkGetInstanceProcAddr(instance, \"vkCreateSurfaceOHOS\"));\n"
        "    if (!createSurfaceOhos) {\n"
        "        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER)) << \"Missing vkCreateSurfaceOHOS\";\n"
        "    }\n"
        "    const VkSurfaceCreateInfoOHOS surfaceCi = {\n"
        "        .sType = static_cast<VkStructureType>(1000685000),\n"
        "        .pNext = nullptr,\n"
        "        .flags = 0,\n"
        "        .window = window,\n"
        "    };\n"
        "    VK_CHECK(createSurfaceOhos(instance, &surfaceCi, nullptr, &surface));\n"
        "#else",
        "DisplaySurfaceVk OHOS vkCreateSurfaceOHOS",
    )

    # 5. VkCommonOperations.cpp & SwapChainStateVk.cpp: enable VK_OHOS_surface instance extension
    vkops = os.path.join(gfx, "host/vulkan/VkCommonOperations.cpp")
    patch_file(
        vkops,
        "    std::vector<const char*> surfaceInstanceExtNames = {\n"
        "        VK_KHR_SURFACE_EXTENSION_NAME,\n"
        "    };",
        "    std::vector<const char*> surfaceInstanceExtNames = {\n"
        "        VK_KHR_SURFACE_EXTENSION_NAME,\n"
        "#ifdef __OHOS__\n"
        "        \"VK_OHOS_surface\",\n"
        "#endif\n"
        "    };",
        "VkCommonOperations OHOS surface ext",
    )
    patch_file(
        vkops,
        "    if (res != VK_SUCCESS) {\n"
        "        if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {\n"
        "            VERBOSE(\"Not Supported: %s %s %s %s\", string_VkFormat(info->format),",
        "    if (res != VK_SUCCESS) {\n"
        "        if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {\n"
        "            // Retry without VkPhysicalDeviceExternalImageFormatInfo: mobile/OHOS GPUs\n"
        "            // (e.g. Maleoon 916) support these formats natively for Vulkan ColorBuffers\n"
        "            // even when OPAQUE_FD external image export is unsupported.\n"
        "            VkImageFormatProperties plainProps = {};\n"
        "            VkResult plainRes = vk->vkGetPhysicalDeviceImageFormatProperties(\n"
        "                physdev, info->format, info->type, info->tiling, info->usageFlags,\n"
        "                info->createFlags, &plainProps);\n"
        "            if (plainRes == VK_SUCCESS) {\n"
        "                info->supported = true;\n"
        "                info->supportsExternalMemory = false;\n"
        "                info->requiresDedicatedAllocation = false;\n"
        "                info->imageFormatProps2 = {\n"
        "                    VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,\n"
        "                    nullptr,\n"
        "                    plainProps,\n"
        "                };\n"
        "                return true;\n"
        "            }\n"
        "            VERBOSE(\"Not Supported: %s %s %s %s\", string_VkFormat(info->format),",
        "VkCommonOperations non-external format fallback",
    )
    patch_file(
        vkops,
        "    emulation->mImageSupportInfo = getBasicImageSupportList();\n"
        "    for (size_t i = 0; i < emulation->mImageSupportInfo.size(); ++i) {\n"
        "        emulation->populateImageFormatExternalMemorySupportInfo(ivk, emulation->mPhysicalDevice,\n"
        "                                                                &emulation->mImageSupportInfo[i]);\n"
        "    }",
        "    emulation->mImageSupportInfo = getBasicImageSupportList();\n"
        "    bool anyImageSupportsExtMem = false;\n"
        "    for (size_t i = 0; i < emulation->mImageSupportInfo.size(); ++i) {\n"
        "        emulation->populateImageFormatExternalMemorySupportInfo(ivk, emulation->mPhysicalDevice,\n"
        "                                                                &emulation->mImageSupportInfo[i]);\n"
        "        if (emulation->mImageSupportInfo[i].supported &&\n"
        "            emulation->mImageSupportInfo[i].supportsExternalMemory) {\n"
        "            anyImageSupportsExtMem = true;\n"
        "        }\n"
        "    }\n"
        "    if (!anyImageSupportsExtMem) {\n"
        "        emulation->mDeviceInfo.supportsExternalMemoryExport = false;\n"
        "    }",
        "VkCommonOperations disable extmem export when images unsupported",
    )
    patch_file(
        vkops,
        "        bool allocRes = allocExternalMemory(vk, &infoPtr->memory, true /*actuallyExternal*/,\n"
        "                                            deviceAlignment, kNullopt, dedicatedImage);",
        "        bool allocRes = allocExternalMemory(\n"
        "            vk, &infoPtr->memory, mDeviceInfo.supportsExternalMemoryExport /*actuallyExternal*/,\n"
        "            deviceAlignment, kNullopt, dedicatedImage);",
        "VkCommonOperations respect supportsExternalMemoryExport in alloc",
    )
    scvk_h = os.path.join(gfx, "host/vulkan/SwapChainStateVk.h")
    patch_file(
        scvk_h,
        "    const static VkFormat k_vkFormat = VK_FORMAT_B8G8R8A8_UNORM;",
        "#ifdef __OHOS__\n"
        "    const static VkFormat k_vkFormat = VK_FORMAT_R8G8B8A8_UNORM;\n"
        "#else\n"
        "    const static VkFormat k_vkFormat = VK_FORMAT_B8G8R8A8_UNORM;\n"
        "#endif",
        "SwapChainStateVk.h OHOS RGBA8 swapchain format",
    )
    scvk = os.path.join(gfx, "host/vulkan/SwapChainStateVk.cpp")
    patch_file(
        scvk,
        "#ifdef VK_USE_PLATFORM_XCB_KHR\n"
        "            VK_KHR_XCB_SURFACE_EXTENSION_NAME,\n"
        "#endif\n"
        "    };",
        "#ifdef VK_USE_PLATFORM_XCB_KHR\n"
        "            VK_KHR_XCB_SURFACE_EXTENSION_NAME,\n"
        "#endif\n"
        "#ifdef __OHOS__\n"
        "            \"VK_OHOS_surface\",\n"
        "#endif\n"
        "    };",
        "SwapChainStateVk OHOS surface ext",
    )
    patch_file(
        scvk,
        "        VkImageViewCreateInfo imageViewCi = {\n"
        "            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,\n"
        "            .image = m_vkImages[i],\n"
        "            .viewType = VK_IMAGE_VIEW_TYPE_2D,\n"
        "            .format = k_vkFormat,",
        "        VkImageViewCreateInfo imageViewCi = {\n"
        "            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,\n"
        "            .image = m_vkImages[i],\n"
        "            .viewType = VK_IMAGE_VIEW_TYPE_2D,\n"
        "            .format = swapChainCi.imageFormat,",
        "SwapChainStateVk use chosen swapchain imageFormat for views",
    )
    patch_file(
        scvk,
        "    auto iSurfaceFormat =\n"
        "        std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& format) {\n"
        "            return format.format == k_vkFormat && format.colorSpace == k_vkColorSpace;\n"
        "        });\n"
        "    if (iSurfaceFormat == formats.end()) {\n"
        "        ERR(\"Failed to create swapchain: the format(%#\" PRIx64\n"
        "                                \") with color space(%#\" PRIx64 \") not supported.\",\n"
        "                                static_cast<uint64_t>(k_vkFormat),\n"
        "                                static_cast<uint64_t>(k_vkColorSpace));\n"
        "        return std::nullopt;\n"
        "    }",
        "    auto iSurfaceFormat =\n"
        "        std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& format) {\n"
        "            return format.format == k_vkFormat && format.colorSpace == k_vkColorSpace;\n"
        "        });\n"
        "    if (iSurfaceFormat == formats.end() && !formats.empty()) {\n"
        "        iSurfaceFormat = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {\n"
        "            return f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM;\n"
        "        });\n"
        "        if (iSurfaceFormat == formats.end()) iSurfaceFormat = formats.begin();\n"
        "    }\n"
        "    if (iSurfaceFormat == formats.end()) {\n"
        "        ERR(\"Failed to create swapchain: no supported surface formats.\");\n"
        "        return std::nullopt;\n"
        "    }",
        "SwapChainStateVk surface format fallback",
    )
    patch_file(
        scvk,
        "    VkFormatProperties formatProperties = {};\n"
        "    vk.vkGetPhysicalDeviceFormatProperties(physicalDevice, k_vkFormat, &formatProperties);",
        "    VkFormatProperties formatProperties = {};\n"
        "    vk.vkGetPhysicalDeviceFormatProperties(physicalDevice, iSurfaceFormat->format, &formatProperties);",
        "SwapChainStateVk check chosen format properties",
    )
    patch_file(
        scvk,
        "        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,",
        "        .compositeAlpha = (surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)\n"
        "                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR\n"
        "                              : ((surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)\n"
        "                                     ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR\n"
        "                                     : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR),",
        "SwapChainStateVk OHOS compositeAlpha fallback",
    )
    patch_file(
        scvk,
        "    uint32_t imageCount = surfaceCaps.minImageCount + 1;\n"
        "    if (surfaceCaps.maxImageCount != 0 && surfaceCaps.maxImageCount < imageCount) {\n"
        "        imageCount = surfaceCaps.maxImageCount;\n"
        "    }",
        "#ifdef __OHOS__\n"
        "    // OHOS XComponent NativeWindow BufferQueue has 3 slots by default;\n"
        "    // requesting 4 triggers VulkanSwapchainLayer RequestBuffer[3] timeout (40601000).\n"
        "    uint32_t imageCount = surfaceCaps.minImageCount;\n"
        "    if (imageCount < 2) imageCount = 2;\n"
        "    if (imageCount > 3) imageCount = 3;\n"
        "#else\n"
        "    uint32_t imageCount = surfaceCaps.minImageCount + 1;\n"
        "#endif\n"
        "    if (surfaceCaps.maxImageCount != 0 && surfaceCaps.maxImageCount < imageCount) {\n"
        "        imageCount = surfaceCaps.maxImageCount;\n"
        "    }",
        "SwapChainStateVk OHOS clamp swapchain imageCount <= 3",
    )

    # 6. FrameBuffer.cpp: when using Vulkan native swapchain without GL/Vk external-memory interop
    #    (e.g. Maleoon 916), GL-composited SurfaceFlinger frames must be marked dirty so
    #    borrowColorBufferForDisplay -> invalidateColorBufferForVk syncs GL -> Vk before present.
    fb_cpp = os.path.join(gfx, "host/FrameBuffer.cpp")
    patch_file(
        fb_cpp,
        "bool FrameBuffer::post(HandleType p_colorbuffer, bool needLockAndBind) {\n"
        "#if GFXSTREAM_ENABLE_HOST_GLES\n"
        "    if (m_features.GuestVulkanOnly.enabled) {\n"
        "        flushColorBufferFromGl(p_colorbuffer);\n"
        "    }\n"
        "#endif",
        "bool FrameBuffer::post(HandleType p_colorbuffer, bool needLockAndBind) {\n"
        "#if GFXSTREAM_ENABLE_HOST_GLES\n"
        "    if (m_features.GuestVulkanOnly.enabled || m_useVulkanComposition) {\n"
        "        flushColorBufferFromGl(p_colorbuffer);\n"
        "    }\n"
        "#endif",
        "FrameBuffer::post flushColorBufferFromGl for Vulkan composition",
    )
    patch_file(
        fb_cpp,
        "void FrameBuffer::postWithCallback(HandleType p_colorbuffer, Post::CompletionCallback callback,\n"
        "                                   bool needLockAndBind) {\n"
        "#if GFXSTREAM_ENABLE_HOST_GLES\n"
        "    if (m_features.GuestVulkanOnly.enabled) {\n"
        "        flushColorBufferFromGl(p_colorbuffer);\n"
        "    }\n"
        "#endif",
        "void FrameBuffer::postWithCallback(HandleType p_colorbuffer, Post::CompletionCallback callback,\n"
        "                                   bool needLockAndBind) {\n"
        "#if GFXSTREAM_ENABLE_HOST_GLES\n"
        "    if (m_features.GuestVulkanOnly.enabled || m_useVulkanComposition) {\n"
        "        flushColorBufferFromGl(p_colorbuffer);\n"
        "    }\n"
        "#endif",
        "FrameBuffer::postWithCallback flushColorBufferFromGl for Vulkan composition",
    )
    patch_file(
        fb_cpp,
        "                if (m_lastPostedColorBuffer) {\n"
        "                    GL_LOG(\"setupSubwindow: draw last posted cb\");\n"
        "                    postImpl(m_lastPostedColorBuffer,\n"
        "                        [](std::shared_future<void> waitForGpu) {}, false);\n"
        "                } else {\n"
        "                    Post clearCmd;\n"
        "                    clearCmd.cmd = PostCmd::Clear;\n"
        "                    sendPostWorkerCmd(std::move(clearCmd));\n"
        "                }\n"
        "            }\n"
        "            m_windowContentFullWidth = fbw;",
        "                if (m_lastPostedColorBuffer) {\n"
        "                    GL_LOG(\"setupSubwindow: draw last posted cb\");\n"
        "                    postImpl(m_lastPostedColorBuffer,\n"
        "                        [](std::shared_future<void> waitForGpu) {}, false);\n"
        "                } else {\n"
        "                    Post clearCmd;\n"
        "                    clearCmd.cmd = PostCmd::Clear;\n"
        "                    sendPostWorkerCmd(std::move(clearCmd));\n"
        "                }\n"
        "            } else if (m_lastPostedColorBuffer) {\n"
        "                // Redraw the last committed frame immediately when re-attaching a\n"
        "                // Vulkan swapchain (e.g. reopening the Classic Emulator window while\n"
        "                // Android is idle) so the window does not stay black until next input.\n"
        "                GL_LOG(\"setupSubwindow: draw last posted cb (vk)\");\n"
        "                postImpl(m_lastPostedColorBuffer,\n"
        "                    [](std::shared_future<void> waitForGpu) {}, false);\n"
        "            }\n"
        "            m_windowContentFullWidth = fbw;",
        "FrameBuffer::setupSubWindow redraw last posted cb on Vulkan swapchain attach",
    )
    patch_file(
        fb_cpp,
        "bool FrameBuffer::repost(bool needLockAndBind) {\n"
        "    GL_LOG(\"Reposting framebuffer.\");\n"
        "    if (m_displayVk) {\n"
        "        setGuestPostedAFrame();\n"
        "        return true;\n"
        "    }\n"
        "    if (m_lastPostedColorBuffer && sInitialized.load(std::memory_order_relaxed)) {",
        "bool FrameBuffer::repost(bool needLockAndBind) {\n"
        "    GL_LOG(\"Reposting framebuffer.\");\n"
        "    if (m_lastPostedColorBuffer && sInitialized.load(std::memory_order_relaxed)) {",
        "FrameBuffer::repost present last posted cb in Vulkan mode too",
    )
    patch_file(
        fb_cpp,
        "bool FrameBuffer::removeSubWindow() {\n"
        "    if (!m_useSubWindow) {\n"
        "        ERR(\"Cannot remove native sub-window in this configuration\");\n"
        "        return false;\n"
        "    }\n"
        "    AutoLock lock(sGlobals()->lock);\n"
        "    sInitialized.store(false, std::memory_order_relaxed);\n"
        "    sGlobals()->condVar.broadcastAndUnlock(&lock);",
        "bool FrameBuffer::removeSubWindow() {\n"
        "    if (!m_useSubWindow) {\n"
        "        ERR(\"Cannot remove native sub-window in this configuration\");\n"
        "        return false;\n"
        "    }\n"
        "    // Keep sInitialized=true when detaching slot-0 subwindow so multi-window\n"
        "    // secondary displays and shutdown RenderThread teardown never deadlock in\n"
        "    // FrameBuffer::waitUntilInitialized().",
        "FrameBuffer::removeSubWindow keep sInitialized=true to avoid Stop/multi-window deadlock",
    )
    fb_h = os.path.join(gfx, "host/FrameBuffer.h")
    patch_file(
        fb_h,
        "    void setShuttingDown() { m_shuttingDown = true; }",
        "    void setShuttingDown();",
        "FrameBuffer::setShuttingDown declare out-of-line",
    )
    patch_file(
        fb_cpp,
        "void FrameBuffer::waitUntilInitialized() {",
        "void FrameBuffer::setShuttingDown() {\n"
        "    m_shuttingDown = true;\n"
        "    AutoLock lock(sGlobals()->lock);\n"
        "    sInitialized.store(true, std::memory_order_release);\n"
        "    sGlobals()->condVar.broadcastAndUnlock(&lock);\n"
        "}\n\n"
        "void FrameBuffer::waitUntilInitialized() {",
        "FrameBuffer::setShuttingDown wake waitUntilInitialized waiters",
    )

    # 7. VkDecoderGlobalState.cpp: fallback for vkGetPhysicalDeviceImageFormatProperties2 when
    #    host GPU (Maleoon 916) does not support OPAQUE_FD external image queries, so Android
    #    SkiaVK (libhwui) AHardwareBuffer image format queries succeed cleanly.
    vkdgs = os.path.join(gfx, "host/vulkan/VkDecoderGlobalState.cpp")
    patch_file(
        vkdgs,
        "        if (res != VK_SUCCESS) {\n"
        "            return res;\n"
        "        }\n\n"
        "        VkExternalImageFormatProperties* extImageFormatProps =\n"
        "            vk_find_struct<VkExternalImageFormatProperties>(pImageFormatProperties);\n\n"
        "        // Only allow dedicated allocations for external images.\n"
        "        if (extImageFormatInfo && extImageFormatProps) {\n"
        "            extImageFormatProps->externalMemoryProperties.externalMemoryFeatures |=\n"
        "                VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;\n"
        "        }",
        "        VkExternalImageFormatProperties* extImageFormatProps =\n"
        "            vk_find_struct<VkExternalImageFormatProperties>(pImageFormatProperties);\n"
        "        if (res == VK_ERROR_FORMAT_NOT_SUPPORTED && extImageFormatInfo) {\n"
        "            VkResult plainRes = vk->vkGetPhysicalDeviceImageFormatProperties(\n"
        "                physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,\n"
        "                pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,\n"
        "                &pImageFormatProperties->imageFormatProperties);\n"
        "            if (plainRes == VK_SUCCESS) {\n"
        "                res = VK_SUCCESS;\n"
        "                if (extImageFormatProps) {\n"
        "                    const auto defHandleType = m_vkEmulation->getDefaultExternalMemoryHandleType();\n"
        "                    extImageFormatProps->externalMemoryProperties.externalMemoryFeatures =\n"
        "                        VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT |\n"
        "                        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |\n"
        "                        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;\n"
        "                    extImageFormatProps->externalMemoryProperties.exportFromImportedHandleTypes =\n"
        "                        defHandleType;\n"
        "                    extImageFormatProps->externalMemoryProperties.compatibleHandleTypes =\n"
        "                        defHandleType;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        if (res != VK_SUCCESS) {\n"
        "            return res;\n"
        "        }\n\n"
        "        // Only allow dedicated allocations for external images.\n"
        "        if (extImageFormatInfo && extImageFormatProps) {\n"
        "            extImageFormatProps->externalMemoryProperties.externalMemoryFeatures |=\n"
        "                VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;\n"
        "        }",
        "VkDecoderGlobalState non-external fallback for GetPhysicalDeviceImageFormatProperties2",
    )

    # 8. TextureDraw.cpp: log shader compile errors via ERR() so they appear in hilog
    texdraw = os.path.join(gfx, "host/gl/TextureDraw.cpp")
    patch_file(
        texdraw,
        "        fprintf(stderr, \"%s: TextureDraw shader compile failed.\\n\", __func__);\n"
        "        s_gles2.glGetShaderInfoLog(shader, infoLogLength, 0, &infoLog[0]);\n"
        "        fprintf(stderr, \"%s: Info log:\\n%s\\n\", __func__,\n"
        "                infoLog.c_str());",
        "        s_gles2.glGetShaderInfoLog(shader, infoLogLength, 0, &infoLog[0]);\n"
        "        ERR(\"%s: TextureDraw shader compile failed: %s\", __func__, infoLog.c_str());",
        "TextureDraw log shader compile error via ERR",
    )

    # 9. EglOsApi_egl.cpp: log swapBuffers failure via ERR()
    patch_file(
        egl_api,
        "void EglOsEglDisplay::swapBuffers(Surface* surface) {\n"
        "    D(\"%s\\n\", __FUNCTION__);\n"
        "    EglOsEglSurface* sfc = (EglOsEglSurface*)surface;\n"
        "    mDispatcher.eglSwapBuffers(mDisplay, sfc->getHndl());\n"
        "}",
        "void EglOsEglDisplay::swapBuffers(Surface* surface) {\n"
        "    D(\"%s\\n\", __FUNCTION__);\n"
        "    EglOsEglSurface* sfc = (EglOsEglSurface*)surface;\n"
        "    if (!mDispatcher.eglSwapBuffers(mDisplay, sfc->getHndl())) {\n"
        "        static int sSwapErrs = 0;\n"
        "        if (sSwapErrs++ < 5) {\n"
        "            fprintf(stderr, \"EglOsEglDisplay::swapBuffers failed err=0x%x\\n\", mDispatcher.eglGetError());\n"
        "        }\n"
        "    }\n"
        "}",
        "EglOsApi_egl log swapBuffers failure",
    )

    # 10. GLESVersionDetector.cpp: OHOS host uses native OpenGL ES 3.2 (egl2egl), NEVER Desktop GL Core Profile.
    #     Enabling Core Profile causes glestranslator to emit '#version 330 core' shaders which Maleoon 916
    #     GLES 3.2 rejects ("Language version '330' unknown, only supports up to '320 es'"), breaking all blits!
    gles_ver = os.path.join(gfx, "host/gl/GLESVersionDetector.cpp")
    patch_file(
        gles_ver,
        "bool shouldEnableCoreProfile() {\n"
        "    int dispatchMaj, dispatchMin;\n\n"
        "    emugl::getGlesVersion(&dispatchMaj, &dispatchMin);\n"
        "    return emugl::getRenderer() == SELECTED_RENDERER_HOST &&\n"
        "           dispatchMaj > 2;\n"
        "}",
        "bool shouldEnableCoreProfile() {\n"
        "#ifdef __OHOS__\n"
        "    return false;\n"
        "#else\n"
        "    int dispatchMaj, dispatchMin;\n\n"
        "    emugl::getGlesVersion(&dispatchMaj, &dispatchMin);\n"
        "    return emugl::getRenderer() == SELECTED_RENDERER_HOST &&\n"
        "           dispatchMaj > 2;\n"
        "#endif\n"
        "}",
        "GLESVersionDetector OHOS disable desktop GL Core Profile",
    )

    # 11. OH_NativeBuffer zero-copy Vulkan + GL interop on HarmonyOS Maleoon 916:
    #     Maleoon 916 implements external VkImage memory via VK_OHOS_external_memory (OH_NativeBuffer)
    #     rather than OPAQUE_FD, and imports OH_NativeBuffer into EGL via EGL_NATIVE_BUFFER_OHOS (0x34E1).
    vk_sid = os.path.join(gfx, "host/vulkan/cereal/common/vk_struct_id.h")
    patch_file(
        vk_sid,
        "#include <vulkan/vulkan.h>\n"
        "#include <vulkan/vulkan_core.h>",
        "#include <vulkan/vulkan.h>\n"
        "#include <vulkan/vulkan_core.h>\n"
        "#ifdef __OHOS__\n"
        "#include <vulkan/vulkan_ohos.h>\n"
        "#include <native_buffer/native_buffer.h>\n"
        "static constexpr VkStructureType kVkStNativeBufferPropsOhos = static_cast<VkStructureType>(1000452001);\n"
        "static constexpr VkStructureType kVkStImportNativeBufferOhos = static_cast<VkStructureType>(1000452003);\n"
        "static constexpr VkStructureType kVkStMemGetNativeBufferOhos = static_cast<VkStructureType>(1000452004);\n"
        "static constexpr VkExternalMemoryHandleTypeFlagBits kVkExtMemHandleOhosNativeBuffer =\n"
        "    static_cast<VkExternalMemoryHandleTypeFlagBits>(0x00008000);\n"
        "#endif",
        "vk_struct_id.h OHOS vulkan_ohos constants",
    )
    patch_file(
        vk_sid,
        "#if defined(VK_USE_PLATFORM_SCREEN_QNX)\n"
        "REGISTER_VK_STRUCT_ID(VkPhysicalDeviceExternalMemoryScreenBufferFeaturesQNX,\n"
        "                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_SCREEN_BUFFER_FEATURES_QNX);\n"
        "REGISTER_VK_STRUCT_ID(VkImportScreenBufferInfoQNX, VK_STRUCTURE_TYPE_IMPORT_SCREEN_BUFFER_INFO_QNX);\n"
        "#endif",
        "#if defined(VK_USE_PLATFORM_SCREEN_QNX)\n"
        "REGISTER_VK_STRUCT_ID(VkPhysicalDeviceExternalMemoryScreenBufferFeaturesQNX,\n"
        "                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_SCREEN_BUFFER_FEATURES_QNX);\n"
        "REGISTER_VK_STRUCT_ID(VkImportScreenBufferInfoQNX, VK_STRUCTURE_TYPE_IMPORT_SCREEN_BUFFER_INFO_QNX);\n"
        "#endif\n"
        "#ifdef __OHOS__\n"
        "REGISTER_VK_STRUCT_ID(VkImportNativeBufferInfoOHOS, kVkStImportNativeBufferOhos);\n"
        "#endif",
        "vk_struct_id.h register VkImportNativeBufferInfoOHOS",
    )

    vkops_h = os.path.join(gfx, "host/vulkan/VkCommonOperations.h")
    patch_file(
        vkops_h,
        "#ifdef __APPLE__\n"
        "        // This is used as an external handle when MoltenVK is enabled\n"
        "        MTLResource_id externalMetalHandle = nullptr;\n"
        "#endif",
        "#ifdef __APPLE__\n"
        "        // This is used as an external handle when MoltenVK is enabled\n"
        "        MTLResource_id externalMetalHandle = nullptr;\n"
        "#endif\n"
        "#ifdef __OHOS__\n"
        "        struct OH_NativeBuffer* externalOhosNativeBuffer = nullptr;\n"
        "#endif",
        "VkCommonOperations.h ExternalMemoryInfo OH_NativeBuffer",
    )
    patch_file(
        vkops_h,
        "#ifdef __APPLE__\n"
        "    MTLResource_id getColorBufferMetalMemoryHandle(uint32_t colorBufferHandle);\n"
        "    VkImage getColorBufferVkImage(uint32_t colorBufferHandle);\n"
        "#endif",
        "#ifdef __APPLE__\n"
        "    MTLResource_id getColorBufferMetalMemoryHandle(uint32_t colorBufferHandle);\n"
        "    VkImage getColorBufferVkImage(uint32_t colorBufferHandle);\n"
        "#endif\n"
        "#ifdef __OHOS__\n"
        "    struct OH_NativeBuffer* getColorBufferOhosNativeBuffer(uint32_t colorBufferHandle);\n"
        "    void setColorBufferGlExported(uint32_t colorBufferHandle);\n"
        "#endif",
        "VkCommonOperations.h getColorBufferOhosNativeBuffer decl",
    )

    patch_file(
        vkops,
        "#include <vulkan/vk_enum_string_helper.h>",
        "#include <vulkan/vk_enum_string_helper.h>\n"
        "#ifdef __OHOS__\n"
        "#include <vulkan/vulkan_ohos.h>\n"
        "#include <native_buffer/native_buffer.h>\n"
        "#endif",
        "VkCommonOperations.cpp OHOS headers",
    )
    patch_file(
        vkops,
        "#if defined(__QNX__)\n"
        "    // TODO(aruby@blackberry.com): Use (DMABUF|OPAQUE_FD) on QNX, when screen_buffer not supported?\n"
        "    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_SCREEN_BUFFER_BIT_QNX;\n"
        "#endif\n\n"
        "    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;",
        "#if defined(__QNX__)\n"
        "    // TODO(aruby@blackberry.com): Use (DMABUF|OPAQUE_FD) on QNX, when screen_buffer not supported?\n"
        "    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_SCREEN_BUFFER_BIT_QNX;\n"
        "#endif\n"
        "#if defined(__OHOS__)\n"
        "    return kVkExtMemHandleOhosNativeBuffer;\n"
        "#endif\n\n"
        "    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;",
        "VkCommonOperations.cpp OHOS default external memory handle type",
    )
    patch_file(
        vkops,
        "#elif defined(__APPLE__)\n"
        "        // VK_EXT_metal_objects will be added if host MoltenVK is enabled,\n"
        "        // otherwise VK_KHR_external_memory_fd will be used\n"
        "#else\n"
        "        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "#endif",
        "#elif defined(__APPLE__)\n"
        "        // VK_EXT_metal_objects will be added if host MoltenVK is enabled,\n"
        "        // otherwise VK_KHR_external_memory_fd will be used\n"
        "#elif defined(__OHOS__)\n"
        "        VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME,\n"
        "        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,\n"
        "        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "#else\n"
        "        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "#endif",
        "VkCommonOperations.cpp OHOS external memory device extensions",
    )
    patch_file(
        vkops,
        "    if (mDeviceInfo.supportsExternalMemoryExport && actuallyExternal) {\n"
        "#ifdef __APPLE__\n"
        "        if (mInstanceSupportsMoltenVK) {\n"
        "            // Change handle type for metal resources\n"
        "            exportAi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;\n"
        "        }\n"
        "#endif\n"
        "        if (mDeviceInfo.supportsDmaBuf) {\n"
        "            exportAi.handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;\n"
        "        }\n\n"
        "        vk_append_struct(&allocInfoChain, &exportAi);\n"
        "    }",
        "    const bool doExternalExport = mDeviceInfo.supportsExternalMemoryExport && actuallyExternal\n"
        "#ifdef __OHOS__\n"
        "        && imageForDedicatedAllocation.hasValue()\n"
        "#endif\n"
        "        ;\n"
        "    if (doExternalExport) {\n"
        "#ifdef __APPLE__\n"
        "        if (mInstanceSupportsMoltenVK) {\n"
        "            // Change handle type for metal resources\n"
        "            exportAi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;\n"
        "        }\n"
        "#endif\n"
        "        if (mDeviceInfo.supportsDmaBuf) {\n"
        "            exportAi.handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;\n"
        "        }\n\n"
        "        vk_append_struct(&allocInfoChain, &exportAi);\n"
        "    }",
        "VkCommonOperations.cpp OHOS export only dedicated VkImage as OH_NativeBuffer",
    )
    patch_file(
        vkops,
        "    if (!mDeviceInfo.supportsExternalMemoryExport || !actuallyExternal) {\n"
        "        return true;\n"
        "    }",
        "    if (!doExternalExport) {\n"
        "        return true;\n"
        "    }",
        "VkCommonOperations.cpp OHOS honor doExternalExport",
    )
    patch_file(
        vkops,
        "    bool opaqueFd = true;\n"
        "#if defined(__APPLE__)\n"
        "    if (mInstanceSupportsMoltenVK) {\n"
        "        opaqueFd = false;\n"
        "        info->externalMetalHandle = getMtlResourceFromVkDeviceMemory(vk, info->memory);\n"
        "        validHandle = (nullptr != info->externalMetalHandle);\n"
        "        if (validHandle) {\n"
        "            CFRetain(info->externalMetalHandle);\n"
        "            exportRes = VK_SUCCESS;\n"
        "        } else {\n"
        "            exportRes = VK_ERROR_INVALID_EXTERNAL_HANDLE;\n"
        "        }\n"
        "    }\n"
        "#endif",
        "    bool opaqueFd = true;\n"
        "#if defined(__APPLE__)\n"
        "    if (mInstanceSupportsMoltenVK) {\n"
        "        opaqueFd = false;\n"
        "        info->externalMetalHandle = getMtlResourceFromVkDeviceMemory(vk, info->memory);\n"
        "        validHandle = (nullptr != info->externalMetalHandle);\n"
        "        if (validHandle) {\n"
        "            CFRetain(info->externalMetalHandle);\n"
        "            exportRes = VK_SUCCESS;\n"
        "        } else {\n"
        "            exportRes = VK_ERROR_INVALID_EXTERNAL_HANDLE;\n"
        "        }\n"
        "    }\n"
        "#elif defined(__OHOS__)\n"
        "    if (imageForDedicatedAllocation.hasValue()) {\n"
        "        opaqueFd = false;\n"
        "        auto fnGetNB = reinterpret_cast<PFN_vkGetMemoryNativeBufferOHOS>(\n"
        "            vk->vkGetDeviceProcAddr(mDevice, \"vkGetMemoryNativeBufferOHOS\"));\n"
        "        if (fnGetNB) {\n"
        "            VkMemoryGetNativeBufferInfoOHOS getNbInfo = {\n"
        "                kVkStMemGetNativeBufferOhos,\n"
        "                nullptr,\n"
        "                info->memory,\n"
        "            };\n"
        "            OH_NativeBuffer* nb = nullptr;\n"
        "            exportRes = fnGetNB(mDevice, &getNbInfo, &nb);\n"
        "            if (exportRes == VK_SUCCESS && nb != nullptr) {\n"
        "                info->externalOhosNativeBuffer = nb;\n"
        "                validHandle = true;\n"
        "                auto fnGetProps = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(\n"
        "                    vk->vkGetDeviceProcAddr(mDevice, \"vkGetNativeBufferPropertiesOHOS\"));\n"
        "                if (fnGetProps) {\n"
        "                    VkNativeBufferPropertiesOHOS nbProps = {kVkStNativeBufferPropsOhos, nullptr, 0, 0};\n"
        "                    if (fnGetProps(mDevice, nb, &nbProps) == VK_SUCCESS && nbProps.allocationSize > 0) {\n"
        "                        info->size = nbProps.allocationSize;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        } else {\n"
        "            exportRes = VK_ERROR_EXTENSION_NOT_PRESENT;\n"
        "        }\n"
        "    }\n"
        "#endif",
        "VkCommonOperations.cpp OHOS vkGetMemoryNativeBufferOHOS export",
    )
    patch_file(
        vkops,
        "#if defined(__APPLE__)\n"
        "    if (info->externalMetalHandle) {\n"
        "        CFRelease(info->externalMetalHandle);\n"
        "    }\n"
        "#endif",
        "#if defined(__APPLE__)\n"
        "    if (info->externalMetalHandle) {\n"
        "        CFRelease(info->externalMetalHandle);\n"
        "    }\n"
        "#elif defined(__OHOS__)\n"
        "    if (info->externalOhosNativeBuffer) {\n"
        "        OH_NativeBuffer_Unreference(info->externalOhosNativeBuffer);\n"
        "        info->externalOhosNativeBuffer = nullptr;\n"
        "    }\n"
        "#endif",
        "VkCommonOperations.cpp OHOS release OH_NativeBuffer in freeExternalMemoryLocked",
    )
    patch_file(
        vkops,
        "#elif defined(__APPLE__)\n"
        "    VkImportMemoryMetalHandleInfoEXT importInfoMetalInfo = {\n"
        "        VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT, dedicatedAllocInfoPtr,\n"
        "        VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT, nullptr};\n"
        "    if (mInstanceSupportsMoltenVK) {\n"
        "        importInfoMetalInfo.handle = info->externalMetalHandle;\n"
        "        importInfoPtr = &importInfoMetalInfo;\n"
        "    }\n"
        "#endif",
        "#elif defined(__APPLE__)\n"
        "    VkImportMemoryMetalHandleInfoEXT importInfoMetalInfo = {\n"
        "        VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT, dedicatedAllocInfoPtr,\n"
        "        VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT, nullptr};\n"
        "    if (mInstanceSupportsMoltenVK) {\n"
        "        importInfoMetalInfo.handle = info->externalMetalHandle;\n"
        "        importInfoPtr = &importInfoMetalInfo;\n"
        "    }\n"
        "#elif defined(__OHOS__)\n"
        "    VkImportNativeBufferInfoOHOS importOhosNb = {\n"
        "        kVkStImportNativeBufferOhos, dedicatedAllocInfoPtr, info->externalOhosNativeBuffer};\n"
        "    VkDeviceSize ohosAllocSize = info->size;\n"
        "    uint32_t ohosTypeIdx = info->typeIndex;\n"
        "    if (info->externalOhosNativeBuffer) {\n"
        "        importInfoPtr = &importOhosNb;\n"
        "        auto fnGetProps = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(\n"
        "            vk->vkGetDeviceProcAddr(targetDevice, \"vkGetNativeBufferPropertiesOHOS\"));\n"
        "        if (fnGetProps) {\n"
        "            VkNativeBufferPropertiesOHOS nbProps = {kVkStNativeBufferPropsOhos, nullptr, 0, 0};\n"
        "            if (fnGetProps(targetDevice, info->externalOhosNativeBuffer, &nbProps) == VK_SUCCESS) {\n"
        "                ohosAllocSize = nbProps.allocationSize;\n"
        "                for (uint32_t i = 0; i < 32; ++i) {\n"
        "                    if (nbProps.memoryTypeBits & (1u << i)) {\n"
        "                        ohosTypeIdx = i;\n"
        "                        break;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "#endif",
        "VkCommonOperations.cpp OHOS importExternalMemory via OH_NativeBuffer",
    )
    patch_file(
        vkops,
        "    VkMemoryAllocateInfo allocInfo = {\n"
        "        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,\n"
        "        importInfoPtr,\n"
        "        info->size,\n"
        "        info->typeIndex,\n"
        "    };",
        "    VkMemoryAllocateInfo allocInfo = {\n"
        "        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,\n"
        "        importInfoPtr,\n"
        "#ifdef __OHOS__\n"
        "        ohosAllocSize,\n"
        "        ohosTypeIdx,\n"
        "#else\n"
        "        info->size,\n"
        "        info->typeIndex,\n"
        "#endif\n"
        "    };",
        "VkCommonOperations.cpp OHOS use queried NB size and typeIndex in importExternalMemory",
    )
    patch_file(
        vkops,
        "    Optional<VkImage> dedicatedImage = useDedicated ? Optional<VkImage>(infoPtr->image) : kNullopt;",
        "#ifdef __OHOS__\n"
        "    if (mDeviceInfo.supportsExternalMemoryExport) {\n"
        "        useDedicated = true;\n"
        "    }\n"
        "#endif\n"
        "    Optional<VkImage> dedicatedImage = useDedicated ? Optional<VkImage>(infoPtr->image) : kNullopt;",
        "VkCommonOperations.cpp OHOS force dedicatedImage for OH_NativeBuffer ColorBuffers",
    )
    patch_file(
        vkops,
        "#endif  // __APPLE__\n\n"
        "bool VkEmulation::setColorBufferVulkanMode",
        "#endif  // __APPLE__\n\n"
        "#ifdef __OHOS__\n"
        "struct OH_NativeBuffer* VkEmulation::getColorBufferOhosNativeBuffer(uint32_t colorBuffer) {\n"
        "    std::lock_guard<std::mutex> lock(mMutex);\n"
        "    auto infoPtr = android::base::find(mColorBuffers, colorBuffer);\n"
        "    if (!infoPtr) return nullptr;\n"
        "    return infoPtr->memory.externalOhosNativeBuffer;\n"
        "}\n\n"
        "void VkEmulation::setColorBufferGlExported(uint32_t colorBuffer) {\n"
        "    std::lock_guard<std::mutex> lock(mMutex);\n"
        "    auto infoPtr = android::base::find(mColorBuffers, colorBuffer);\n"
        "    if (infoPtr) {\n"
        "        infoPtr->glExported = true;\n"
        "    }\n"
        "}\n"
        "#endif  // __OHOS__\n\n"
        "bool VkEmulation::setColorBufferVulkanMode",
        "VkCommonOperations.cpp define getColorBufferOhosNativeBuffer",
    )

    # 12. VkDecoderGlobalState.cpp: enable VK_OHOS_external_memory on guest VkDevices,
    #     import OH_NativeBuffer without double-remapping host memoryTypeIndex, and allow re-init.
    patch_file(
        vkdgs,
        "void VkDecoderGlobalState::initialize(VkEmulation* emulation) {\n"
        "    if (sGlobalDecoderState) {\n"
        "        GFXSTREAM_ABORT(FatalError(ABORT_REASON_OTHER))\n"
        "            << \"Attempted to re-initialize VkDecoderGlobalState.\";\n"
        "    }\n"
        "    sGlobalDecoderState = new VkDecoderGlobalState(emulation);\n"
        "}",
        "void VkDecoderGlobalState::initialize(VkEmulation* emulation) {\n"
        "    if (sGlobalDecoderState) {\n"
        "        reset();\n"
        "    }\n"
        "    sGlobalDecoderState = new VkDecoderGlobalState(emulation);\n"
        "}",
        "VkDecoderGlobalState allow clean re-initialize on live GPU mode switch",
    )
    patch_file(
        vkdgs,
        "#elif __unix__\n"
        "            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,\n"
        "#endif",
        "#elif defined(__OHOS__)\n"
        "            VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME,\n"
        "            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,\n"
        "#elif __unix__\n"
        "            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,\n"
        "            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,\n"
        "#endif",
        "VkDecoderGlobalState enable VK_OHOS_external_memory on guest VkDevice",
    )
    patch_file(
        vkdgs,
        "#elif defined(__APPLE__)\n"
        "        VkImportMemoryMetalHandleInfoEXT importInfoMetalHandle = {\n"
        "            VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,\n"
        "            0,\n"
        "            VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT,\n"
        "            nullptr,\n"
        "        };\n"
        "#endif",
        "#elif defined(__APPLE__)\n"
        "        VkImportMemoryMetalHandleInfoEXT importInfoMetalHandle = {\n"
        "            VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,\n"
        "            0,\n"
        "            VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT,\n"
        "            nullptr,\n"
        "        };\n"
        "#elif defined(__OHOS__)\n"
        "        VkImportNativeBufferInfoOHOS importOhosNbInfo = {\n"
        "            kVkStImportNativeBufferOhos,\n"
        "            nullptr,\n"
        "            nullptr,\n"
        "        };\n"
        "        bool ohosImportedNb = false;\n"
        "        uint32_t ohosHostMemTypeIdx = 0;\n"
        "#endif",
        "VkDecoderGlobalState declare importOhosNbInfo",
    )
    patch_file(
        vkdgs,
        "                if (opaqueFd && m_vkEmulation->supportsExternalMemoryImport()) {",
        "#if defined(__OHOS__)\n"
        "                {\n"
        "                    OH_NativeBuffer* nb =\n"
        "                        m_vkEmulation->getColorBufferOhosNativeBuffer(importCbInfoPtr->colorBuffer);\n"
        "                    if (!nb) {\n"
        "                        ERR(\"OHOS ColorBuffer %u has no OH_NativeBuffer backing\",\n"
        "                            importCbInfoPtr->colorBuffer);\n"
        "                        return VK_ERROR_OUT_OF_DEVICE_MEMORY;\n"
        "                    }\n"
        "                    auto fnGetNBProps = reinterpret_cast<PFN_vkGetNativeBufferPropertiesOHOS>(\n"
        "                        vk->vkGetDeviceProcAddr(device, \"vkGetNativeBufferPropertiesOHOS\"));\n"
        "                    if (fnGetNBProps) {\n"
        "                        VkNativeBufferPropertiesOHOS nbProps = {kVkStNativeBufferPropsOhos, nullptr, 0, 0};\n"
        "                        if (fnGetNBProps(device, nb, &nbProps) == VK_SUCCESS) {\n"
        "                            localAllocInfo.allocationSize = nbProps.allocationSize;\n"
        "                            for (uint32_t i = 0; i < 32; ++i) {\n"
        "                                if (nbProps.memoryTypeBits & (1u << i)) {\n"
        "                                    ohosHostMemTypeIdx = i;\n"
        "                                    ohosImportedNb = true;\n"
        "                                    break;\n"
        "                                }\n"
        "                            }\n"
        "                        }\n"
        "                    }\n"
        "                    importOhosNbInfo.buffer = nb;\n"
        "                    vk_append_struct(&structChainIter, &importOhosNbInfo);\n"
        "                    opaqueFd = false;\n"
        "                }\n"
        "#endif\n"
        "                if (opaqueFd && m_vkEmulation->supportsExternalMemoryImport()) {",
        "VkDecoderGlobalState import OH_NativeBuffer in on_vkAllocateMemory",
    )
    patch_file(
        vkdgs,
        "            const auto hostMemoryInfoOpt =\n"
        "                physicalDeviceInfo->memoryPropertiesHelper\n"
        "                    ->getHostMemoryInfoFromGuestMemoryTypeIndex(localAllocInfo.memoryTypeIndex);\n"
        "            if (!hostMemoryInfoOpt) {\n"
        "                return VK_ERROR_INCOMPATIBLE_DRIVER;\n"
        "            }\n"
        "            const auto& hostMemoryInfo = *hostMemoryInfoOpt;\n\n"
        "            localAllocInfo.memoryTypeIndex = hostMemoryInfo.index;\n"
        "            memoryPropertyFlags = hostMemoryInfo.memoryType.propertyFlags;",
        "#if defined(__OHOS__)\n"
        "            if (ohosImportedNb) {\n"
        "                localAllocInfo.memoryTypeIndex = ohosHostMemTypeIdx;\n"
        "                memoryPropertyFlags = 0;\n"
        "            } else\n"
        "#endif\n"
        "            {\n"
        "                const auto hostMemoryInfoOpt =\n"
        "                    physicalDeviceInfo->memoryPropertiesHelper\n"
        "                        ->getHostMemoryInfoFromGuestMemoryTypeIndex(localAllocInfo.memoryTypeIndex);\n"
        "                if (!hostMemoryInfoOpt) {\n"
        "                    return VK_ERROR_INCOMPATIBLE_DRIVER;\n"
        "                }\n"
        "                const auto& hostMemoryInfo = *hostMemoryInfoOpt;\n\n"
        "                localAllocInfo.memoryTypeIndex = hostMemoryInfo.index;\n"
        "                memoryPropertyFlags = hostMemoryInfo.memoryType.propertyFlags;\n"
        "            }",
        "VkDecoderGlobalState preserve OH_NativeBuffer host memoryTypeIndex",
    )
    patch_file(
        vkdgs,
        "        VkResult result = vk->vkAllocateMemory(device, &localAllocInfo, pAllocator, pMemory);\n"
        "        if (result != VK_SUCCESS) {\n"
        "            return result;\n"
        "        }",
        "        VkResult result = vk->vkAllocateMemory(device, &localAllocInfo, pAllocator, pMemory);\n"
        "        if (result != VK_SUCCESS) {\n"
        "            if (importCbInfoPtr) {\n"
        "                ERR(\"vkAllocateMemory failed for ColorBuffer:%u res=%d size=%llu typeIdx=%u dedicated=%d\",\n"
        "                    importCbInfoPtr->colorBuffer, (int)result,\n"
        "                    (unsigned long long)localAllocInfo.allocationSize,\n"
        "                    localAllocInfo.memoryTypeIndex, (int)shouldUseDedicatedAllocInfo);\n"
        "            }\n"
        "            return result;\n"
        "        }",
        "VkDecoderGlobalState log ColorBuffer vkAllocateMemory failure",
    )

    # 13. ColorBufferGl + ColorBuffer: zero-copy OH_NativeBuffer -> EGL_NATIVE_BUFFER_OHOS GL interop
    cbgl_h = os.path.join(gfx, "host/gl/ColorBufferGl.h")
    patch_file(
        cbgl_h,
        "    // Change to EGL native pixmap\n"
        "    bool importEglNativePixmap(void* pixmap, bool preserveContent);",
        "    // Change to EGL native pixmap\n"
        "    bool importEglNativePixmap(void* pixmap, bool preserveContent);\n"
        "#ifdef __OHOS__\n"
        "    // Zero-copy import of OH_NativeBuffer via EGL_NATIVE_BUFFER_OHOS\n"
        "    bool importOhosNativeBuffer(void* ohNativeBuffer);\n"
        "#endif",
        "ColorBufferGl.h declare importOhosNativeBuffer",
    )
    patch_file(
        cbgl_h,
        "    uint32_t m_displayId = 0;\n"
        "    bool m_BRSwizzle = false;\n"
        "};",
        "    uint32_t m_displayId = 0;\n"
        "    bool m_BRSwizzle = false;\n"
        "#ifdef __OHOS__\n"
        "    void* m_ohosNativeWindowBuffer = nullptr;\n"
        "#endif\n"
        "};",
        "ColorBufferGl.h add m_ohosNativeWindowBuffer member",
    )

    cbgl_cpp = os.path.join(gfx, "host/gl/ColorBufferGl.cpp")
    patch_file(
        cbgl_cpp,
        "#include <GLES2/gl2ext.h>",
        "#include <GLES2/gl2ext.h>\n"
        "#ifdef __OHOS__\n"
        "#include <native_buffer/native_buffer.h>\n"
        "#include <native_window/external_window.h>\n"
        "#ifndef EGL_NATIVE_BUFFER_OHOS\n"
        "#define EGL_NATIVE_BUFFER_OHOS 0x34E1\n"
        "#endif\n"
        "#endif",
        "ColorBufferGl.cpp OHOS headers",
    )
    patch_file(
        cbgl_cpp,
        "    if (m_eglImage) {\n"
        "        s_egl.eglDestroyImageKHR(m_display, m_eglImage);\n"
        "    }",
        "    if (m_eglImage) {\n"
        "        s_egl.eglDestroyImageKHR(m_display, m_eglImage);\n"
        "    }\n"
        "#ifdef __OHOS__\n"
        "    if (m_ohosNativeWindowBuffer) {\n"
        "        OH_NativeWindow_DestroyNativeWindowBuffer(\n"
        "            static_cast<OHNativeWindowBuffer*>(m_ohosNativeWindowBuffer));\n"
        "        m_ohosNativeWindowBuffer = nullptr;\n"
        "    }\n"
        "#endif",
        "ColorBufferGl.cpp destroy m_ohosNativeWindowBuffer in destructor",
    )
    patch_file(
        cbgl_cpp,
        "std::unique_ptr<BorrowedImageInfo> ColorBufferGl::getBorrowedImageInfo() {",
        "#ifdef __OHOS__\n"
        "bool ColorBufferGl::importOhosNativeBuffer(void* ohNativeBuffer) {\n"
        "    if (!ohNativeBuffer) return false;\n"
        "    OHNativeWindowBuffer* nwb = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(\n"
        "        static_cast<OH_NativeBuffer*>(ohNativeBuffer));\n"
        "    if (!nwb) {\n"
        "        ERR(\"ColorBufferGl::importOhosNativeBuffer: CreateNativeWindowBuffer failed\");\n"
        "        return false;\n"
        "    }\n"
        "    const EGLint imgAttribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};\n"
        "    EGLImageKHR image = s_egl.eglCreateImageKHR(\n"
        "        m_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS,\n"
        "        reinterpret_cast<EGLClientBuffer>(nwb), imgAttribs);\n"
        "    if (image == EGL_NO_IMAGE_KHR) {\n"
        "        ERR(\"ColorBufferGl::importOhosNativeBuffer: eglCreateImageKHR failed err=0x%x\",\n"
        "            s_egl.eglGetError());\n"
        "        OH_NativeWindow_DestroyNativeWindowBuffer(nwb);\n"
        "        return false;\n"
        "    }\n"
        "    if (s_egl.eglSetImageInfoANDROID(m_display, image, m_width, m_height, m_internalFormat) != EGL_TRUE) {\n"
        "        ERR(\"ColorBufferGl::importOhosNativeBuffer: eglSetImageInfoANDROID failed\");\n"
        "        s_egl.eglDestroyImageKHR(m_display, image);\n"
        "        OH_NativeWindow_DestroyNativeWindowBuffer(nwb);\n"
        "        return false;\n"
        "    }\n"
        "    RecursiveScopedContextBind context(m_helper);\n"
        "    if (m_eglImage) {\n"
        "        s_egl.eglDestroyImageKHR(m_display, m_eglImage);\n"
        "    }\n"
        "    if (m_ohosNativeWindowBuffer) {\n"
        "        OH_NativeWindow_DestroyNativeWindowBuffer(\n"
        "            static_cast<OHNativeWindowBuffer*>(m_ohosNativeWindowBuffer));\n"
        "    }\n"
        "    m_eglImage = image;\n"
        "    m_ohosNativeWindowBuffer = nwb;\n"
        "    m_importedMemory = true;\n"
        "    m_needFboReattach = true;\n"
        "    s_gles2.glBindTexture(GL_TEXTURE_2D, m_tex);\n"
        "    s_gles2.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)m_eglImage);\n"
        "    return true;\n"
        "}\n"
        "#endif\n\n"
        "std::unique_ptr<BorrowedImageInfo> ColorBufferGl::getBorrowedImageInfo() {",
        "ColorBufferGl.cpp implement importOhosNativeBuffer",
    )

    cb_cpp = os.path.join(gfx, "host/ColorBuffer.cpp")
    patch_file(
        cb_cpp,
        "    if ((!stream || vkSnapshotEnabled) && colorBuffer->mColorBufferGl && colorBuffer->mColorBufferVk &&\n"
        "        shouldAttemptExternalMemorySharing(frameworkFormat)) {\n"
        "        colorBuffer->touch();\n"
        "        auto memoryExport = emulationVk->exportColorBufferMemory(handle);",
        "    if ((!stream || vkSnapshotEnabled) && colorBuffer->mColorBufferGl && colorBuffer->mColorBufferVk &&\n"
        "        shouldAttemptExternalMemorySharing(frameworkFormat)) {\n"
        "        colorBuffer->touch();\n"
        "#ifdef __OHOS__\n"
        "        if (auto* nb = emulationVk->getColorBufferOhosNativeBuffer(handle)) {\n"
        "            if (colorBuffer->mColorBufferGl->importOhosNativeBuffer(nb)) {\n"
        "                emulationVk->setColorBufferGlExported(handle);\n"
        "                colorBuffer->mGlAndVkAreSharingExternalMemory = true;\n"
        "            } else {\n"
        "                ERR(\"Failed to import OH_NativeBuffer to ColorBufferGl:%d\", handle);\n"
        "            }\n"
        "        }\n"
        "#else\n"
        "        auto memoryExport = emulationVk->exportColorBufferMemory(handle);",
        "ColorBuffer.cpp OHOS zero-copy OH_NativeBuffer GL-Vk interop start",
    )
    patch_file(
        cb_cpp,
        "            } else {\n"
        "                ERR(\"Failed to import memory to ColorBufferGl:%d\", handle);\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "#endif",
        "            } else {\n"
        "                ERR(\"Failed to import memory to ColorBufferGl:%d\", handle);\n"
        "            }\n"
        "        }\n"
        "#endif\n"
        "    }\n"
        "#endif",
        "ColorBuffer.cpp OHOS zero-copy OH_NativeBuffer GL-Vk interop end",
    )

    # 14. VkAndroidNativeBuffer.cpp: sync QSRI fence before returning -1 fence fd on OHOS
    #     (no kernel goldfish_sync), plus log ANB vkCreateImage errors.
    vkanb = os.path.join(gfx, "host/vulkan/VkAndroidNativeBuffer.cpp")
    patch_file(
        vkanb,
        "        VkResult createResult =\n"
        "            vk->vkCreateImage(out->mDevice, &createImageCi, pAllocator, &out->mImage);\n"
        "        if (createResult != VK_SUCCESS) {\n"
        "            return nullptr;\n"
        "        }",
        "        VkResult createResult =\n"
        "            vk->vkCreateImage(out->mDevice, &createImageCi, pAllocator, &out->mImage);\n"
        "        if (createResult != VK_SUCCESS) {\n"
        "            VK_ANB_ERR(\"VK_ANDROID_native_buffer: vkCreateImage failed res=%d fmt=%d usage=0x%x flags=0x%x ext=0x%x\",\n"
        "                       (int)createResult, (int)createImageCi.format, (unsigned)createImageCi.usage,\n"
        "                       (unsigned)createImageCi.flags, (unsigned)extImageCi.handleTypes);\n"
        "            return nullptr;\n"
        "        }",
        "VkAndroidNativeBuffer log vkCreateImage failure",
    )
    patch_file(
        vkanb,
        "    if (mUseVulkanNativeImage) {\n"
        "        VK_ANB_DEBUG_OBJ(this, \"using native image, so use sync thread to wait\");\n"
        "        // Queue wait to sync thread with completion callback\n"
        "        // Pass anbInfo by value to get a ref\n"
        "        auto waitable = emu->getCallbacks().scheduleAsyncWork(\n"
        "            [waitForQsriFenceTask = std::move(waitForQsriFenceTask), this]() mutable {\n"
        "                waitForQsriFenceTask();\n"
        "                mQsriTimeline->signalNextPresentAndPoll();\n"
        "            },\n"
        "            \"wait for the guest Qsri VkFence signaled\");\n\n"
        "        queueState.latestUse = std::move(waitable);\n"
        "    } else {",
        "    if (mUseVulkanNativeImage) {\n"
        "#ifdef __OHOS__\n"
        "        // Without kernel sync_fd (/dev/goldfish_sync), *pNativeFenceFd == -1 tells\n"
        "        // SurfaceFlinger the buffer is already ready; wait on qsriFence here so GLES\n"
        "        // RenderEngine never samples an incomplete OH_NativeBuffer.\n"
        "        waitForQsriFenceTask();\n"
        "        mQsriTimeline->signalNextPresentAndPoll();\n"
        "#else\n"
        "        VK_ANB_DEBUG_OBJ(this, \"using native image, so use sync thread to wait\");\n"
        "        // Queue wait to sync thread with completion callback\n"
        "        // Pass anbInfo by value to get a ref\n"
        "        auto waitable = emu->getCallbacks().scheduleAsyncWork(\n"
        "            [waitForQsriFenceTask = std::move(waitForQsriFenceTask), this]() mutable {\n"
        "                waitForQsriFenceTask();\n"
        "                mQsriTimeline->signalNextPresentAndPoll();\n"
        "            },\n"
        "            \"wait for the guest Qsri VkFence signaled\");\n\n"
        "        queueState.latestUse = std::move(waitable);\n"
        "#endif\n"
        "    } else {",
        "VkAndroidNativeBuffer OHOS sync wait on qsriFence for native image",
    )

    # 15. VkEmulatedPhysicalDeviceMemory.cpp: preserve VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    #     on __OHOS__ (required by Vulkan 1.0 spec & Android 11 SkiaVK GrVkTransferBuffer /
    #     HardwareBitmapUploader). Our guest ResourceTracker emulates coherency over AF_UNIX
    #     by flushing mapped coherent shadow buffers on vkUnmapMemory & vkQueueSubmit.
    vkmem = os.path.join(gfx, "host/vulkan/VkEmulatedPhysicalDeviceMemory.cpp")
    patch_file(
        vkmem,
        "    // Coherent memory in the guest requires one of these features:\n"
        "    if (!features.GlDirectMem.enabled && !features.VirtioGpuNext.enabled) {",
        "    // Coherent memory in the guest requires one of these features (or OHOS guest shadow flush):\n"
        "#ifndef __OHOS__\n"
        "    if (!features.GlDirectMem.enabled && !features.VirtioGpuNext.enabled) {",
        "VkEmulatedPhysicalDeviceMemory OHOS preserve HOST_COHERENT start",
    )
    patch_file(
        vkmem,
        "            mGuestMemoryProperties.memoryTypes[i].propertyFlags =\n"
        "                mGuestMemoryProperties.memoryTypes[i].propertyFlags &\n"
        "                ~(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);\n"
        "        }\n"
        "    }",
        "            mGuestMemoryProperties.memoryTypes[i].propertyFlags =\n"
        "                mGuestMemoryProperties.memoryTypes[i].propertyFlags &\n"
        "                ~(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);\n"
        "        }\n"
        "    }\n"
        "#endif",
        "VkEmulatedPhysicalDeviceMemory OHOS preserve HOST_COHERENT end",
    )

    # Secondary DisplayVk factory for multi-window zero-copy Vulkan presentation.
    vkops_h = os.path.join(gfx, "host/vulkan/VkCommonOperations.h")
    patch_file(
        vkops_h,
        "    DisplayVk* getDisplay();\n",
        "    DisplayVk* getDisplay();\n"
        "    std::unique_ptr<DisplayVk> createSecondaryDisplay();\n",
        "VkEmulation::createSecondaryDisplay header",
    )
    vkops_cpp = os.path.join(gfx, "host/vulkan/VkCommonOperations.cpp")
    patch_file(
        vkops_cpp,
        "DisplayVk* VkEmulation::getDisplay() { return mDisplayVk.get(); }\n",
        "DisplayVk* VkEmulation::getDisplay() { return mDisplayVk.get(); }\n\n"
        "std::unique_ptr<DisplayVk> VkEmulation::createSecondaryDisplay() {\n"
        "    if (!mIvk || !mPhysicalDevice || !mDevice || !mQueue || !mQueueLock) {\n"
        "        return nullptr;\n"
        "    }\n"
        "    return std::make_unique<DisplayVk>(*mIvk, mPhysicalDevice, mQueueFamilyIndex,\n"
        "                                       mQueueFamilyIndex, mDevice, mQueue, mQueueLock,\n"
        "                                       mQueue, mQueueLock);\n"
        "}\n",
        "VkEmulation::createSecondaryDisplay impl",
    )
    patch_file(
        vkops_cpp,
        "    if (x != 0 || y != 0 || w != colorBufferInfo->imageCreateInfoShallow.extent.width ||\n"
        "        h != colorBufferInfo->imageCreateInfoShallow.extent.height) {\n"
        "        ERR(\"Failed to read from ColorBuffer:%d, unhandled subrect.\", colorBufferHandle);\n"
        "        return false;\n"
        "    }\n\n"
        "    VkDeviceSize bufferCopySize = 0;\n"
        "    std::vector<VkBufferImageCopy> bufferImageCopies;\n"
        "    if (!getFormatTransferInfo(colorBufferInfo->imageCreateInfoShallow.format,\n"
        "                               colorBufferInfo->imageCreateInfoShallow.extent.width,\n"
        "                               colorBufferInfo->imageCreateInfoShallow.extent.height,\n"
        "                               &bufferCopySize, &bufferImageCopies)) {\n"
        "        ERR(\"Failed to read ColorBuffer:%d, unable to get transfer info.\", colorBufferHandle);\n"
        "        return false;\n"
        "    }\n",
        "    const uint32_t extW = colorBufferInfo->imageCreateInfoShallow.extent.width;\n"
        "    const uint32_t extH = colorBufferInfo->imageCreateInfoShallow.extent.height;\n"
        "    const bool isSubrect = (x != 0 || y != 0 || w != extW || h != extH);\n"
        "    if (w == 0 || h == 0 || x + w > extW || y + h > extH) {\n"
        "        ERR(\"Failed to read from ColorBuffer:%d, out-of-bounds subrect.\", colorBufferHandle);\n"
        "        return false;\n"
        "    }\n\n"
        "    VkDeviceSize bufferCopySize = 0;\n"
        "    std::vector<VkBufferImageCopy> bufferImageCopies;\n"
        "    if (!getFormatTransferInfo(colorBufferInfo->imageCreateInfoShallow.format, w, h,\n"
        "                               &bufferCopySize, &bufferImageCopies)) {\n"
        "        ERR(\"Failed to read ColorBuffer:%d, unable to get transfer info.\", colorBufferHandle);\n"
        "        return false;\n"
        "    }\n"
        "    if (isSubrect) {\n"
        "        if (bufferImageCopies.size() != 1) {\n"
        "            ERR(\"Failed to read from ColorBuffer:%d, unhandled multi-plane subrect.\", colorBufferHandle);\n"
        "            return false;\n"
        "        }\n"
        "        bufferImageCopies[0].imageOffset.x = static_cast<int32_t>(x);\n"
        "        bufferImageCopies[0].imageOffset.y = static_cast<int32_t>(y);\n"
        "    }\n",
        "VkEmulation::readColorBufferToBytesLocked single-plane subrect support",
    )


if __name__ == "__main__":
    main()
