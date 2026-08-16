idf_component_register(
    SRCS "np2_memory_probe.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_psram esp_hw_support heap np2core)
