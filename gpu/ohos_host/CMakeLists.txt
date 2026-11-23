# SPDX-License-Identifier: MIT
# Standalone trimmed Google gfxstream host library for HarmonyOS PC (API 26, ARM64).
# Zero X11, zero GLX, zero Magma, zero Protobuf, zero Flatbuffers, zero VirtioGpu.
cmake_minimum_required(VERSION 3.18)
project(svbox_gfxstream_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(AEMU_ROOT "${GPU_ROOT}/third_party/aemu")
set(GFX_ROOT "${GPU_ROOT}/third_party/gfxstream")

add_compile_definitions(
    __OHOS__=1
    OHOS_PLATFORM=1
    GFXSTREAM_ENABLE_HOST_GLES=1
    GFXSTREAM_ENABLE_HOST_MAGMA=0
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    VK_GFXSTREAM_STRUCTURE_TYPE_EXT
    VK_GOOGLE_gfxstream
    _FILE_OFFSET_BITS=64
)

add_compile_options(
    -O2
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-private-field
    -Wno-missing-field-initializers
    -Wno-return-type-c-linkage
    -Wno-extern-c-compat
    -Wno-inconsistent-missing-override
    -Wno-macro-redefined
    -Wno-deprecated-declarations
    -Wno-sign-compare
    -Wno-reorder
    -Wno-switch
    -Wno-null-conversion
    -Wno-format
)

# 1. Header-only interface libraries
add_library(gfxstream_vulkan_headers INTERFACE)
target_include_directories(gfxstream_vulkan_headers INTERFACE
    "${GFX_ROOT}/common/vulkan/include"
)

add_library(gfxstream_egl_headers INTERFACE)
target_include_directories(gfxstream_egl_headers INTERFACE
    "${GFX_ROOT}/include"
)

add_library(renderdoc_headers INTERFACE)
target_include_directories(renderdoc_headers INTERFACE
    "${GFX_ROOT}/third-party/renderdoc/include"
)

# 2. AEMU base + snapshot + logging + host-common
add_library(aemu_snapshot STATIC
    "${AEMU_ROOT}/snapshot/TextureLoader.cpp"
    "${AEMU_ROOT}/snapshot/TextureSaver.cpp"
)
target_include_directories(aemu_snapshot PUBLIC
    "${AEMU_ROOT}/snapshot/include"
    "${AEMU_ROOT}/base/include"
    "${AEMU_ROOT}/host-common/include"
)

add_library(aemu_logging STATIC
    "${AEMU_ROOT}/host-common/logging.cpp"
    "${AEMU_ROOT}/host-common/GfxstreamFatalError.cpp"
    "${AEMU_ROOT}/base/Metrics.cpp"
)
target_include_directories(aemu_logging PUBLIC
    "${AEMU_ROOT}/base/include"
    "${AEMU_ROOT}/host-common/include"
    "${AEMU_ROOT}/snapshot/include"
)
target_compile_options(aemu_logging PRIVATE -idirafter "${AEMU_ROOT}/host-common/include/host-common")

add_library(aemu_base STATIC
    "${AEMU_ROOT}/base/AlignedBuf.cpp"
    "${AEMU_ROOT}/base/CLog.cpp"
    "${AEMU_ROOT}/base/CpuTime.cpp"
    "${AEMU_ROOT}/base/FileUtils.cpp"
    "${AEMU_ROOT}/base/FunctorThread.cpp"
    "${AEMU_ROOT}/base/GLObjectCounter.cpp"
    "${AEMU_ROOT}/base/GraphicsObjectCounter.cpp"
    "${AEMU_ROOT}/base/HealthMonitor.cpp"
    "${AEMU_ROOT}/base/LayoutResolver.cpp"
    "${AEMU_ROOT}/base/MemStream.cpp"
    "${AEMU_ROOT}/base/StdioStream.cpp"
    "${AEMU_ROOT}/base/MemoryTracker.cpp"
    "${AEMU_ROOT}/base/MessageChannel.cpp"
    "${AEMU_ROOT}/base/PathUtils.cpp"
    "${AEMU_ROOT}/base/ring_buffer.cpp"
    "${AEMU_ROOT}/base/SharedLibrary.cpp"
    "${AEMU_ROOT}/base/StringFormat.cpp"
    "${AEMU_ROOT}/base/Stream.cpp"
    "${AEMU_ROOT}/base/StreamSerializing.cpp"
    "${AEMU_ROOT}/base/SubAllocator.cpp"
    "${AEMU_ROOT}/base/System.cpp"
    "${AEMU_ROOT}/base/Tracing.cpp"
    "${AEMU_ROOT}/base/SharedMemory_posix.cpp"
    "${AEMU_ROOT}/base/Thread_pthread.cpp"
)
target_include_directories(aemu_base PUBLIC
    "${AEMU_ROOT}/base/include"
    "${AEMU_ROOT}/host-common/include"
    "${AEMU_ROOT}/snapshot/include"
    "${AEMU_ROOT}"
)
target_link_libraries(aemu_base PUBLIC aemu_logging aemu_snapshot dl)

add_library(aemu_host_common STATIC
    "${AEMU_ROOT}/host-common/empty-crash-handler.cpp"
    "${AEMU_ROOT}/host-common/crash_reporter.cpp"
    "${AEMU_ROOT}/host-common/vm_operations.cpp"
    "${AEMU_ROOT}/host-common/feature_control.cpp"
    "${AEMU_ROOT}/host-common/FeatureControlOverride.cpp"
    "${AEMU_ROOT}/host-common/dma_device.cpp"
    "${AEMU_ROOT}/host-common/sync_device.cpp"
    "${AEMU_ROOT}/host-common/misc.cpp"
    "${AEMU_ROOT}/host-common/window_operations.cpp"
    "${AEMU_ROOT}/host-common/AndroidPipe.cpp"
    "${AEMU_ROOT}/host-common/HostmemIdMapping.cpp"
    "${AEMU_ROOT}/host-common/RefcountPipe.cpp"
    "${AEMU_ROOT}/host-common/GraphicsAgentFactory.cpp"
    "${AEMU_ROOT}/host-common/GoldfishSyncCommandQueue.cpp"
    "${AEMU_ROOT}/host-common/goldfish_sync.cpp"
    "${AEMU_ROOT}/host-common/DmaMap.cpp"
    "${AEMU_ROOT}/host-common/GoldfishDma.cpp"
    "${AEMU_ROOT}/host-common/address_space_device_control_ops.cpp"
    "${AEMU_ROOT}/host-common/address_space_device.cpp"
    "${AEMU_ROOT}/host-common/address_space_host_memory_allocator.cpp"
    "${AEMU_ROOT}/host-common/address_space_shared_slots_host_memory_allocator.cpp"
    "${AEMU_ROOT}/host-common/address_space_graphics.cpp"
    "${AEMU_ROOT}/host-common/address_space_host_media.cpp"
    "${AEMU_ROOT}/host-common/hw-config.cpp"
)
target_include_directories(aemu_host_common PUBLIC
    "${AEMU_ROOT}/host-common/include"
    "${AEMU_ROOT}/base/include"
    "${AEMU_ROOT}/snapshot/include"
)
target_compile_options(aemu_host_common PRIVATE -idirafter "${AEMU_ROOT}/host-common/include/host-common")
target_link_libraries(aemu_host_common PUBLIC aemu_base gfxstream_vulkan_headers renderdoc_headers)

# 3. gfxstream common + utils + features + tracing + etc + compressedTextures
add_library(gfx_common STATIC
    "${GFX_ROOT}/common/etc/etc.cpp"
    "${GFX_ROOT}/common/utils/Strings.cpp"
    "${GFX_ROOT}/utils/GfxApiLogger.cpp"
    "${GFX_ROOT}/host/features/Features.cpp"
    "${GFX_ROOT}/host/tracing/Tracing.cpp"
    "${GFX_ROOT}/host/compressedTextureFormats/AstcCpuDecompressorNoOp.cpp"
    "${GFX_ROOT}/host/apigen-codec-common/ChecksumCalculator.cpp"
    "${GFX_ROOT}/host/apigen-codec-common/ChecksumCalculatorThreadInfo.cpp"
    "${GFX_ROOT}/host/apigen-codec-common/glUtils.cpp"
)
target_include_directories(gfx_common PUBLIC
    "${GFX_ROOT}"
    "${GFX_ROOT}/include"
    "${GFX_ROOT}/common/etc/include"
    "${GFX_ROOT}/common/utils/include"
    "${GFX_ROOT}/utils/include"
    "${GFX_ROOT}/host"
    "${GFX_ROOT}/host/include"
    "${GFX_ROOT}/host/backend/include"
    "${GFX_ROOT}/host/features/include"
    "${GFX_ROOT}/host/tracing/include"
    "${GFX_ROOT}/host/apigen-codec-common"
    "${GFX_ROOT}/host/gl/gl-host-common/include"
    "${GFX_ROOT}/third-party/glm/include"
)
target_link_libraries(gfx_common PUBLIC aemu_host_common gfxstream_vulkan_headers gfxstream_egl_headers renderdoc_headers)

# 4. gfxstream gl-host-common
add_library(gfx_gl_host_common STATIC
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/misc.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengles.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/EmuglBackendList.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/emugl_config.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/GLProcessPipe.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/gpuinfo.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/logger.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/OpenglEsPipe.cpp"
    "${GFX_ROOT}/host/gl/gl-host-common/opengl/NativeGpuInfo_linux.cpp"
)
target_link_libraries(gfx_gl_host_common PUBLIC gfx_common)

# 5. glestranslator (EGL-on-EGL only, ZERO GLX / ZERO X11!)
add_library(gfx_gl_translator STATIC
    # GLcommon
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/rgtc.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/FramebufferData.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLBackgroundLoader.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLDispatch.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLESbuffer.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLEScontext.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLESpointer.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLESvalidate.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/GLutils.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/NamedObject.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/ObjectData.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/ObjectNameSpace.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/PaletteTexture.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/RangeManip.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/SaveableTexture.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/ScopedGLState.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/ShareGroup.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/TextureData.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/TextureUtils.cpp"
    # GLES_V2
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/ANGLEShaderParser.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/GLESv2Context.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/GLESv2Imp.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/GLESv2Validate.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/ProgramData.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/SamplerData.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/ShaderParser.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/ShaderValidator.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_V2/TransformFeedbackData.cpp"
    # GLES_CM
    "${GFX_ROOT}/host/gl/glestranslator/GLES_CM/CoreProfileEngine.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_CM/GLEScmContext.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_CM/GLEScmImp.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_CM/GLEScmUtils.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/GLES_CM/GLEScmValidate.cpp"
    # EGL (EglOsApi_egl.cpp ONLY)
    "${GFX_ROOT}/host/gl/glestranslator/EGL/ClientAPIExts.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglConfig.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglContext.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglDisplay.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglGlobalInfo.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglImp.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglOsApi_egl.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglPbufferSurface.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglSurface.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglThreadInfo.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglValidate.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/EglWindowSurface.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/ShaderCache.cpp"
    "${GFX_ROOT}/host/gl/glestranslator/EGL/ThreadInfo.cpp"
)
target_include_directories(gfx_gl_translator PUBLIC
    "${GFX_ROOT}/host/gl/glestranslator/include"
    "${GFX_ROOT}/host/gl/glestranslator/GLcommon/include"
)
target_link_libraries(gfx_gl_translator PUBLIC gfx_gl_host_common)

# 6. GLES decoders, GLSnapshot, OpenGLESDispatch, GL server, renderControl_dec
add_library(gfx_gl_server STATIC
    "${GFX_ROOT}/host/gl/glsnapshot/GLSnapshot.cpp"
    "${GFX_ROOT}/host/gl/gles1_dec/gles1_dec.cpp"
    "${GFX_ROOT}/host/gl/gles1_dec/gles1_server_context.cpp"
    "${GFX_ROOT}/host/gl/gles1_dec/GLESv1Decoder.cpp"
    "${GFX_ROOT}/host/gl/gles2_dec/gles2_dec.cpp"
    "${GFX_ROOT}/host/gl/gles2_dec/gles2_server_context.cpp"
    "${GFX_ROOT}/host/gl/gles2_dec/GLESv2Decoder.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/EGLDispatch.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/gles1_stubbed_in_translator_namespace.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/gles2_stubbed_in_translator_namespace.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/GLESv1Dispatch.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/GLESv2Dispatch.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/OpenGLDispatchLoader.cpp"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch/StaticDispatch.cpp"
    "${GFX_ROOT}/host/gl/BufferGl.cpp"
    "${GFX_ROOT}/host/gl/ColorBufferGl.cpp"
    "${GFX_ROOT}/host/gl/CompositorGl.cpp"
    "${GFX_ROOT}/host/gl/DebugGl.cpp"
    "${GFX_ROOT}/host/gl/DisplayGl.cpp"
    "${GFX_ROOT}/host/gl/DisplaySurfaceGl.cpp"
    "${GFX_ROOT}/host/gl/EmulatedEglConfig.cpp"
    "${GFX_ROOT}/host/gl/EmulatedEglContext.cpp"
    "${GFX_ROOT}/host/gl/EmulatedEglFenceSync.cpp"
    "${GFX_ROOT}/host/gl/EmulatedEglImage.cpp"
    "${GFX_ROOT}/host/gl/EmulatedEglWindowSurface.cpp"
    "${GFX_ROOT}/host/gl/EmulationGl.cpp"
    "${GFX_ROOT}/host/gl/GLESVersionDetector.cpp"
    "${GFX_ROOT}/host/gl/ReadbackWorkerGl.cpp"
    "${GFX_ROOT}/host/gl/TextureDraw.cpp"
    "${GFX_ROOT}/host/gl/TextureResize.cpp"
    "${GFX_ROOT}/host/gl/YUVConverter.cpp"
    "${GFX_ROOT}/host/renderControl_dec/renderControl_dec.cpp"
    "${GFX_ROOT}/host/renderControl_dec/renderControl_server_context.cpp"
)
target_include_directories(gfx_gl_server PUBLIC
    "${GFX_ROOT}/host/gl"
    "${GFX_ROOT}/host/gl/glsnapshot"
    "${GFX_ROOT}/host/gl/OpenGLESDispatch"
    "${GFX_ROOT}/host/renderControl_dec"
)
target_link_libraries(gfx_gl_server PUBLIC gfx_gl_translator)

# 7. Vulkan server (cereal + emulated_textures + VkDecoder + DisplaySurfaceVk w/ VK_OHOS_surface)
add_library(gfx_vk_server STATIC
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_extension_structs.cpp"
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_marshaling.cpp"
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_reserved_marshaling.cpp"
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_deepcopy.cpp"
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_dispatch.cpp"
    "${GFX_ROOT}/host/vulkan/cereal/common/goldfish_vk_transform.cpp"
    "${GFX_ROOT}/host/vulkan/emulated_textures/AstcTexture.cpp"
    "${GFX_ROOT}/host/vulkan/emulated_textures/CompressedImageInfo.cpp"
    "${GFX_ROOT}/host/vulkan/emulated_textures/GpuDecompressionPipeline.cpp"
    "${GFX_ROOT}/host/vulkan/BorrowedImageVk.cpp"
    "${GFX_ROOT}/host/vulkan/BufferVk.cpp"
    "${GFX_ROOT}/host/vulkan/ColorBufferVk.cpp"
    "${GFX_ROOT}/host/vulkan/CompositorVk.cpp"
    "${GFX_ROOT}/host/vulkan/DeviceLostHelper.cpp"
    "${GFX_ROOT}/host/vulkan/DeviceOpTracker.cpp"
    "${GFX_ROOT}/host/vulkan/DisplayVk.cpp"
    "${GFX_ROOT}/host/vulkan/DisplaySurfaceVk.cpp"
    "${GFX_ROOT}/host/vulkan/DebugUtilsHelper.cpp"
    "${GFX_ROOT}/host/vulkan/PostWorkerVk.cpp"
    "${GFX_ROOT}/host/vulkan/SwapChainStateVk.cpp"
    "${GFX_ROOT}/host/vulkan/RenderThreadInfoVk.cpp"
    "${GFX_ROOT}/host/vulkan/VkAndroidNativeBuffer.cpp"
    "${GFX_ROOT}/host/vulkan/VkCommonOperations.cpp"
    "${GFX_ROOT}/host/vulkan/VkDecoder.cpp"
    "${GFX_ROOT}/host/vulkan/VkDecoderGlobalState.cpp"
    "${GFX_ROOT}/host/vulkan/VkDecoderSnapshot.cpp"
    "${GFX_ROOT}/host/vulkan/VkDecoderSnapshotUtils.cpp"
    "${GFX_ROOT}/host/vulkan/VkEmulatedPhysicalDeviceMemory.cpp"
    "${GFX_ROOT}/host/vulkan/VkEmulatedPhysicalDeviceQueue.cpp"
    "${GFX_ROOT}/host/vulkan/VkFormatUtils.cpp"
    "${GFX_ROOT}/host/vulkan/VkReconstruction.cpp"
    "${GFX_ROOT}/host/vulkan/VulkanBoxedHandles.cpp"
    "${GFX_ROOT}/host/vulkan/VulkanDispatch.cpp"
    "${GFX_ROOT}/host/vulkan/VulkanHandleMapping.cpp"
    "${GFX_ROOT}/host/vulkan/VulkanStream.cpp"
    "${GFX_ROOT}/host/vulkan/vk_util.cpp"
)
target_include_directories(gfx_vk_server PUBLIC
    "${GFX_ROOT}/host/vulkan"
    "${GFX_ROOT}/host/vulkan/cereal"
    "${GFX_ROOT}/host/vulkan/cereal/common"
)
target_link_libraries(gfx_vk_server PUBLIC gfx_common)

# 8. Core stream server + OHOS C API wrapper -> libgfxstream_host.so
add_library(gfxstream_host SHARED
    "${GFX_ROOT}/host/Buffer.cpp"
    "${GFX_ROOT}/host/ExternalObjectManager.cpp"
    "${GFX_ROOT}/host/ColorBuffer.cpp"
    "${GFX_ROOT}/host/GfxStreamAgents.cpp"
    "${GFX_ROOT}/host/VsyncThread.cpp"
    "${GFX_ROOT}/host/ChannelStream.cpp"
    "${GFX_ROOT}/host/DisplaySurface.cpp"
    "${GFX_ROOT}/host/DisplaySurfaceUser.cpp"
    "${GFX_ROOT}/host/Hwc2.cpp"
    "${GFX_ROOT}/host/PostWorker.cpp"
    "${GFX_ROOT}/host/PostWorkerGl.cpp"
    "${GFX_ROOT}/host/ReadBuffer.cpp"
    "${GFX_ROOT}/host/RenderChannelImpl.cpp"
    "${GFX_ROOT}/host/RenderThread.cpp"
    "${GFX_ROOT}/host/RenderThreadInfo.cpp"
    "${GFX_ROOT}/host/RenderThreadInfoGl.cpp"
    "${GFX_ROOT}/host/RingStream.cpp"
    "${GFX_ROOT}/host/SyncThread.cpp"
    "${GFX_ROOT}/host/RenderControl.cpp"
    "${GFX_ROOT}/host/RenderWindow.cpp"
    "${GFX_ROOT}/host/RenderLibImpl.cpp"
    "${GFX_ROOT}/host/RendererImpl.cpp"
    "${GFX_ROOT}/host/FrameBuffer.cpp"
    "${GFX_ROOT}/host/GraphicsDriverLock.cpp"
    "${GFX_ROOT}/host/render_api.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/NativeSubWindow_ohos.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/svbox_gfxstream_capi.cpp"
)
target_include_directories(gfxstream_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(gfxstream_host PRIVATE
    gfx_gl_server
    gfx_vk_server
    gfx_gl_host_common
    gfx_common
    aemu_host_common
    aemu_base
    native_buffer
    native_window
    dl
)
