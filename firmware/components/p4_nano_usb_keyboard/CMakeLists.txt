cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS "producer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES usb hid_boot_keyboard usb_hid_keyboard_adapter
             np2_keyboard_input np2_keyboard_input_bridge freertos
             usb_host_hid
    PRIV_REQUIRES esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra -Werror)
