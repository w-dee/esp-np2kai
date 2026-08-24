cmake_minimum_required(VERSION 3.16)

get_filename_component(P4_NANO_LIVE_SESSION_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)

idf_component_register(
    SRCS "p4_nano_live_display_session.cpp"
    INCLUDE_DIRS "include"
                 "${P4_NANO_LIVE_SESSION_REPO_ROOT}/host/compat"
                 "${P4_NANO_LIVE_SESSION_REPO_ROOT}/host/presentation"
                 "${P4_NANO_LIVE_SESSION_REPO_ROOT}/third_party/np2kai/src"
                 "${P4_NANO_LIVE_SESSION_REPO_ROOT}/third_party/np2kai/src/i286c"
    REQUIRES p4_nano_display
    PRIV_REQUIRES p4_nano_board np2presentation
                 esp_psram esp_hw_support heap freertos esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra -Werror)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SUPPORT_16BPP
    BYTESEX_LITTLE)

if(P4_NANO_RUNTIME_EMU_BACKEND)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_RUNTIME_EMU_BACKEND=1)
endif()
