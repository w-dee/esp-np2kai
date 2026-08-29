get_filename_component(NP2_AUDIO_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(NP2_AUDIO_FIXTURE
    "${NP2_AUDIO_REPO_ROOT}/testdata/s98/retrofm-pocket-demo-strict.s98")
set(NP2_AUDIO_EMBED_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/retrofm-pocket-demo-strict.s98)
configure_file("${NP2_AUDIO_FIXTURE}" "${NP2_AUDIO_EMBED_FIXTURE}" COPYONLY)

idf_component_register(
    SRCS "p4_nano_audio_i2s_opngen.cpp"
         "${NP2_AUDIO_REPO_ROOT}/host/compat/np2_crc32.c"
         "${NP2_AUDIO_REPO_ROOT}/host/compat/np2_sha256.c"
    INCLUDE_DIRS "include"
                 "${NP2_AUDIO_REPO_ROOT}/host/compat"
    PRIV_REQUIRES np2opngen_fixture p4_nano_board esp_driver_i2s freertos esp_timer
    EMBED_FILES "${NP2_AUDIO_EMBED_FIXTURE}")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
