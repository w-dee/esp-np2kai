idf_component_register(
    SRCS "p4_nano_audio86_physical_sink.c"
         "p4_nano_audio86_physical_sink_idf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES np2pcm_output np2opngen_fixture p4_nano_board
             esp_driver_i2s esp_timer freertos heap)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>)
