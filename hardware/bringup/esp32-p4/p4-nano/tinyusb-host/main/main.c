#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tusb.h"
#include "class/hid/hid_host.h"
#include "hid_boot_keyboard.h"
#include "tinyusb_board_p4_nano.h"

#define TINYUSB_RHPORT 1U
#define EXPECTED_VID 0x0853U
#define EXPECTED_PID 0x0103U

typedef struct {
    hid_boot_keyboard_event_kind_t kind;
    uint8_t usage;
} expected_event_t;

static const char *const TAG = "p4-tinyusb-host";

static const expected_event_t s_direct_sequence[] = {
    {HID_BOOT_EVENT_KEY_PRESS, 0x04},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe1},
    {HID_BOOT_EVENT_KEY_PRESS, 0x04},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe1},
    {HID_BOOT_EVENT_KEY_PRESS, 0x28},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x28},
    {HID_BOOT_EVENT_KEY_PRESS, 0x52},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x52},
    {HID_BOOT_EVENT_KEY_PRESS, 0x51},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x51},
    {HID_BOOT_EVENT_KEY_PRESS, 0x50},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x50},
    {HID_BOOT_EVENT_KEY_PRESS, 0x4f},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x4f},
};

static const expected_event_t s_hub_sequence[] = {
    {HID_BOOT_EVENT_KEY_PRESS, 0x04},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04},
    {HID_BOOT_EVENT_KEY_PRESS, 0x1e},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x1e},
    {HID_BOOT_EVENT_KEY_PRESS, 0x2c},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x2c},
    {HID_BOOT_EVENT_KEY_PRESS, 0x28},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x28},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe1},
    {HID_BOOT_EVENT_KEY_PRESS, 0x04},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe1},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe0},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe0},
    {HID_BOOT_EVENT_MOD_PRESS, 0xe2},
    {HID_BOOT_EVENT_MOD_RELEASE, 0xe2},
    {HID_BOOT_EVENT_KEY_PRESS, 0x3a},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x3a},
    {HID_BOOT_EVENT_KEY_PRESS, 0x52},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x52},
    {HID_BOOT_EVENT_KEY_PRESS, 0x51},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x51},
    {HID_BOOT_EVENT_KEY_PRESS, 0x50},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x50},
    {HID_BOOT_EVENT_KEY_PRESS, 0x4f},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x4f},
};

static hid_boot_keyboard_state_t s_keyboard_state;
static const expected_event_t *s_expected_sequence;
static size_t s_expected_sequence_length;
static size_t s_expected_sequence_index;
static bool s_hid_ready;
static bool s_cleanup_requested;
static bool s_sequence_pass_reported;

static const char *speed_name(tusb_speed_t speed)
{
    switch (speed) {
    case TUSB_SPEED_LOW:
        return "LS";
    case TUSB_SPEED_FULL:
        return "FS";
    case TUSB_SPEED_HIGH:
        return "HS";
    default:
        return "UNKNOWN";
    }
}

static void log_device_bus(uint8_t daddr, const char *prefix)
{
    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_bus_info_t bus_info = {0};
    (void)tuh_vid_pid_get(daddr, &vid, &pid);
    (void)tuh_bus_info_get(daddr, &bus_info);

    ESP_LOGI(TAG,
             "P4-NANO TINYUSB %s: addr=%u VIDPID=%04x:%04x SPEED=%s RHPORT=%u PARENT=%u:%u",
             prefix, daddr, vid, pid, speed_name((tusb_speed_t)bus_info.speed),
             bus_info.rhport, bus_info.hub_addr, bus_info.hub_port);
    ESP_LOGI(TAG, "P4-NANO TINYUSB VIDPID: %04x:%04x", vid, pid);
    ESP_LOGI(TAG, "P4-NANO TINYUSB SPEED: %s", speed_name((tusb_speed_t)bus_info.speed));
    ESP_LOGI(TAG, "P4-NANO TINYUSB PARENT: %s addr=%u port=%u",
             bus_info.hub_addr == 0 ? "root" : "hub",
             bus_info.hub_addr, bus_info.hub_port);
}

