cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2TEST_RUNNER_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2TEST_RUNNER_VENDOR_SRC "${NP2TEST_RUNNER_REPO_ROOT}/third_party/np2kai/src")
set(NP2TEST_RUNNER_HOST_COMPAT "${NP2TEST_RUNNER_REPO_ROOT}/host/compat")
set(NP2TEST_RUNNER_HOST_RUNNER "${NP2TEST_RUNNER_REPO_ROOT}/host/runner")

idf_component_register(
    SRCS
        "np2test_runner.c"
        "${NP2TEST_RUNNER_HOST_RUNNER}/result_v1_parser.c"
        "${NP2TEST_RUNNER_HOST_RUNNER}/execution_controller.c"
        "${NP2TEST_RUNNER_HOST_RUNNER}/stage1_machine_config.c"
    INCLUDE_DIRS
        "include"
        "${NP2TEST_RUNNER_HOST_RUNNER}"
        "${NP2TEST_RUNNER_HOST_COMPAT}"
        "${NP2TEST_RUNNER_VENDOR_SRC}"
        "${NP2TEST_RUNNER_VENDOR_SRC}/i286c"
    PRIV_REQUIRES np2core np2host np2fixture esp_psram esp_hw_support heap)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    SUPPORT_16BPP
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO)

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
    "SHELL:-iquote ${NP2TEST_RUNNER_VENDOR_SRC}"
    "SHELL:-iquote ${NP2TEST_RUNNER_VENDOR_SRC}/bios"
    "SHELL:-iquote ${NP2TEST_RUNNER_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${NP2TEST_RUNNER_VENDOR_SRC}/io")
