cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "validation_controller.cpp"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
