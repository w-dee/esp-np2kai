cmake_minimum_required(VERSION 3.16)

get_filename_component(NP2_KEYBOARD_BRIDGE_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(NP2_KEYBOARD_BRIDGE_VENDOR_SRC
    "${NP2_KEYBOARD_BRIDGE_REPO_ROOT}/third_party/np2kai/src")

idf_component_register(
    SRCS "keyboard_input_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES np2_keyboard_input
    PRIV_REQUIRES np2core freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
    -Wall -Wextra -Werror
    "SHELL:-iquote ${NP2_KEYBOARD_BRIDGE_VENDOR_SRC}"
    "SHELL:-iquote ${NP2_KEYBOARD_BRIDGE_VENDOR_SRC}/io"
    "SHELL:-iquote ${NP2_KEYBOARD_BRIDGE_VENDOR_SRC}/common")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND SUPPORT_KAI_IMAGES SUPPORT_FAST_MEMORYCHECK SUPPORT_16BPP
    BYTESEX_LITTLE OSLANG_UTF8 SUPPORT_UTF8 NP2_FIRMWARE_DOSIO)
