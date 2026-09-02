cmake_minimum_required(VERSION 3.16)

get_filename_component(P4_NANO_AUDIO86_BINDING_ROOT
                       "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
set(P4_NANO_AUDIO86_VENDOR_SRC
    "${P4_NANO_AUDIO86_BINDING_ROOT}/third_party/np2kai/src")

set(P4_NANO_AUDIO86_BINDING_REQUIRES
    np2core np2host np2runtime np2audio86_fixture np2audio86_guest_adapter np2opngen_fixture
    np2pcm_output p4_nano_audio86_notifications
    p4_nano_audio86_physical_sink freertos esp_system heap)

idf_component_register(
    SRCS "p4_nano_audio86_guest_binding.cpp"
    INCLUDE_DIRS "include" "${P4_NANO_AUDIO86_VENDOR_SRC}"
                 "${P4_NANO_AUDIO86_VENDOR_SRC}/i286c"
                 "${P4_NANO_AUDIO86_BINDING_ROOT}/host/compat"
    REQUIRES ${P4_NANO_AUDIO86_BINDING_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror
    "SHELL:-iquote ${P4_NANO_AUDIO86_VENDOR_SRC}"
    "SHELL:-iquote ${P4_NANO_AUDIO86_VENDOR_SRC}/cbus"
    "SHELL:-iquote ${P4_NANO_AUDIO86_VENDOR_SRC}/io")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND BYTESEX_LITTLE NP2_ASYNC_AUDIO86=1)
if(P4_NANO_AUDIO86_PRESSURE_SCENARIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PRESSURE_SCENARIO=${P4_NANO_AUDIO86_PRESSURE_SCENARIO})
endif()
if(P4_NANO_AUDIO86_FAILURE_KIND)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_FAILURE_KIND=${P4_NANO_AUDIO86_FAILURE_KIND})
endif()
if(P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE=1)
endif()
if(P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO=${P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO})
endif()
