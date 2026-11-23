cmake_minimum_required(VERSION 3.16)
project(svbox_gfxstream_guest C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(GPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(REPO_ROOT "${GPU_ROOT}/.." ABSOLUTE)

set(GF_ROOT "${GPU_ROOT}/third_party/goldfish-opengl-r")
set(AOSP_HDR "${GPU_ROOT}/third_party/aosp_headers")
set(NDK_SYSROOT "${GPU_ROOT}/ndk_sysroot/sysroot")
set(ROOTFS_DIR "${REPO_ROOT}/rootfs")

# Verify required Android 11 rootfs libraries exist for exact ABI linking
set(A_LIBCXX "${ROOTFS_DIR}/vendor/lib64/libc++_shared.so")
set(A_LIBCUTILS "${ROOTFS_DIR}/system/apex/com.android.vndk.current/lib64/libcutils.so")
set(A_LIBLOG "${ROOTFS_DIR}/system/lib64/liblog.so")
set(A_LIBNATIVEWINDOW "${ROOTFS_DIR}/system/lib64/libnativewindow.so")
set(A_LIBSYNC "${ROOTFS_DIR}/system/lib64/libsync.so")
set(A_LIBUTILS "${ROOTFS_DIR}/system/apex/com.android.vndk.current/lib64/libutils.so")

foreach(_lib ${A_LIBCXX} ${A_LIBCUTILS} ${A_LIBLOG} ${A_LIBNATIVEWINDOW} ${A_LIBSYNC} ${A_LIBUTILS})
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "Missing rootfs library for ABI linking: ${_lib}")
    endif()
endforeach()

# Common include paths (AOSP Android 11 headers + goldfish-opengl common headers)
set(AOSP_COMMON_INCLUDES
    "${AOSP_HDR}/libhardware/include"
    "${AOSP_HDR}/system_core/include"
    "${AOSP_HDR}/system_core/libcutils/include"
    "${AOSP_HDR}/system_core/libutils/include"
    "${AOSP_HDR}/system_core/libsystem/include"
    "${AOSP_HDR}/system_core/libsync/include"
    "${AOSP_HDR}/system_logging/liblog/include"
    "${AOSP_HDR}/frameworks_native/include"
    "${AOSP_HDR}/frameworks_native/libs/nativebase/include"
    "${AOSP_HDR}/frameworks_native/libs/nativewindow/include"
    "${AOSP_HDR}/frameworks_native/libs/arect/include"
    "${AOSP_HDR}/frameworks_native/libs/ui/include"
    "${AOSP_HDR}/frameworks_native/vulkan/include"
    "${NDK_SYSROOT}/usr/include/drm"
)

set(GF_COMMON_INCLUDES
    "${GF_ROOT}/host/include/libOpenglRender"
    "${GF_ROOT}/system/include"
    "${GF_ROOT}/shared/gralloc_cb/include"
    "${GF_ROOT}/shared/qemupipe/include"
    "${GF_ROOT}/shared/qemupipe/include-types"
    "${GF_ROOT}/shared/GoldfishAddressSpace/include"
    "${GF_ROOT}/android-emu"
)

add_compile_definitions(
    ANDROID
    __ANDROID__
    PLATFORM_SDK_VERSION=30
    GOLDFISH_HIDL_GRALLOC
    GFXSTREAM
    GL_GLEXT_PROTOTYPES
    EGL_EGLEXT_PROTOTYPES
    PAGE_SIZE=4096
)

add_compile_options(
    -O2
    -fno-exceptions
    -fno-rtti
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-private-field
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-gnu-designator
    -Wno-deprecated-declarations
    -Wno-format
    -Wno-inconsistent-missing-override
    -Wno-macro-redefined
)

# Output staging layout matching Android /vendor/lib64/{,egl,hw}
set(OUT_LIB64 "${CMAKE_BINARY_DIR}/out/lib64")
set(OUT_EGL "${OUT_LIB64}/egl")
set(OUT_HW "${OUT_LIB64}/hw")

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUT_LIB64}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/static")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Helper to enforce SONAME and strict undefined symbol checks
function(svbox_android_so target_name subdir)
    if(NOT "${subdir}" STREQUAL "")
        set_target_properties(${target_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OUT_LIB64}/${subdir}"
        )
    endif()
    # Ensure SONAME is just the filename (no path) and verify zero undefined symbols
    target_link_options(${target_name} PRIVATE
        -Wl,--no-undefined
        -Wl,--exclude-libs,ALL
    )
    target_link_libraries(${target_name} PRIVATE ${A_LIBCXX} dl m)