static bool event_matches(const expected_event_t *expected,
                          const hid_boot_keyboard_event_t *event)
{
    return expected->kind == event->kind && expected->usage == event->usage;
}

static void observe_event(const hid_boot_keyboard_event_t *event)
{
    switch (event->kind) {
    case HID_BOOT_EVENT_KEY_PRESS:
        ESP_LOGI(TAG, "P4-NANO TINYUSB PRESS usage=0x%02x", event->usage);
        break;
    case HID_BOOT_EVENT_KEY_RELEASE:
        ESP_LOGI(TAG, "P4-NANO TINYUSB RELEASE usage=0x%02x", event->usage);
        break;
    case HID_BOOT_EVENT_MOD_PRESS:
        ESP_LOGI(TAG, "P4-NANO TINYUSB MOD PRESS usage=0x%02x", event->usage);
        break;
    case HID_BOOT_EVENT_MOD_RELEASE:
        ESP_LOGI(TAG, "P4-NANO TINYUSB MOD RELEASE usage=0x%02x", event->usage);
        break;
    case HID_BOOT_EVENT_ERROR_USAGE:
        ESP_LOGE(TAG, "P4-NANO TINYUSB ERROR USAGE usage=0x%02x", event->usage);
        break;
    default:
        break;
    }

    if (s_expected_sequence == NULL || s_sequence_pass_reported) {
        return;
    }

    if (event_matches(&s_expected_sequence[s_expected_sequence_index], event)) {
        ++s_expected_sequence_index;
    } else if (event_matches(&s_expected_sequence[0], event)) {
        s_expected_sequence_index = 1;
    } else {
        s_expected_sequence_index = 0;
    }

    if (s_expected_sequence_index == s_expected_sequence_length) {
        s_sequence_pass_reported = true;
        s_cleanup_requested = true;
        ESP_LOGI(TAG, "P4-NANO TINYUSB %s RESULT: PASS (software sequence)",
                 s_expected_sequence == s_hub_sequence ? "HUB FS" : "DIRECT");
    }
}

void tuh_mount_cb(uint8_t daddr)
{
    log_device_bus(daddr, "DEVICE MOUNT");
}

