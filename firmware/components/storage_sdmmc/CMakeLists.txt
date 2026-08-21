idf_component_register(
    SRCS "storage_sdmmc.cpp"
    INCLUDE_DIRS "include"
    REQUIRES storage_fatfs fatfs sdmmc esp_driver_sdmmc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
