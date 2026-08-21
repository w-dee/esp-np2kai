set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../")
set(TINYUSB_SRC "${REPO_ROOT}/third_party/tinyusb/src")
set(STOCK_USB_HOST_MAIN "${REPO_ROOT}/hardware/bringup/esp32-p4/p4-nano/usb-host/main")

set(TINYUSB_HOST_SRCS
    "${TINYUSB_SRC}/tusb.c"
    "${TINYUSB_SRC}/common/tusb_fifo.c"
    "${TINYUSB_SRC}/host/usbh.c"
    "${TINYUSB_SRC}/host/hub.c"
    "${TINYUSB_SRC}/class/hid/hid_host.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/dwc2_common.c"
    "${TINYUSB_SRC}/portable/synopsys/dwc2/hcd_dwc2.c"
)

idf_component_register(
    SRCS
        "main.c"
        "tinyusb_board_p4_nano.c"
        "${STOCK_USB_HOST_MAIN}/hid_boot_keyboard.c"
        ${TINYUSB_HOST_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${TINYUSB_SRC}" "${STOCK_USB_HOST_MAIN}"
    PRIV_REQUIRES usb
)
