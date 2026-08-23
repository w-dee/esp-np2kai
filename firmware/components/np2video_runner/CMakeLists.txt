cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2VIDEO_RUNNER_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2VIDEO_RUNNER_VENDOR_SRC "${NP2VIDEO_RUNNER_REPO_ROOT}/third_party/np2kai/src")
set(NP2VIDEO_RUNNER_HOST_COMPAT "${NP2VIDEO_RUNNER_REPO_ROOT}/host/compat")
set(NP2VIDEO_RUNNER_HOST_RUNNER "${NP2VIDEO_RUNNER_REPO_ROOT}/host/runner")

set(NP2VIDEO_PROFILE_ACTIVE FALSE)
if(NP2_VIDEO_PROFILE OR P4_NANO_LIVE_DISPLAY_PROFILE OR
   P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE OR
   "$ENV{NP2_VIDEO_PROFILE}" STREQUAL "1" OR
   "$ENV{P4_NANO_LIVE_DISPLAY_PROFILE}" STREQUAL "1" OR
   "$ENV{P4_NANO_LIVE_DISPLAY_BENCHMARK_PROFILE}" STREQUAL "1")
    set(NP2VIDEO_PROFILE_ACTIVE TRUE)
endif()

if(NOT DEFINED NP2VIDEO_GOLDEN_HEADER AND
   DEFINED ENV{NP2VIDEO_GOLDEN_HEADER})
    set(NP2VIDEO_GOLDEN_HEADER "$ENV{NP2VIDEO_GOLDEN_HEADER}")
endif()

set(NP2VIDEO_RUNNER_INCLUDE_DIRS
    "include"
    "${NP2VIDEO_RUNNER_HOST_RUNNER}"
    "${NP2VIDEO_RUNNER_HOST_COMPAT}"
    "${NP2VIDEO_RUNNER_VENDOR_SRC}"
    "${NP2VIDEO_RUNNER_VENDOR_SRC}/i286c")
if(DEFINED NP2VIDEO_GOLDEN_HEADER AND
   EXISTS "${NP2VIDEO_GOLDEN_HEADER}")
    get_filename_component(NP2VIDEO_GOLDEN_HEADER_DIR
                           "${NP2VIDEO_GOLDEN_HEADER}" DIRECTORY)
    list(APPEND NP2VIDEO_RUNNER_INCLUDE_DIRS "${NP2VIDEO_GOLDEN_HEADER_DIR}")
endif()

idf_component_register(
    SRCS
        "np2video_runner.c"
        "${NP2VIDEO_RUNNER_HOST_RUNNER}/video_control_v1.c"
        "${NP2VIDEO_RUNNER_HOST_RUNNER}/stage1_machine_config.c"
    INCLUDE_DIRS ${NP2VIDEO_RUNNER_INCLUDE_DIRS}
    REQUIRES np2core
    PRIV_REQUIRES np2host np2fixture esp_psram esp_hw_support heap
                 esp_timer)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    SUPPORT_16BPP
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO)
if(NP2VIDEO_BENCHMARK_PROFILE OR
   "$ENV{NP2VIDEO_BENCHMARK_PROFILE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        NP2VIDEO_BENCHMARK_PROFILE=1)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Wno-error=parentheses
    -Wno-error=maybe-uninitialized
    -Wno-error=incompatible-pointer-types
    -Wno-error=unused-value
    -Wno-error=unknown-pragmas
    -Wno-error=format
    -Wno-error=address
    -Wno-error=return-type
    -Werror=implicit-function-declaration
    "SHELL:-iquote ${NP2VIDEO_RUNNER_VENDOR_SRC}"
    "SHELL:-iquote ${NP2VIDEO_RUNNER_VENDOR_SRC}/bios"
    "SHELL:-iquote ${NP2VIDEO_RUNNER_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${NP2VIDEO_RUNNER_VENDOR_SRC}/io")
