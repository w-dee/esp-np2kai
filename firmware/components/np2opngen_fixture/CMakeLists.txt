set(NP2_OPNGEN_VENDOR_DIR "${CMAKE_SOURCE_DIR}/../third_party/np2kai/src")
set(NP2_OPNGEN_HOST_COMPAT_DIR "${CMAKE_SOURCE_DIR}/../host/compat")

idf_component_register(
    SRCS
        "np2opngen_fixture.c"
        "np2opngen_synth_event.c"
        "np2opngen_pcm_canonical.c"
        "np2opngen_spsc.c"
        "np2opngen_e1b_stream.c"
        "np2opngen_fixture_esp.c"
        "${NP2_OPNGEN_VENDOR_DIR}/sound/opngenc.c"
        "${NP2_OPNGEN_VENDOR_DIR}/sound/opngeng.c"
        "${NP2_OPNGEN_HOST_COMPAT_DIR}/np2_crc32.c"
        "${NP2_OPNGEN_HOST_COMPAT_DIR}/np2_sha256.c"
    INCLUDE_DIRS
        "include"
        "${NP2_OPNGEN_HOST_COMPAT_DIR}"
        "${NP2_OPNGEN_VENDOR_DIR}"
        "${NP2_OPNGEN_VENDOR_DIR}/i286c"
    PRIV_REQUIRES esp_timer)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DISABLE_SOUND=1
    BYTESEX_LITTLE=1
    OSLANG_UTF8=1
    SUPPORT_UTF8=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -Wall
    -Wextra
    -Werror=implicit-function-declaration)
