idf_component_register(
    SRCS "main.c" "hid_boot_keyboard.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES usb esp_driver_gpio usb_host_hid
)
