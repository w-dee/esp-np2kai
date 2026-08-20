idf_component_register(
    SRCS "main.c" "display_patterns.c" "p4_nano_jd9365_safe.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_hw_support esp_mm esp_lcd_jd9365_10_1
)