endfunction()

# =============================================================================
# 1. Static helper libraries
# =============================================================================
add_library(qemupipe_svbox STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/svbox_pipe_guest.cpp"
)
target_include_directories(qemupipe_svbox PUBLIC
    "${GF_ROOT}/shared/qemupipe/include"
    "${GF_ROOT}/shared/qemupipe/include-types"
    ${AOSP_COMMON_INCLUDES}
)

add_library(ringbuffer STATIC
    "${GF_ROOT}/android-emu/android/base/ring_buffer.c"
)
target_include_directories(ringbuffer PUBLIC
    "${GF_ROOT}/android-emu"
    ${AOSP_COMMON_INCLUDES}
)

add_library(GoldfishAddressSpace STATIC
    "${GF_ROOT}/shared/GoldfishAddressSpace/goldfish_address_space.cpp"
)
target_include_directories(GoldfishAddressSpace PUBLIC
    "${GF_ROOT}/shared/GoldfishAddressSpace/include"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)

# =============================================================================
# 2. Core shared libraries (/vendor/lib64/)
# =============================================================================

# libandroidemu.so
add_library(androidemu SHARED
    "${GF_ROOT}/android-emu/android/base/AlignedBuf.cpp"
    "${GF_ROOT}/android-emu/android/base/files/MemStream.cpp"
    "${GF_ROOT}/android-emu/android/base/files/Stream.cpp"
    "${GF_ROOT}/android-emu/android/base/files/StreamSerializing.cpp"
    "${GF_ROOT}/android-emu/android/base/Pool.cpp"
    "${GF_ROOT}/android-emu/android/base/StringFormat.cpp"
    "${GF_ROOT}/android-emu/android/base/AndroidSubAllocator.cpp"
    "${GF_ROOT}/android-emu/android/base/synchronization/AndroidMessageChannel.cpp"
    "${GF_ROOT}/android-emu/android/base/threads/AndroidFunctorThread.cpp"
    "${GF_ROOT}/android-emu/android/base/threads/AndroidThreadStore.cpp"
    "${GF_ROOT}/android-emu/android/base/threads/AndroidThread_pthread.cpp"
    "${GF_ROOT}/android-emu/android/base/threads/AndroidWorkPool.cpp"
    "${GF_ROOT}/android-emu/android/base/Tracing.cpp"
    "${GF_ROOT}/android-emu/android/utils/debug.c"
)
target_compile_definitions(androidemu PRIVATE LOG_TAG="androidemu")
target_compile_options(androidemu PRIVATE -fvisibility=default -fstrict-aliasing)
target_include_directories(androidemu PUBLIC "${GF_ROOT}/android-emu" ${AOSP_COMMON_INCLUDES})
target_link_libraries(androidemu PRIVATE ${A_LIBCUTILS} ${A_LIBUTILS} ${A_LIBLOG})
svbox_android_so(androidemu "")

