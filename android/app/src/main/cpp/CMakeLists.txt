cmake_minimum_required(VERSION 3.22)
project(saintsrm_smoke LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "SaintsRM Android smoke target requires the Android NDK")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "SaintsRM smoke target supports arm64-v8a only; got ${CMAKE_ANDROID_ARCH_ABI}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(saintsrm_smoke SHARED
    native_smoke.cpp
    vulkan_report.cpp
)

target_compile_options(saintsrm_smoke PRIVATE
    -Wall -Wextra -Wpedantic
    -fno-omit-frame-pointer
)

target_link_options(saintsrm_smoke PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)

target_link_libraries(saintsrm_smoke PRIVATE
    android
    log
    vulkan
)
