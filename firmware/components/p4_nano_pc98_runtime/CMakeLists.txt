cmake_minimum_required(VERSION 3.16)

get_filename_component(P4_NANO_PC98_RUNTIME_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(P4_NANO_PC98_RUNTIME_VENDOR_SRC
    "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/third_party/np2kai/src")

idf_component_register(
    SRCS "p4_nano_pc98_runtime.cpp"
    INCLUDE_DIRS "include"
                 "${P4_NANO_PC98_RUNTIME_VENDOR_SRC}"
                 "${P4_NANO_PC98_RUNTIME_VENDOR_SRC}/i286c"
                 "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/compat"
    PRIV_REQUIRES np2runtime np2host p4_nano_live_display_session
                 storage_sdmmc storage_fatfs freertos esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra -Werror
    "SHELL:-iquote ${P4_NANO_PC98_RUNTIME_VENDOR_SRC}"
    "SHELL:-iquote ${P4_NANO_PC98_RUNTIME_VENDOR_SRC}/bios"
    "SHELL:-iquote ${P4_NANO_PC98_RUNTIME_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${P4_NANO_PC98_RUNTIME_VENDOR_SRC}/io")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND SUPPORT_KAI_IMAGES SUPPORT_FAST_MEMORYCHECK SUPPORT_16BPP
    BYTESEX_LITTLE OSLANG_UTF8 SUPPORT_UTF8 NP2_FIRMWARE_DOSIO)
