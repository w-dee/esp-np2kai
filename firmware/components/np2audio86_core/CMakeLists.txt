set(NP2_AUDIO86_VENDOR_DIR "${CMAKE_SOURCE_DIR}/../third_party/np2kai/src")
set(NP2_AUDIO86_HOST_COMPAT_DIR "${CMAKE_SOURCE_DIR}/../host/compat")

idf_component_register(
    SRCS
        "np2audio86_core.c"
        "np2audio86_transport.c"
        "np2audio86_runtime_transport.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/opngenc.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/opngeng.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/psggenc.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/psggeng.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/pcmmix.c"
        "${NP2_AUDIO86_VENDOR_DIR}/sound/pcm86g.c"
    INCLUDE_DIRS
        "include"
        "${NP2_AUDIO86_HOST_COMPAT_DIR}"
        "${NP2_AUDIO86_VENDOR_DIR}"
        "${NP2_AUDIO86_VENDOR_DIR}/i286c")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND=1
    BYTESEX_LITTLE=1
    OSLANG_UTF8=1
    SUPPORT_UTF8=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-error=format
    -Werror=implicit-function-declaration)

if(P4_NANO_AUDIO86_CAPACITY_PROFILE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()

if(P4_AUDIO86_PROFILE_MODE OR "$ENV{P4_AUDIO86_PROFILE_MODE}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NP2_AUDIO86_PROFILE=1)
endif()
