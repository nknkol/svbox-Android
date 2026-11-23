cmake_minimum_required(VERSION 3.5.0)
project(OhosBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OHOS_BRIDGE_HEADLESS "Build without XComponent/OHNativeWindow HAP-only libraries (for standalone CLI tools)" OFF)

add_library(ohos_bridge STATIC
    src/sched_qos.cpp
    src/input_bridge.cpp
    src/audio_bridge.cpp
    src/usb_bridge.cpp
    src/surface_bridge.cpp
    src/gpu_bridge.cpp
    src/container_runner.cpp
)

target_include_directories(ohos_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gpu/ohos_host
)

if(OHOS_BRIDGE_HEADLESS)
    target_compile_definitions(ohos_bridge PUBLIC OHOS_BRIDGE_HEADLESS=1)
    target_link_libraries(ohos_bridge PUBLIC pthread dl)
else()
    target_link_libraries(ohos_bridge PUBLIC
        libnative_window.so
        libnative_vsync.so
        libohaudio.so
        libohhidebug.so
        libhilog_ndk.z.so
        pthread
        dl
    )
endif()
