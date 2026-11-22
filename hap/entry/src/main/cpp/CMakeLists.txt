cmake_minimum_required(VERSION 3.5.0)
project(SvboxAndroid)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(entry SHARED
    napi_init.cpp
    hwc_renderer.cpp
    input_bridge.cpp
    audio_bridge.cpp
    sched_qos.cpp
    android_runtime.cpp
)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_vsync.so
    libohaudio.so
    libohhidebug.so
    libhilog_ndk.z.so
    dl
)