# libOpenglCodecCommon.so
add_library(OpenglCodecCommon SHARED
    "${GF_ROOT}/shared/OpenglCodecCommon/EncoderDebug.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/GLClientState.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/GLESTextureUtils.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/ChecksumCalculator.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/GLSharedGroup.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/glUtils.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/IndexRangeCache.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/SocketStream.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/TcpStream.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/auto_goldfish_dma_context.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/etc.cpp"
    "${GF_ROOT}/shared/OpenglCodecCommon/goldfish_dma.cpp"
)
target_compile_definitions(OpenglCodecCommon PRIVATE LOG_TAG="eglCodecCommon")
target_include_directories(OpenglCodecCommon PUBLIC
    "${GF_ROOT}/shared/OpenglCodecCommon"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(OpenglCodecCommon PUBLIC
    androidemu
)
target_link_libraries(OpenglCodecCommon PRIVATE
    qemupipe_svbox
    ${A_LIBCUTILS}
    ${A_LIBUTILS}
    ${A_LIBLOG}
)
svbox_android_so(OpenglCodecCommon "")

# lib_renderControl_enc.so
add_library(_renderControl_enc SHARED
    "${GF_ROOT}/system/renderControl_enc/renderControl_client_context.cpp"
    "${GF_ROOT}/system/renderControl_enc/renderControl_enc.cpp"
    "${GF_ROOT}/system/renderControl_enc/renderControl_entry.cpp"
)
target_include_directories(_renderControl_enc PUBLIC
    "${GF_ROOT}/system/renderControl_enc"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(_renderControl_enc PRIVATE OpenglCodecCommon ${A_LIBLOG})
svbox_android_so(_renderControl_enc "")

# libGLESv1_enc.so
add_library(GLESv1_enc SHARED
    "${GF_ROOT}/system/GLESv1_enc/GLEncoder.cpp"
    "${GF_ROOT}/system/GLESv1_enc/GLEncoderUtils.cpp"
    "${GF_ROOT}/system/GLESv1_enc/gl_client_context.cpp"
    "${GF_ROOT}/system/GLESv1_enc/gl_enc.cpp"
    "${GF_ROOT}/system/GLESv1_enc/gl_entry.cpp"
)
target_compile_definitions(GLESv1_enc PRIVATE LOG_TAG="emuglGLESv1_enc")
target_include_directories(GLESv1_enc PUBLIC
    "${GF_ROOT}/system/GLESv1_enc"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(GLESv1_enc PRIVATE OpenglCodecCommon ${A_LIBLOG})
svbox_android_so(GLESv1_enc "")

# libGLESv2_enc.so
add_library(GLESv2_enc SHARED
    "${GF_ROOT}/system/GLESv2_enc/GL2EncoderUtils.cpp"
    "${GF_ROOT}/system/GLESv2_enc/GL2Encoder.cpp"
    "${GF_ROOT}/system/GLESv2_enc/GLESv2Validation.cpp"
    "${GF_ROOT}/system/GLESv2_enc/gl2_client_context.cpp"
    "${GF_ROOT}/system/GLESv2_enc/gl2_enc.cpp"
    "${GF_ROOT}/system/GLESv2_enc/gl2_entry.cpp"
    "${GF_ROOT}/system/GLESv2_enc/IOStream2.cpp"
)
target_compile_definitions(GLESv2_enc PRIVATE LOG_TAG="emuglGLESv2_enc")
target_include_directories(GLESv2_enc PUBLIC
    "${GF_ROOT}/system/GLESv2_enc"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(GLESv2_enc PRIVATE OpenglCodecCommon ${A_LIBLOG})
svbox_android_so(GLESv2_enc "")

# libvulkan_enc.so
add_library(vulkan_enc SHARED
    "${GF_ROOT}/system/vulkan_enc/AndroidHardwareBuffer.cpp"
    "${GF_ROOT}/system/vulkan_enc/CommandBufferStagingStream.cpp"
    "${GF_ROOT}/system/vulkan_enc/DescriptorSetVirtualization.cpp"
    "${GF_ROOT}/system/vulkan_enc/HostVisibleMemoryVirtualization.cpp"
    "${GF_ROOT}/system/vulkan_enc/Resources.cpp"
    "${GF_ROOT}/system/vulkan_enc/Validation.cpp"
    "${GF_ROOT}/system/vulkan_enc/VulkanStreamGuest.cpp"
    "${GF_ROOT}/system/vulkan_enc/VulkanHandleMapping.cpp"
    "${GF_ROOT}/system/vulkan_enc/ResourceTracker.cpp"
    "${GF_ROOT}/system/vulkan_enc/VkEncoder.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_extension_structs_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_marshaling_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_reserved_marshaling_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_deepcopy_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_counting_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_handlemap_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/goldfish_vk_transform_guest.cpp"
    "${GF_ROOT}/system/vulkan_enc/func_table.cpp"
)
target_compile_definitions(vulkan_enc PRIVATE
    LOG_TAG="goldfish_vulkan"
    VK_ANDROID_native_buffer
    VK_EXT_device_memory_report
    VK_GOOGLE_gfxstream
    VK_USE_PLATFORM_ANDROID_KHR
    VK_NO_PROTOTYPES
)
target_compile_options(vulkan_enc PRIVATE -fstrict-aliasing)
target_include_directories(vulkan_enc PUBLIC
    "${GF_ROOT}/system/vulkan_enc"
    "${GF_ROOT}/host/include"
    "${GF_ROOT}/host/include/vulkan"
    "${GF_ROOT}/system/OpenglSystemCommon"
    "${GF_ROOT}/system/GLESv1_enc"
    "${GF_ROOT}/system/GLESv2_enc"
    "${GF_ROOT}/system/renderControl_enc"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(vulkan_enc PRIVATE
    OpenglCodecCommon
    _renderControl_enc
    androidemu
    GoldfishAddressSpace
    ${A_LIBCUTILS}
    ${A_LIBUTILS}
    ${A_LIBLOG}
    ${A_LIBSYNC}
    ${A_LIBNATIVEWINDOW}
)
svbox_android_so(vulkan_enc "")

# libOpenglSystemCommon.so
add_library(OpenglSystemCommon SHARED
    "${GF_ROOT}/system/OpenglSystemCommon/FormatConversions.cpp"
    "${GF_ROOT}/system/OpenglSystemCommon/HostConnection.cpp"
    "${GF_ROOT}/system/OpenglSystemCommon/QemuPipeStream.cpp"
    "${GF_ROOT}/system/OpenglSystemCommon/ProcessPipe.cpp"
    "${GF_ROOT}/system/OpenglSystemCommon/ThreadInfo.cpp"
    "${GF_ROOT}/system/OpenglSystemCommon/AddressSpaceStream.cpp"
)
target_compile_options(OpenglSystemCommon PRIVATE -fno-emulated-tls)
target_include_directories(OpenglSystemCommon PUBLIC
    "${GF_ROOT}/system/OpenglSystemCommon"
    "${GF_ROOT}/system/OpenglSystemCommon/bionic-include"
    "${GF_ROOT}/host/include"
    "${GF_ROOT}/host/include/vulkan"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(OpenglSystemCommon PUBLIC
    GLESv1_enc
    GLESv2_enc
    _renderControl_enc
    vulkan_enc
    OpenglCodecCommon
)
target_link_libraries(OpenglSystemCommon PRIVATE
    qemupipe_svbox
    ringbuffer
    GoldfishAddressSpace
    androidemu
    ${A_LIBCUTILS}
    ${A_LIBUTILS}
    ${A_LIBLOG}
)
svbox_android_so(OpenglSystemCommon "")

# libGoldfishProfiler.so (stub: zero perfetto/libbase dependency)
add_library(GoldfishProfiler SHARED
    "${GF_ROOT}/system/profiler/profiler_stub.cpp"
)
target_include_directories(GoldfishProfiler PUBLIC
    "${GF_ROOT}/system/profiler"
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(GoldfishProfiler PRIVATE ${A_LIBLOG})
svbox_android_so(GoldfishProfiler "")

# =============================================================================
# 3. EGL & GLES Drivers (/vendor/lib64/egl/)
# =============================================================================

# libEGL_emulation.so
add_library(EGL_emulation SHARED
    "${GF_ROOT}/system/egl/eglDisplay.cpp"
    "${GF_ROOT}/system/egl/egl.cpp"
    "${GF_ROOT}/system/egl/ClientAPIExts.cpp"
)
target_compile_definitions(EGL_emulation PRIVATE
    LOG_TAG="EGL_emulation"
    WITH_GLES2
)
target_include_directories(EGL_emulation PRIVATE
    "${GF_ROOT}/system/egl"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(EGL_emulation PRIVATE
    OpenglSystemCommon
    GoldfishProfiler
    qemupipe_svbox
    ${A_LIBCUTILS}
    ${A_LIBUTILS}
    ${A_LIBLOG}
    ${A_LIBNATIVEWINDOW}
    ${A_LIBSYNC}
)
svbox_android_so(EGL_emulation "egl")

# libGLESv1_CM_emulation.so
add_library(GLESv1_CM_emulation SHARED
    "${GF_ROOT}/system/GLESv1/gl.cpp"
)
target_compile_definitions(GLESv1_CM_emulation PRIVATE LOG_TAG="GLES_emulation")
target_include_directories(GLESv1_CM_emulation PRIVATE
    "${GF_ROOT}/system/GLESv1"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(GLESv1_CM_emulation PRIVATE
    OpenglSystemCommon
    GLESv1_enc
    _renderControl_enc
    ${A_LIBLOG}
)
svbox_android_so(GLESv1_CM_emulation "egl")

# libGLESv2_emulation.so
add_library(GLESv2_emulation SHARED
    "${GF_ROOT}/system/GLESv2/gl2.cpp"
)
target_compile_definitions(GLESv2_emulation PRIVATE LOG_TAG="GLESv2_emulation")
target_include_directories(GLESv2_emulation PRIVATE
    "${GF_ROOT}/system/GLESv2"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(GLESv2_emulation PRIVATE
    OpenglSystemCommon
    GLESv2_enc
    _renderControl_enc
    ${A_LIBLOG}
)
svbox_android_so(GLESv2_emulation "egl")

# =============================================================================
# 4. Vendor HALs (/vendor/lib64/hw/)
# =============================================================================

# gralloc.ranchu.so (uses gralloc_old.cpp: ashmem + refcount pipe, zero libdrm)
add_library(gralloc.ranchu SHARED
    "${GF_ROOT}/system/gralloc/gralloc_old.cpp"
)
set_target_properties(gralloc.ranchu PROPERTIES PREFIX "")
target_compile_definitions(gralloc.ranchu PRIVATE LOG_TAG="gralloc_ranchu")
target_include_directories(gralloc.ranchu PRIVATE
    "${GF_ROOT}/system/gralloc"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(gralloc.ranchu PRIVATE
    OpenglSystemCommon
    GLESv1_enc
    _renderControl_enc
    qemupipe_svbox
    GoldfishAddressSpace
    ${A_LIBCUTILS}
    ${A_LIBUTILS}
    ${A_LIBLOG}
)
svbox_android_so(gralloc.ranchu "hw")

# hwcomposer.ranchu.so
add_library(hwcomposer.ranchu SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/hwcomposer_ranchu.cpp"
)
set_target_properties(hwcomposer.ranchu PROPERTIES PREFIX "")
target_include_directories(hwcomposer.ranchu PRIVATE
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(hwcomposer.ranchu PRIVATE
    OpenglSystemCommon
    _renderControl_enc
    ${A_LIBCUTILS}
    ${A_LIBLOG}
    ${A_LIBSYNC}
)
svbox_android_so(hwcomposer.ranchu "hw")

# vulkan.ranchu.so
add_library(vulkan.ranchu SHARED
    "${GF_ROOT}/system/vulkan/goldfish_vulkan.cpp"
)
set_target_properties(vulkan.ranchu PROPERTIES PREFIX "")
target_compile_definitions(vulkan.ranchu PRIVATE
    LOG_TAG="goldfish_vulkan"
    VK_USE_PLATFORM_ANDROID_KHR
    VK_NO_PROTOTYPES
)
target_compile_options(vulkan.ranchu PRIVATE -fvisibility=hidden -fstrict-aliasing)
target_include_directories(vulkan.ranchu PRIVATE
    "${GF_ROOT}/system/vulkan"
    "${GF_ROOT}/host/include"
    "${GF_ROOT}/host/include/vulkan"
    ${GF_COMMON_INCLUDES}
    ${AOSP_COMMON_INCLUDES}
)
target_link_libraries(vulkan.ranchu PRIVATE
    OpenglSystemCommon
    vulkan_enc
    _renderControl_enc
    OpenglCodecCommon
    androidemu
    ${A_LIBCUTILS}
    ${A_LIBLOG}
    ${A_LIBSYNC}
    ${A_LIBNATIVEWINDOW}
)
svbox_android_so(vulkan.ranchu "hw")