void tuh_umount_cb(uint8_t daddr)
{
    ESP_LOGI(TAG, "P4-NANO TINYUSB DEVICE UMOUNT addr=%u", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx,
                      const uint8_t *report_desc, uint16_t desc_len)
{
    (void)report_desc;
    (void)desc_len;

    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_bus_info_t bus_info = {0};
    (void)tuh_vid_pid_get(daddr, &vid, &pid);
    (void)tuh_bus_info_get(daddr, &bus_info);

    const uint8_t interface_protocol = tuh_hid_interface_protocol(daddr, idx);
    const uint8_t protocol = tuh_hid_get_protocol(daddr, idx);
    const bool via_hub = bus_info.hub_addr != 0;
    const bool known_keyboard = vid == EXPECTED_VID && pid == EXPECTED_PID;
    const bool full_speed = bus_info.speed == TUSB_SPEED_FULL;
    const bool boot_keyboard = interface_protocol == HID_ITF_PROTOCOL_KEYBOARD &&
                               protocol == HID_PROTOCOL_BOOT;

    ESP_LOGI(TAG,
             "P4-NANO TINYUSB HID MOUNT addr=%u idx=%u VIDPID=%04x:%04x SPEED=%s PARENT=%u:%u ITF=%u PROTOCOL=%u",
             daddr, idx, vid, pid, speed_name((tusb_speed_t)bus_info.speed),
             bus_info.hub_addr, bus_info.hub_port, interface_protocol, protocol);

    if (via_hub) {
        ESP_LOGI(TAG, "P4-NANO TINYUSB HUB: PASS child=%u parent=%u port=%u speed=%s",
                 daddr, bus_info.hub_addr, bus_info.hub_port,
                 speed_name((tusb_speed_t)bus_info.speed));
    }

    if (!known_keyboard || !full_speed || !boot_keyboard) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HID: FAIL expected=0853:0103 FS Boot keyboard");
        return;
    }

    s_hid_ready = true;
    hid_boot_keyboard_init(&s_keyboard_state);
    s_expected_sequence = via_hub ? s_hub_sequence : s_direct_sequence;
    s_expected_sequence_length = via_hub
        ? sizeof(s_hub_sequence) / sizeof(s_hub_sequence[0])
        : sizeof(s_direct_sequence) / sizeof(s_direct_sequence[0]);
    s_expected_sequence_index = 0;
    s_sequence_pass_reported = false;
    ESP_LOGI(TAG, "P4-NANO TINYUSB HID: PASS");
    ESP_LOGI(TAG, "P4-NANO TINYUSB HID READY mode=%s", via_hub ? "HUB-FS" : "DIRECT-FS");
    if (via_hub) {
        ESP_LOGI(TAG, "P4-NANO TINYUSB HUB + HID READY FOR KEY TEST");
    }

    if (!tuh_hid_receive_report(daddr, idx)) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HID RECEIVE: FAIL addr=%u idx=%u", daddr, idx);
        s_hid_ready = false;
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx)
{
    ESP_LOGI(TAG, "P4-NANO TINYUSB HID UMOUNT addr=%u idx=%u", daddr, idx);
    if (s_expected_sequence != NULL && !s_sequence_pass_reported) {
        ESP_LOGI(TAG, "P4-NANO TINYUSB %s RESULT: FAIL",
                 s_expected_sequence == s_hub_sequence ? "HUB FS" : "DIRECT");
    }
    s_hid_ready = false;
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                const uint8_t *report, uint16_t len)
{
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    const size_t event_count = hid_boot_keyboard_process(
        &s_keyboard_state, report, len, events, HID_BOOT_KEYBOARD_MAX_EVENTS);

    for (size_t i = 0; i < event_count && i < HID_BOOT_KEYBOARD_MAX_EVENTS; ++i) {
        observe_event(&events[i]);
    }

    if (s_hid_ready && !s_cleanup_requested &&
        !tuh_hid_receive_report(daddr, idx)) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HID RECEIVE: FAIL addr=%u idx=%u", daddr, idx);
        s_hid_ready = false;
    }
}

static void tinyusb_host_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "P4-NANO TINYUSB HOST START");
    ESP_LOGI(TAG, "P4-NANO TINYUSB VERSION: 0.21.0");
    ESP_LOGI(TAG, "P4-NANO TINYUSB RHPORT: %u", TINYUSB_RHPORT);
    ESP_LOGI(TAG, "P4-NANO TINYUSB PHY: HS/UTMI");
    ESP_LOGI(TAG, "P4-NANO TINYUSB ROOT MODE: FS");
    ESP_LOGI(TAG, "P4-NANO TINYUSB SPLIT: DISABLED root-speed=FS");

    if (!tinyusb_board_p4_nano_phy_init()) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB PHY INIT: FAIL");
        vTaskDelete(NULL);
        return;
    }

    const tuh_configure_param_t configure = {
        .dwc2 = {
            .use_hs_phy = true,
        },
    };
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };

    if (!tuh_configure(TINYUSB_RHPORT, TUH_CFGID_DWC2, &configure) ||
        !tuh_rhport_init(TINYUSB_RHPORT, &rh_init)) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HOST INIT: FAIL");
        (void)tinyusb_board_p4_nano_phy_deinit();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "P4-NANO TINYUSB HOST INIT: PASS");

    for (;;) {
        tuh_task();

        if (s_cleanup_requested) {
            const bool host_cleanup = tuh_deinit(TINYUSB_RHPORT);
            const bool phy_cleanup = tinyusb_board_p4_nano_phy_deinit();
            ESP_LOGI(TAG, "P4-NANO TINYUSB CLEANUP RESULT: %s",
                     host_cleanup && phy_cleanup ? "PASS" : "FAIL");
            vTaskDelete(NULL);
            return;
        }
    }
}

void app_main(void)
{
    xTaskCreate(tinyusb_host_task, "tinyusb_host", 6144, NULL, 5, NULL);
}
