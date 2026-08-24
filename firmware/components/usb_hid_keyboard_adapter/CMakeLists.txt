cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "adapter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hid_boot_keyboard np2_keyboard_input)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
    -Wall -Wextra -Werror)
