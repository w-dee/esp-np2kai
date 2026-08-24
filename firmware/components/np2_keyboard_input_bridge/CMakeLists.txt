cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "keyboard_input_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES np2_keyboard_input freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
    -Wall -Wextra -Werror)
