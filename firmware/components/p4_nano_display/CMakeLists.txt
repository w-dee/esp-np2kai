idf_component_register(
    SRCS "p4_nano_display.cpp" "p4_nano_display_pattern.cpp"
         "p4_nano_display_transform.cpp" "p4_nano_display_transform_pattern.cpp"
         "p4_nano_jd9365_safe.c"
    INCLUDE_DIRS "include" "include/p4_nano_display"
    REQUIRES p4_nano_board esp_lcd esp_hw_support esp_mm esp_driver_gpio
             esp_driver_i2c esp_psram heap freertos esp_lcd_jd9365_10_1)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra)
