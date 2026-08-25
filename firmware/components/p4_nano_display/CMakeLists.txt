idf_component_register(
    SRCS "p4_nano_display.cpp" "p4_nano_display_pattern.cpp"
         "p4_nano_display_transform.cpp" "p4_nano_display_transform_pattern.cpp"
         "p4_nano_jd9365_safe.c"
    INCLUDE_DIRS "include" "include/p4_nano_display"
    REQUIRES p4_nano_board esp_lcd esp_hw_support esp_mm esp_driver_gpio
             esp_driver_i2c esp_psram heap freertos esp_timer esp_lcd_jd9365_10_1)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
    -Wall -Wextra)

if(P4_NANO_RUNTIME_EMU_BACKEND)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        P4_NANO_RUNTIME_EMU_BACKEND=1)
endif()

set(P4_NANO_DISPLAY_TRANSFORM_OPT "debug" CACHE STRING
    "Optimization mode for the P4-NANO transform translation unit")
set_property(CACHE P4_NANO_DISPLAY_TRANSFORM_OPT PROPERTY STRINGS debug o2)
if(P4_NANO_DISPLAY_TRANSFORM_OPT STREQUAL "o2")
    set_source_files_properties(
        p4_nano_display_transform.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
elseif(NOT P4_NANO_DISPLAY_TRANSFORM_OPT STREQUAL "debug")
    message(FATAL_ERROR
        "P4_NANO_DISPLAY_TRANSFORM_OPT must be debug or o2, got: "
        "${P4_NANO_DISPLAY_TRANSFORM_OPT}")
endif()
