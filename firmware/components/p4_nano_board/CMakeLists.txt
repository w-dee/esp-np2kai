idf_component_register(
    SRCS "p4_nano_board.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
