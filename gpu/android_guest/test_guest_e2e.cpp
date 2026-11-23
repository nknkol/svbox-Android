/* SPDX-License-Identifier: MIT */
/*
 * test_guest_e2e.cpp - Android 11 guest end-to-end test for svbox gfxstream.
 *
 * Tests:
 *   mode "gles":   libEGL_emulation.so + libGLESv2_emulation.so + gralloc.ranchu.so + hwcomposer.ranchu.so
 *   mode "vulkan": vulkan.ranchu.so (hwvulkan HAL -> VkInstance -> Maleoon 916 -> VkDevice -> host-visible buffer)
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>
#include <hardware/hwvulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static int test_gles_and_hals() {
    printf("[E2E-GLES] Loading libEGL_emulation.so & libGLESv2_emulation.so...\n");
    void* hEgl = dlopen("/vendor/lib64/egl/libEGL_emulation.so", RTLD_NOW | RTLD_LOCAL);
    if (!hEgl) {
        fprintf(stderr, "[FAIL] dlopen libEGL_emulation.so: %s\n", dlerror());
        return 1;
    }
    void* hGl = dlopen("/vendor/lib64/egl/libGLESv2_emulation.so", RTLD_NOW | RTLD_LOCAL);
    if (!hGl) {
        fprintf(stderr, "[FAIL] dlopen libGLESv2_emulation.so: %s\n", dlerror());
        return 1;
    }

    auto pEglGetDisplay = reinterpret_cast<EGLDisplay (*)(EGLNativeDisplayType)>(dlsym(hEgl, "eglGetDisplay"));
    auto pEglInitialize = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(dlsym(hEgl, "eglInitialize"));
    auto pEglChooseConfig = reinterpret_cast<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(dlsym(hEgl, "eglChooseConfig"));
    auto pEglCreatePbufferSurface = reinterpret_cast<EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*)>(dlsym(hEgl, "eglCreatePbufferSurface"));
    auto pEglCreateContext = reinterpret_cast<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(dlsym(hEgl, "eglCreateContext"));
    auto pEglMakeCurrent = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(dlsym(hEgl, "eglMakeCurrent"));
    auto pEglDestroyContext = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLContext)>(dlsym(hEgl, "eglDestroyContext"));
    auto pEglDestroySurface = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface)>(dlsym(hEgl, "eglDestroySurface"));
    auto pEglTerminate = reinterpret_cast<EGLBoolean (*)(EGLDisplay)>(dlsym(hEgl, "eglTerminate"));
    auto pEglGetError = reinterpret_cast<EGLint (*)()>(dlsym(hEgl, "eglGetError"));

    auto pGlGetString = reinterpret_cast<const GLubyte* (*)(GLenum)>(dlsym(hGl, "glGetString"));
    auto pGlClearColor = reinterpret_cast<void (*)(GLfloat, GLfloat, GLfloat, GLfloat)>(dlsym(hGl, "glClearColor"));
    auto pGlClear = reinterpret_cast<void (*)(GLbitfield)>(dlsym(hGl, "glClear"));
    auto pGlFinish = reinterpret_cast<void (*)()>(dlsym(hGl, "glFinish"));
    auto pGlReadPixels = reinterpret_cast<void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)>(dlsym(hGl, "glReadPixels"));

    EGLDisplay dpy = pEglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!pEglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "[FAIL] eglInitialize err=0x%x\n", pEglGetError());
        return 2;
    }
    printf("[E2E-GLES] eglInitialize OK: EGL %d.%d\n", major, minor);

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint numCfg = 0;
    if (!pEglChooseConfig(dpy, cfgAttribs, &cfg, 1, &numCfg) || numCfg < 1) {
        fprintf(stderr, "[FAIL] eglChooseConfig err=0x%x numCfg=%d\n", pEglGetError(), numCfg);
        return 3;
    }

    const EGLint pbAttribs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = pEglCreatePbufferSurface(dpy, cfg, pbAttribs);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "[FAIL] eglCreatePbufferSurface err=0x%x\n", pEglGetError());
        return 4;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = pEglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[FAIL] eglCreateContext(GLES3) err=0x%x\n", pEglGetError());
        return 5;
    }

    if (!pEglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "[FAIL] eglMakeCurrent err=0x%x\n", pEglGetError());
        return 6;
    }

    printf("[E2E-GLES] GL_VENDOR   = %s\n", reinterpret_cast<const char*>(pGlGetString(GL_VENDOR)));
    printf("[E2E-GLES] GL_RENDERER = %s\n", reinterpret_cast<const char*>(pGlGetString(GL_RENDERER)));
    printf("[E2E-GLES] GL_VERSION  = %s\n", reinterpret_cast<const char*>(pGlGetString(GL_VERSION)));

    pGlClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    pGlClear(GL_COLOR_BUFFER_BIT);
    pGlFinish();

    uint8_t px[4] = {0, 0, 0, 0};
    pGlReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("[E2E-GLES] ReadPixels(32,32) = (%u, %u, %u, %u) (expected ~64, 128, 191, 255)\n",
           px[0], px[1], px[2], px[3]);
    if (abs(int(px[0]) - 64) > 2 || abs(int(px[1]) - 128) > 2 ||
        abs(int(px[2]) - 191) > 2 || px[3] != 255) {
        fprintf(stderr, "[FAIL] Pixel mismatch!\n");
        return 7;
    }

    pEglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    pEglDestroyContext(dpy, ctx);
    pEglDestroySurface(dpy, surf);
    pEglTerminate(dpy);
    printf("[E2E-GLES] EGL/GLES3 render + readback PASS!\n");

    // Test gralloc.ranchu.so + hwcomposer.ranchu.so
    printf("[E2E-HAL] Testing gralloc.ranchu.so + hwcomposer.ranchu.so...\n");
    void* hGralloc = dlopen("/vendor/lib64/hw/gralloc.ranchu.so", RTLD_NOW | RTLD_LOCAL);
    if (!hGralloc) {
        fprintf(stderr, "[FAIL] dlopen gralloc.ranchu.so: %s\n", dlerror());
        return 8;
    }
    auto* gMod = reinterpret_cast<hw_module_t*>(dlsym(hGralloc, HAL_MODULE_INFO_SYM_AS_STR));
    if (!gMod || !gMod->methods || !gMod->methods->open) {
        fprintf(stderr, "[FAIL] gralloc HAL_MODULE_INFO_SYM invalid\n");
        return 9;
    }
    alloc_device_t* allocDev = nullptr;
    int rc = gMod->methods->open(gMod, GRALLOC_HARDWARE_GPU0, reinterpret_cast<hw_device_t**>(&allocDev));
    if (rc != 0 || !allocDev) {
        fprintf(stderr, "[FAIL] gralloc open rc=%d\n", rc);
        return 10;
    }
    buffer_handle_t buf = nullptr;
    int stride = 0;
    rc = allocDev->alloc(allocDev, 64, 64, HAL_PIXEL_FORMAT_RGBA_8888,
                         GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER,
                         &buf, &stride);
    if (rc != 0 || !buf) {
        fprintf(stderr, "[FAIL] gralloc alloc rc=%d\n", rc);
        return 11;
    }
    printf("[E2E-HAL] gralloc alloc(64x64 RGBA8888) OK! stride=%d numFds=%d numInts=%d\n",
           stride, buf->numFds, buf->numInts);

    void* hHwc = dlopen("/vendor/lib64/hw/hwcomposer.ranchu.so", RTLD_NOW | RTLD_LOCAL);
    if (!hHwc) {
        fprintf(stderr, "[FAIL] dlopen hwcomposer.ranchu.so: %s\n", dlerror());
        return 12;
    }
    auto* hMod = reinterpret_cast<hw_module_t*>(dlsym(hHwc, HAL_MODULE_INFO_SYM_AS_STR));
    hwc2_device_t* hwcDev = nullptr;
    rc = hMod->methods->open(hMod, HWC_HARDWARE_COMPOSER, reinterpret_cast<hw_device_t**>(&hwcDev));
    if (rc != 0 || !hwcDev) {
        fprintf(stderr, "[FAIL] hwcomposer open rc=%d\n", rc);
        return 13;
    }
    auto fnSetClientTarget = reinterpret_cast<HWC2_PFN_SET_CLIENT_TARGET>(
        hwcDev->getFunction(hwcDev, HWC2_FUNCTION_SET_CLIENT_TARGET));
    auto fnValidate = reinterpret_cast<HWC2_PFN_VALIDATE_DISPLAY>(
        hwcDev->getFunction(hwcDev, HWC2_FUNCTION_VALIDATE_DISPLAY));
    auto fnPresent = reinterpret_cast<HWC2_PFN_PRESENT_DISPLAY>(
        hwcDev->getFunction(hwcDev, HWC2_FUNCTION_PRESENT_DISPLAY));

    hwc_region_t dmg{0, nullptr};
    fnSetClientTarget(hwcDev, 0, buf, -1, 0, dmg);
    uint32_t nt = 0, nr = 0;
    fnValidate(hwcDev, 0, &nt, &nr);
    int32_t pf = -1;
    int32_t pr = fnPresent(hwcDev, 0, &pf);
    printf("[E2E-HAL] hwcomposer presentDisplay rc=%d fence=%d\n", pr, pf);
    if (pr != HWC2_ERROR_NONE) {
        fprintf(stderr, "[FAIL] presentDisplay failed: %d\n", pr);
        return 14;
    }

    hwcDev->common.close(&hwcDev->common);
    allocDev->free(allocDev, buf);
    allocDev->common.close(&allocDev->common);
    printf("[E2E-HAL] gralloc + hwcomposer PASS!\n");
    return 0;
}

static int test_vulkan() {
    printf("[E2E-VK] Loading vulkan.ranchu.so...\n");
    void* hVk = dlopen("/vendor/lib64/hw/vulkan.ranchu.so", RTLD_NOW | RTLD_LOCAL);
    if (!hVk) {
        fprintf(stderr, "[FAIL] dlopen vulkan.ranchu.so: %s\n", dlerror());
        return 20;
    }
    auto* vMod = reinterpret_cast<hw_module_t*>(dlsym(hVk, HAL_MODULE_INFO_SYM_AS_STR));
    if (!vMod || !vMod->methods || !vMod->methods->open) {
        fprintf(stderr, "[FAIL] vulkan.ranchu.so HAL_MODULE_INFO_SYM invalid\n");
        return 21;
    }
    hwvulkan_device_t* vkDev = nullptr;
    int rc = vMod->methods->open(vMod, HWVULKAN_DEVICE_0, reinterpret_cast<hw_device_t**>(&vkDev));
    if (rc != 0 || !vkDev) {
        fprintf(stderr, "[FAIL] hwvulkan open rc=%d\n", rc);
        return 22;
    }

    auto pCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vkDev->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!pCreateInstance) {
        fprintf(stderr, "[FAIL] GetInstanceProcAddr(vkCreateInstance) null\n");
        return 23;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "svbox_vk_e2e";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult vr = pCreateInstance(&ci, nullptr, &inst);
    if (vr != VK_SUCCESS || !inst) {
        fprintf(stderr, "[FAIL] vkCreateInstance vr=%d\n", vr);
        return 24;
    }

    auto pEnumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        vkDev->GetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices"));
    auto pGetProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        vkDev->GetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties"));
    auto pGetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        vkDev->GetInstanceProcAddr(inst, "vkGetPhysicalDeviceMemoryProperties"));
    auto pCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        vkDev->GetInstanceProcAddr(inst, "vkCreateDevice"));
    auto pGetDevProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vkDev->GetInstanceProcAddr(inst, "vkGetDeviceProcAddr"));
    auto pDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        vkDev->GetInstanceProcAddr(inst, "vkDestroyInstance"));

    uint32_t gpuCount = 0;
    vr = pEnumPhys(inst, &gpuCount, nullptr);
    printf("[E2E-VK] vkEnumeratePhysicalDevices vr=%d count=%u\n", vr, gpuCount);
    if (vr != VK_SUCCESS || gpuCount == 0) {
        return 25;
    }
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    gpuCount = 1;
    pEnumPhys(inst, &gpuCount, &phys);

    VkPhysicalDeviceProperties props{};
    pGetProps(phys, &props);
    printf("[E2E-VK] GPU: %s (vendor=0x%x, device=0x%x, api=%u.%u.%u)\n",
           props.deviceName, props.vendorID, props.deviceID,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev = VK_NULL_HANDLE;
    vr = pCreateDevice(phys, &dci, nullptr, &dev);
    if (vr != VK_SUCCESS || !dev) {
        fprintf(stderr, "[FAIL] vkCreateDevice vr=%d\n", vr);
        return 26;
    }

    auto pCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(pGetDevProc(dev, "vkCreateBuffer"));
    auto pGetBufReq = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(pGetDevProc(dev, "vkGetBufferMemoryRequirements"));
    auto pAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(pGetDevProc(dev, "vkAllocateMemory"));
    auto pBindBuf = reinterpret_cast<PFN_vkBindBufferMemory>(pGetDevProc(dev, "vkBindBufferMemory"));
    auto pMapMem = reinterpret_cast<PFN_vkMapMemory>(pGetDevProc(dev, "vkMapMemory"));
    auto pFlushMem = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(pGetDevProc(dev, "vkFlushMappedMemoryRanges"));
    auto pInvalidateMem = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(pGetDevProc(dev, "vkInvalidateMappedMemoryRanges"));
    auto pUnmapMem = reinterpret_cast<PFN_vkUnmapMemory>(pGetDevProc(dev, "vkUnmapMemory"));
    auto pFreeMem = reinterpret_cast<PFN_vkFreeMemory>(pGetDevProc(dev, "vkFreeMemory"));
    auto pDestroyBuf = reinterpret_cast<PFN_vkDestroyBuffer>(pGetDevProc(dev, "vkDestroyBuffer"));
    auto pDestroyDev = reinterpret_cast<PFN_vkDestroyDevice>(pGetDevProc(dev, "vkDestroyDevice"));
    auto pGetQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(pGetDevProc(dev, "vkGetDeviceQueue"));
    auto pCreateCmdPool = reinterpret_cast<PFN_vkCreateCommandPool>(pGetDevProc(dev, "vkCreateCommandPool"));
    auto pDestroyCmdPool = reinterpret_cast<PFN_vkDestroyCommandPool>(pGetDevProc(dev, "vkDestroyCommandPool"));
    auto pAllocCmdBufs = reinterpret_cast<PFN_vkAllocateCommandBuffers>(pGetDevProc(dev, "vkAllocateCommandBuffers"));
    auto pBeginCmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(pGetDevProc(dev, "vkBeginCommandBuffer"));
    auto pEndCmd = reinterpret_cast<PFN_vkEndCommandBuffer>(pGetDevProc(dev, "vkEndCommandBuffer"));
    auto pCmdCopyBuf = reinterpret_cast<PFN_vkCmdCopyBuffer>(pGetDevProc(dev, "vkCmdCopyBuffer"));
    auto pQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(pGetDevProc(dev, "vkQueueSubmit"));
    auto pQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(pGetDevProc(dev, "vkQueueWaitIdle"));

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer bufSrc = VK_NULL_HANDLE, bufDst = VK_NULL_HANDLE;
    if (pCreateBuffer(dev, &bci, nullptr, &bufSrc) != VK_SUCCESS ||
        pCreateBuffer(dev, &bci, nullptr, &bufDst) != VK_SUCCESS) {
        fprintf(stderr, "[FAIL] vkCreateBuffer failed\n");
        return 27;
    }

    VkMemoryRequirements memReq{};
    pGetBufReq(dev, bufSrc, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    pGetMemProps(phys, &memProps);
    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memIdx = i;
            break;
        }
    }
    printf("[E2E-VK] Buffer memReq size=%llu bits=0x%x chosen memType=%u (flags=0x%x)\n",
           static_cast<unsigned long long>(memReq.size), memReq.memoryTypeBits, memIdx,
           memIdx < memProps.memoryTypeCount ? memProps.memoryTypes[memIdx].propertyFlags : 0);
    if (memIdx == UINT32_MAX) {
        fprintf(stderr, "[FAIL] No HOST_VISIBLE memory type\n");
        return 28;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = memIdx;
    VkDeviceMemory memSrc = VK_NULL_HANDLE, memDst = VK_NULL_HANDLE;
    if (pAllocMem(dev, &mai, nullptr, &memSrc) != VK_SUCCESS ||
        pAllocMem(dev, &mai, nullptr, &memDst) != VK_SUCCESS) {
        fprintf(stderr, "[FAIL] vkAllocateMemory failed\n");
        return 29;
    }
    if (pBindBuf(dev, bufSrc, memSrc, 0) != VK_SUCCESS ||
        pBindBuf(dev, bufDst, memDst, 0) != VK_SUCCESS) {
        fprintf(stderr, "[FAIL] vkBindBufferMemory failed\n");
        return 30;
    }

    // Write magic pattern into bufSrc (leaving persistently mapped or unmapped - tests coherent auto-flush!)
    void* mappedSrc = nullptr;
    vr = pMapMem(dev, memSrc, 0, 4096, 0, &mappedSrc);
    printf("[E2E-VK] vkMapMemory(src) vr=%d ptr=%p\n", vr, mappedSrc);
    if (vr != VK_SUCCESS || !mappedSrc) return 31;
    uint32_t* ps = static_cast<uint32_t*>(mappedSrc);
    ps[0] = 0xCAFEBABE;
    ps[1] = 0x12345678;
    VkMappedMemoryRange rngSrc{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memSrc, 0, VK_WHOLE_SIZE};
    pFlushMem(dev, 1, &rngSrc);
    pUnmapMem(dev, memSrc);

    // Clear bufDst to 0
    void* mappedDst = nullptr;
    vr = pMapMem(dev, memDst, 0, 4096, 0, &mappedDst);
    if (vr != VK_SUCCESS || !mappedDst) return 32;
    memset(mappedDst, 0, 4096);
    VkMappedMemoryRange rngDst{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memDst, 0, VK_WHOLE_SIZE};
    pFlushMem(dev, 1, &rngDst);

    // Execute GPU vkCmdCopyBuffer(bufSrc -> bufDst) on Maleoon 916!
    VkQueue queue = VK_NULL_HANDLE;
    pGetQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, 0};
    VkCommandPool pool = VK_NULL_HANDLE;
    pCreateCmdPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    pAllocCmdBufs(dev, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    pBeginCmd(cmd, &bi);
    VkBufferCopy region{0, 0, 4096};
    pCmdCopyBuf(cmd, bufSrc, bufDst, 1, &region);
    pEndCmd(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    vr = pQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[FAIL] vkQueueSubmit vr=%d\n", vr);
        return 33;
    }
    vr = pQueueWaitIdle(queue);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[FAIL] vkQueueWaitIdle vr=%d\n", vr);
        return 34;
    }

    // Invalidate & read back bufDst from Maleoon 916!
    vr = pInvalidateMem(dev, 1, &rngDst);
    uint32_t* pd = static_cast<uint32_t*>(mappedDst);
    printf("[E2E-VK] GPU vkCmdCopyBuffer readback: pd[0]=0x%08x pd[1]=0x%08x (expected 0xcafebabe 0x12345678)\n",
           pd[0], pd[1]);
    if (vr != VK_SUCCESS || pd[0] != 0xCAFEBABE || pd[1] != 0x12345678) {
        fprintf(stderr, "[FAIL] Vulkan GPU copy readback mismatch!\n");
        return 35;
    }
    pUnmapMem(dev, memDst);
    pDestroyCmdPool(dev, pool, nullptr);
    pDestroyBuf(dev, bufSrc, nullptr);
    pDestroyBuf(dev, bufDst, nullptr);
    pFreeMem(dev, memSrc, nullptr);
    pFreeMem(dev, memDst, nullptr);
    pDestroyDev(dev, nullptr);
    pDestroyInstance(inst, nullptr);
    vkDev->common.close(&vkDev->common);
    printf("[E2E-VK] Vulkan 1.2 end-to-end GPU copy + readback PASS!\n");
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = (argc > 1) ? argv[1] : "gles";
    if (strcmp(mode, "gles") == 0) {
        return test_gles_and_hals();
    } else if (strcmp(mode, "vulkan") == 0) {
        return test_vulkan();
    }
    fprintf(stderr, "Usage: %s [gles|vulkan]\n", argv[0]);
    return 99;
}
