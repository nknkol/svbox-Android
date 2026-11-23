cmake_minimum_required(VERSION 3.5.0)
project(SvboxAndroid)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Layer 2: decoupled, Android-agnostic HarmonyOS host bridge
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ohos_bridge
    ${CMAKE_CURRENT_BINARY_DIR}/ohos_bridge
)

# Layer 3: thin Android 11 compatibility runtime + ArkTS NAPI binding
add_library(entry SHARED
    napi_init.cpp
    android_runtime.cpp
)

target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../gpu/ohos_host
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../svbox/src/payload/include
)

target_link_libraries(entry PUBLIC
    ohos_bridge
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
    dl
)
