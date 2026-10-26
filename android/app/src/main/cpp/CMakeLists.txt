cmake_minimum_required(VERSION 3.22)
project(saintsrm_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(saintsrm_smoke SHARED native_smoke.cpp)

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
