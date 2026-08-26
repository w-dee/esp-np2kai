idf_component_register(
    SRCS "p4_nano_pie_preemption.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES freertos esp_timer heap esp_hw_support)

# Keep this correctness profile independent of the live display/DSI/PPA
# component while reusing the accepted scalar and assembly implementations.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../p4_nano_display/p4_nano_display_exact2x.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../p4_nano_display/p4_nano_display_exact2x_pie.S")
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../p4_nano_display/include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
