idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc esp_driver_sdmmc)
