# CMake toolchain file for cross-compiling Android 11 (API 30) ARM64 guest libraries
# on HarmonyOS PC using OHOS Clang + Android NDK r25c sysroot.

set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 30)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED OHOS_CLT_DIR)
    if(DEFINED ENV{OHOS_CLT_DIR})
        set(OHOS_CLT_DIR "$ENV{OHOS_CLT_DIR}")
    else()
        set(OHOS_CLT_DIR "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0")
    endif()
endif()

if(NOT DEFINED ANDROID_NDK_SYSROOT_ROOT)
    get_filename_component(_THIS_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
    get_filename_component(_GPU_DIR "${_THIS_DIR}" DIRECTORY)
    set(ANDROID_NDK_SYSROOT_ROOT "${_GPU_DIR}/ndk_sysroot")
endif()

set(_LLVM_BIN "${OHOS_CLT_DIR}/sdk/default/openharmony/native/llvm/bin")
set(_SYSROOT "${ANDROID_NDK_SYSROOT_ROOT}/sysroot")
set(_RESDIR "${ANDROID_NDK_SYSROOT_ROOT}/clang_res/14.0.7")

set(CMAKE_C_COMPILER "${_LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${_LLVM_BIN}/clang++")
set(CMAKE_AR "${_LLVM_BIN}/llvm-ar")
set(CMAKE_RANLIB "${_LLVM_BIN}/llvm-ranlib")
set(CMAKE_STRIP "${_LLVM_BIN}/llvm-strip")

set(CMAKE_SYSROOT "${_SYSROOT}")
set(CMAKE_C_COMPILER_TARGET aarch64-linux-android30)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-android30)

set(_COMMON_FLAGS "--target=aarch64-linux-android30 --sysroot=${_SYSROOT} -resource-dir=${_RESDIR} -fPIC")
set(CMAKE_C_FLAGS_INIT "${_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_COMMON_FLAGS} -stdlib=libc++")

set(_LINK_FLAGS "--target=aarch64-linux-android30 --sysroot=${_SYSROOT} -resource-dir=${_RESDIR} -fuse-ld=lld -L${_SYSROOT}/usr/lib/aarch64-linux-android/30 -L${_SYSROOT}/usr/lib/aarch64-linux-android -Wl,--build-id=sha1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_LINK_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_LINK_FLAGS}")

set(CMAKE_FIND_ROOT_PATH "${_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
