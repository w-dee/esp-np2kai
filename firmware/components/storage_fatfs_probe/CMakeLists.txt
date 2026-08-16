idf_component_register(
    SRCS "storage_fatfs_probe.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage_fatfs mbedtls
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
