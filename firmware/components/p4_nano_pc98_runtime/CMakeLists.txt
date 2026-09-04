cmake_minimum_required(VERSION 3.16)

get_filename_component(P4_NANO_PC98_RUNTIME_REPO_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(P4_NANO_PC98_RUNTIME_VENDOR_SRC
    "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/third_party/np2kai/src")
set(P4_NANO_PC98_RUNTIME_SRCS "p4_nano_pc98_runtime.cpp")
if(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    list(APPEND P4_NANO_PC98_RUNTIME_SRCS
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/runner/result_v1_parser.c"
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/compat/np2_crc32.c")
endif()
if(P4_NANO_KEYBOARD_VALIDATION_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    list(APPEND P4_NANO_PC98_RUNTIME_SRCS
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/runner/np2kbd_control_v1_parser.c"
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/runner/np2kbd_result_v1_parser.c"
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/compat/np2_crc32.c"
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/firmware/components/np2_keyboard_validation/validation_controller.cpp")
endif()
set(P4_NANO_PC98_RUNTIME_REQUIRES
    np2runtime np2host p4_nano_live_display_session np2_keyboard_input_bridge
    storage_sdmmc storage_fatfs freertos esp_timer
    # This component has no profile-off call site into the binding.  Keeping
    # the dependency declaration unconditional gives ESP-IDF a stable static
    # archive order when the opt-in profile enables that call site.
    p4_nano_audio86_guest_binding)
if(P4_NANO_REAL_RUNTIME_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    list(APPEND P4_NANO_PC98_RUNTIME_REQUIRES p4_nano_usb_keyboard)
endif()
set(P4_NANO_PC98_RUNTIME_INCLUDE_DIRS
    "include"
    "${P4_NANO_PC98_RUNTIME_VENDOR_SRC}"
    "${P4_NANO_PC98_RUNTIME_VENDOR_SRC}/i286c"
    "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/compat"
    "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/host/runner")
if(P4_NANO_KEYBOARD_VALIDATION_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    list(APPEND P4_NANO_PC98_RUNTIME_INCLUDE_DIRS
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/firmware/components/np2_keyboard_validation/include")
endif()
if(P4_NANO_REAL_RUNTIME_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    list(APPEND P4_NANO_PC98_RUNTIME_INCLUDE_DIRS
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/firmware/components/p4_nano_usb_keyboard/include")
endif()
if(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
    # ESP-IDF resolves this profile-owned component from main first.  Make the
    # binding's public interface explicit for this pre-existing runtime
    # component as well, rather than relying on recursive discovery order.
    list(APPEND P4_NANO_PC98_RUNTIME_INCLUDE_DIRS
         "${P4_NANO_PC98_RUNTIME_REPO_ROOT}/firmware/components/p4_nano_audio86_guest_binding/include")
endif()
idf_component_register(
    SRCS ${P4_NANO_PC98_RUNTIME_SRCS}
    INCLUDE_DIRS ${P4_NANO_PC98_RUNTIME_INCLUDE_DIRS}
    PRIV_REQUIRES ${P4_NANO_PC98_RUNTIME_REQUIRES})

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
if(P4_NANO_REAL_RUNTIME_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::p4_nano_usb_keyboard)
endif()
if(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_RUNTIME_VALIDATION_PROFILE=1)
endif()
if(P4_NANO_REAL_RUNTIME_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_REAL_RUNTIME_PROFILE=1)
endif()
if(P4_NANO_KEYBOARD_VALIDATION_PROFILE OR
   P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_KEYBOARD_VALIDATION_PROFILE=1)
endif()
if(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE=1)
endif()
if(P4_NANO_RUNTIME_EMU_BACKEND)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_RUNTIME_EMU_BACKEND=1)
endif()
if(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_REAL_GUEST_PROFILE=1 NP2_ASYNC_AUDIO86=1)
endif()
if(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_SUSTAINED_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_SUSTAINED_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_OUTER_TIMEOUT_TEST)
    if(NOT NP2_EMU_TEST OR NOT P4_NANO_AUDIO86_SUSTAINED_PROFILE)
        message(FATAL_ERROR
            "Audio86 outer-timeout injection is ESP-EMU sustained-test only")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_OUTER_TIMEOUT_TEST=1)
endif()
