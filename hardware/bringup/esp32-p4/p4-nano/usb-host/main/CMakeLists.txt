set(usb_host_app_srcs "main.c")
if(CONFIG_USB_HOST_HUBS_SUPPORTED)
    set(usb_host_app_srcs "stage2_main.c")
endif()

idf_component_register(
    SRCS ${usb_host_app_srcs} "hid_boot_keyboard.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES usb esp_driver_gpio usb_host_hid
)
