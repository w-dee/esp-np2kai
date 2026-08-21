#include "tinyusb_board_p4_nano.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"

static const char *const TAG = "p4-tinyusb-board";
static usb_phy_handle_t s_phy_handle;

bool tinyusb_board_p4_nano_phy_init(void)
{
    if (s_phy_handle != NULL) {
        return true;
    }

    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };

    const esp_err_t err = usb_new_phy(&phy_config, &s_phy_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_new_phy(HS/UTMI, host) failed: %s", esp_err_to_name(err));
        s_phy_handle = NULL;
        return false;
    }

    return true;
}

bool tinyusb_board_p4_nano_phy_deinit(void)
{
    if (s_phy_handle == NULL) {
        return true;
    }

    const esp_err_t err = usb_del_phy(s_phy_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_del_phy failed: %s", esp_err_to_name(err));
        return false;
    }

    s_phy_handle = NULL;
    return true;
}
