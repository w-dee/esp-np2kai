cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2RUNTIME_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2RUNTIME_VENDOR_SRC "${NP2RUNTIME_REPO_ROOT}/third_party/np2kai/src")
set(NP2RUNTIME_HOST_COMPAT "${NP2RUNTIME_REPO_ROOT}/host/compat")

idf_component_register(
    SRCS
        "np2runtime.cpp"
        "np2runtime_lifecycle.cpp"
        "np2runtime_machine_config.cpp"
    INCLUDE_DIRS
        "include"
        "${NP2RUNTIME_HOST_COMPAT}"
        "${NP2RUNTIME_VENDOR_SRC}"
        "${NP2RUNTIME_VENDOR_SRC}/i286c"
    PRIV_REQUIRES np2core np2host)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND
    SUPPORT_KAI_IMAGES
    SUPPORT_FAST_MEMORYCHECK
    SUPPORT_16BPP
    BYTESEX_LITTLE
    OSLANG_UTF8
    SUPPORT_UTF8
    NP2_FIRMWARE_DOSIO)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
    "SHELL:-iquote ${NP2RUNTIME_VENDOR_SRC}"
    "SHELL:-iquote ${NP2RUNTIME_VENDOR_SRC}/bios"
    "SHELL:-iquote ${NP2RUNTIME_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${NP2RUNTIME_VENDOR_SRC}/io")
