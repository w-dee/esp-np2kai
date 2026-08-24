cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "hid_boot_keyboard.c"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c11
    -Wall -Wextra -Werror)
