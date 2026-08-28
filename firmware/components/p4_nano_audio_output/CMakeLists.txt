set(NP2_AUDIO_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS "p4_nano_audio_output.cpp"
         "${NP2_AUDIO_REPO_ROOT}/host/compat/np2_crc32.c"
         "${NP2_AUDIO_REPO_ROOT}/host/compat/np2_sha256.c"
    INCLUDE_DIRS "include"
                 "${NP2_AUDIO_REPO_ROOT}/host/compat"
    PRIV_REQUIRES p4_nano_board esp_driver_i2s esp_timer freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
