idf_component_register(
    SRCS "uart_control_transport.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES control_plane control_stream binary_data_plane file_transfer storage_ram esp_driver_uart esp_timer freertos vfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
