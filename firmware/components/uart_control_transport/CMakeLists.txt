idf_component_register(
    SRCS "uart_control_transport.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES control_plane esp_driver_uart freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
