cmake_minimum_required(VERSION 3.5.0)
project(SvboxAndroid)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(entry SHARED
    napi_init.cpp
    hwc_renderer.cpp
    gfxstream_bridge.cpp
    input_bridge.cpp
    audio_bridge.cpp
    sched_qos.cpp
    android_runtime.cpp
)

target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../gpu/ohos_host
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../svbox/src/payload/include
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
