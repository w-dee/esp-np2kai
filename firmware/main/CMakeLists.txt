idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES uart_control_transport esp_app_format)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
