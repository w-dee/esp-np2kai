cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "np2presentation_probe.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES np2presentation np2host freertos esp_psram esp_hw_support heap)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BYTESEX_LITTLE)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration)
