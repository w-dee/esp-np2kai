#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tusb.h"
#include "class/hid/hid_host.h"
#include "diagnostic_profile.h"
#include "hid_boot_keyboard.h"
#include "tinyusb_board_p4_nano.h"

#define TINYUSB_RHPORT 1U
#define EXPECTED_VID 0x0853U
#define EXPECTED_PID 0x0103U
#define INVALID_HID_INDEX 0xffU
#define INITIAL_TIMEOUT_MS 20000U
#define HOTPLUG_TIMEOUT_MS 30000U
#define HOTPLUG_HEARTBEAT_MS 1000U
#define STABLE_TIMEOUT_MS 2000U
#define REINIT_ROUNDS 3U

typedef struct {
    hid_boot_keyboard_event_kind_t kind;
    uint8_t usage;
} expected_event_t;

typedef enum {
    TEST_STAGE_WAIT_INITIAL_HID,
    TEST_STAGE_STABLE,
    TEST_STAGE_WAIT_UNMOUNT,
    TEST_STAGE_WAIT_REENUM,
    TEST_STAGE_WAIT_RECONNECTED_A,
    TEST_STAGE_CLEANUP,
} test_stage_t;

static const char *const TAG = "p4-tinyusb-host";

static const expected_event_t s_direct_sequence[] __attribute__((unused)) = {
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

static const expected_event_t s_hub_sequence[] __attribute__((unused)) = {
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

static const expected_event_t s_raw_a_sequence[] __attribute__((unused)) = {
    {HID_BOOT_EVENT_KEY_PRESS, 0x04},
    {HID_BOOT_EVENT_KEY_RELEASE, 0x04},
};

static hid_boot_keyboard_state_t s_keyboard_state;
static const expected_event_t *s_expected_sequence;
static size_t s_expected_sequence_length;
static size_t s_expected_sequence_index;
static bool s_hid_ready;
static bool s_cleanup_requested;
static bool s_sequence_pass_reported;

static uint8_t s_active_hid_daddr;
static uint8_t s_active_hid_idx;
static uint32_t s_hid_generation;
static uint32_t s_hid_mount_count;
static uint8_t s_last_umount_daddr;
static uint8_t s_last_umount_idx;
static uint32_t s_last_umount_generation;
static bool s_stale_callback;
static bool s_test_failed;
static const char *s_failure_reason;

static test_stage_t s_test_stage;
static uint64_t s_stage_deadline_us;
static uint8_t s_reinit_round;

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
static uint64_t s_last_task_before_log_us;
static uint64_t s_last_task_after_log_us;
static uint32_t s_tuh_task_calls;
static uint32_t s_tuh_task_returns;
#endif

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

static const char *profile_name(void)
{
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_COLD_BOOT
    return "COLD_BOOT";
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG
    return "KEYBOARD_HOTPLUG";
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    return "HUB_HOTPLUG";
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_REINIT
    return "REINIT";
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HS_ROOT_DIRECT_FS
    return "HS_ROOT_DIRECT_FS";
#else
    return "LEGACY";
#endif
}

static bool robustness_profile(void)
{
    return TINYUSB_DIAG_PROFILE != TINYUSB_PROFILE_LEGACY;
}

static const char *stage_name(test_stage_t stage)
{
    switch (stage) {
    case TEST_STAGE_WAIT_INITIAL_HID:
        return "WAIT_INITIAL_HID";
    case TEST_STAGE_STABLE:
        return "STABLE";
    case TEST_STAGE_WAIT_UNMOUNT:
        return "WAIT_UNMOUNT";
    case TEST_STAGE_WAIT_REENUM:
        return "WAIT_REENUM";
    case TEST_STAGE_WAIT_RECONNECTED_A:
        return "WAIT_RECONNECTED_A";
    case TEST_STAGE_CLEANUP:
        return "CLEANUP";
    default:
        return "UNKNOWN";
    }
}

static void set_stage(test_stage_t stage, uint32_t timeout_ms)
{
    s_test_stage = stage;
    s_stage_deadline_us = timeout_ms == 0
        ? 0
        : (uint64_t)esp_timer_get_time() + ((uint64_t)timeout_ms * 1000ULL);
    ESP_LOGI(TAG, "ROBUST STAGE: %s timeout_ms=%u", stage_name(stage), timeout_ms);
}

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
static void hotplug_task_heartbeat_before(void)
{
    ++s_tuh_task_calls;
    if (s_test_stage != TEST_STAGE_WAIT_UNMOUNT) {
        return;
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s_last_task_before_log_us == 0 ||
        now - s_last_task_before_log_us >= ((uint64_t)HOTPLUG_HEARTBEAT_MS * 1000ULL)) {
        s_last_task_before_log_us = now;
        ESP_LOGI(TAG,
                 "P4-NANO TINYUSB HOTPLUG TASK ALIVE: before tuh_task_ext calls=%u hid_ready=%s",
                 (unsigned)s_tuh_task_calls, s_hid_ready ? "yes" : "no");
    }
}

static void hotplug_task_heartbeat_after(void)
{
    ++s_tuh_task_returns;
    if (s_test_stage != TEST_STAGE_WAIT_UNMOUNT) {
        return;
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s_last_task_after_log_us == 0 ||
        now - s_last_task_after_log_us >= ((uint64_t)HOTPLUG_HEARTBEAT_MS * 1000ULL)) {
        s_last_task_after_log_us = now;
        ESP_LOGI(TAG,
                 "P4-NANO TINYUSB HOTPLUG TASK ALIVE: after tuh_task_ext returns=%u calls=%u hid_ready=%s",
                 (unsigned)s_tuh_task_returns, (unsigned)s_tuh_task_calls,
                 s_hid_ready ? "yes" : "no");
    }
}
#else
static void hotplug_task_heartbeat_before(void) {}
static void hotplug_task_heartbeat_after(void) {}
#endif

static void request_cleanup(bool failed, const char *reason)
{
    if (failed) {
        s_test_failed = true;
        if (s_failure_reason == NULL) {
            s_failure_reason = reason;
        }
        ESP_LOGE(TAG, "ROBUST FAILURE: %s", reason);
    }

    s_cleanup_requested = true;
    set_stage(TEST_STAGE_CLEANUP, 0);
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

static void clear_hid_state(uint8_t daddr)
{
    if (!s_hid_ready || daddr != s_active_hid_daddr) {
        return;
    }

    s_last_umount_daddr = s_active_hid_daddr;
    s_last_umount_idx = s_active_hid_idx;
    s_last_umount_generation = s_hid_generation;
    s_hid_ready = false;
    s_active_hid_daddr = 0;
    s_active_hid_idx = INVALID_HID_INDEX;
    s_expected_sequence = NULL;
    s_expected_sequence_length = 0;
    s_expected_sequence_index = 0;
    s_sequence_pass_reported = false;
    hid_boot_keyboard_init(&s_keyboard_state);
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    ESP_LOGI(TAG,
             "P4-NANO TINYUSB HOTPLUG APP CHILD STATE CLEARED: addr=%u instance=%u generation=%u",
             s_last_umount_daddr, s_last_umount_idx, s_last_umount_generation);
#endif
    ESP_LOGI(TAG, "ROBUST HID STATE CLEAR PASS addr=%u generation=%u",
             s_last_umount_daddr, s_last_umount_generation);
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

    if (s_expected_sequence == NULL || s_sequence_pass_reported ||
        s_expected_sequence_index >= s_expected_sequence_length) {
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
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_LEGACY
        s_cleanup_requested = true;
        ESP_LOGI(TAG, "P4-NANO TINYUSB %s RESULT: PASS (software sequence)",
                 s_expected_sequence == s_hub_sequence ? "HUB FS" : "DIRECT");
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
      TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
        ESP_LOGI(TAG, "ROBUST RAW A AFTER RECONNECT: PASS generation=%u",
                 s_hid_generation);
        request_cleanup(false, NULL);
#endif
    }
}

void tuh_mount_cb(uint8_t daddr)
{
    log_device_bus(daddr, "DEVICE MOUNT");
}

void tuh_umount_cb(uint8_t daddr)
{
    ESP_LOGI(TAG, "P4-NANO TINYUSB DEVICE UMOUNT addr=%u", daddr);
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    ESP_LOGI(TAG, "P4-NANO TINYUSB HOTPLUG DEVICE UMOUNT: addr=%u stage=%s hid_ready=%s",
             daddr, stage_name(s_test_stage), s_hid_ready ? "yes" : "no");
#endif

    if (s_hid_ready && daddr == s_active_hid_daddr) {
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_LEGACY
        if (s_expected_sequence != NULL && !s_sequence_pass_reported) {
            ESP_LOGI(TAG, "P4-NANO TINYUSB %s RESULT: FAIL",
                     s_expected_sequence == s_hub_sequence ? "HUB FS" : "DIRECT");
        }
#endif
        clear_hid_state(daddr);
    }
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
        if (robustness_profile()) {
            ESP_LOGI(TAG, "ROBUST HUB ENUMERATION PASS addr=%u source=child-parent",
                     bus_info.hub_addr);
        }
    }

    if (!known_keyboard || !full_speed || !boot_keyboard) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HID: FAIL expected=0853:0103 FS Boot keyboard");
        if (robustness_profile()) {
            request_cleanup(true, "invalid HID identity/speed/protocol");
        }
        return;
    }

    if (s_hid_ready) {
        request_cleanup(true, "duplicate active HID instance");
        return;
    }

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_COLD_BOOT || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    if (!via_hub) {
        request_cleanup(true, "expected FS keyboard behind Hub");
        return;
    }
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HS_ROOT_DIRECT_FS
    if (via_hub) {
        request_cleanup(true, "expected direct FS keyboard for HS-root test");
        return;
    }
#endif

    ++s_hid_mount_count;
    ++s_hid_generation;
    s_active_hid_daddr = daddr;
    s_active_hid_idx = idx;
    s_hid_ready = true;
    hid_boot_keyboard_init(&s_keyboard_state);
    s_expected_sequence = NULL;
    s_expected_sequence_length = 0;
    s_expected_sequence_index = 0;
    s_sequence_pass_reported = false;

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_LEGACY
    s_expected_sequence = via_hub ? s_hub_sequence : s_direct_sequence;
    s_expected_sequence_length = via_hub
        ? sizeof(s_hub_sequence) / sizeof(s_hub_sequence[0])
        : sizeof(s_direct_sequence) / sizeof(s_direct_sequence[0]);
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
      TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    if (s_hid_mount_count >= 2) {
        if (s_test_stage != TEST_STAGE_WAIT_REENUM) {
            request_cleanup(true, "unexpected HID re-enumeration stage");
            return;
        }
        s_expected_sequence = s_raw_a_sequence;
        s_expected_sequence_length = sizeof(s_raw_a_sequence) / sizeof(s_raw_a_sequence[0]);
        ESP_LOGI(TAG, "ROBUST HID RE-ENUMERATION PASS addr=%u idx=%u generation=%u",
                 daddr, idx, s_hid_generation);
        ESP_LOGI(TAG, "ROBUST ACTION: press and release raw A after reconnect");
        set_stage(TEST_STAGE_WAIT_RECONNECTED_A, HOTPLUG_TIMEOUT_MS);
    } else {
        set_stage(TEST_STAGE_WAIT_UNMOUNT, HOTPLUG_TIMEOUT_MS);
        ESP_LOGI(TAG, "ROBUST HID READY PASS generation=%u; perform requested hot-unplug",
                 s_hid_generation);
    }
#elif TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_REINIT
    set_stage(TEST_STAGE_STABLE, STABLE_TIMEOUT_MS);
    ESP_LOGI(TAG, "ROBUST HID READY PASS generation=%u reinit_round=%u/%u",
             s_hid_generation, s_reinit_round, REINIT_ROUNDS);
#else
    set_stage(TEST_STAGE_STABLE, STABLE_TIMEOUT_MS);
    ESP_LOGI(TAG, "ROBUST HID READY PASS generation=%u", s_hid_generation);
#endif

    ESP_LOGI(TAG, "P4-NANO TINYUSB HID: PASS");
    ESP_LOGI(TAG, "P4-NANO TINYUSB HID READY mode=%s", via_hub ? "HUB-FS" : "DIRECT-FS");
    if (via_hub) {
        ESP_LOGI(TAG, "P4-NANO TINYUSB HUB + HID READY FOR KEY TEST");
    }

    if (!tuh_hid_receive_report(daddr, idx)) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HID RECEIVE: FAIL addr=%u idx=%u", daddr, idx);
        s_hid_ready = false;
        if (robustness_profile()) {
            request_cleanup(true, "initial HID report submission failed");
        }
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx)
{
    ESP_LOGI(TAG, "P4-NANO TINYUSB HID UMOUNT addr=%u idx=%u", daddr, idx);

    const bool matched = daddr == s_last_umount_daddr && idx == s_last_umount_idx;
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    ESP_LOGI(TAG,
             "P4-NANO TINYUSB HOTPLUG HID UMOUNT: addr=%u instance=%u stage=%s matched=%s",
             daddr, idx, stage_name(s_test_stage), matched ? "yes" : "no");
#endif
    if (robustness_profile() && !matched) {
        s_stale_callback = true;
        request_cleanup(true, "HID unmount identity mismatch");
        return;
    }

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    if (matched && s_test_stage == TEST_STAGE_WAIT_UNMOUNT) {
        ESP_LOGI(TAG, "ROBUST HID UNMOUNT OBSERVED PASS addr=%u idx=%u generation=%u",
                 daddr, idx, s_last_umount_generation);
        if (TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG) {
            ESP_LOGI(TAG, "ROBUST ACTION: verify HUB close, then reconnect Hub upstream");
        } else {
            ESP_LOGI(TAG, "ROBUST ACTION: reconnect keyboard to the same Hub port");
        }
        set_stage(TEST_STAGE_WAIT_REENUM, HOTPLUG_TIMEOUT_MS);
    }
#endif
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                const uint8_t *report, uint16_t len)
{
    if (!s_hid_ready || daddr != s_active_hid_daddr || idx != s_active_hid_idx) {
        s_stale_callback = true;
        ESP_LOGE(TAG, "ROBUST STALE CALLBACK: HID report addr=%u idx=%u active=%u:%u",
                 daddr, idx, s_active_hid_daddr, s_active_hid_idx);
        if (robustness_profile()) {
            request_cleanup(true, "HID report after unmount or wrong instance");
        }
        return;
    }

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
        if (robustness_profile()) {
            request_cleanup(true, "HID report resubmission failed");
        }
    }
}

static bool tinyusb_host_init(void)
{
    if (!tinyusb_board_p4_nano_phy_init()) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB PHY INIT: FAIL");
        return false;
    }

    const tuh_configure_param_t configure = {
        .dwc2 = {
            .use_hs_phy = true,
        },
    };
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_HOST,
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HS_ROOT_DIRECT_FS
        .speed = TUSB_SPEED_HIGH,
#else
        .speed = TUSB_SPEED_FULL,
#endif
    };

    if (!tuh_configure(TINYUSB_RHPORT, TUH_CFGID_DWC2, &configure) ||
        !tuh_rhport_init(TINYUSB_RHPORT, &rh_init)) {
        ESP_LOGE(TAG, "P4-NANO TINYUSB HOST INIT: FAIL");
        (void)tinyusb_board_p4_nano_phy_deinit();
        return false;
    }

    ESP_LOGI(TAG, "P4-NANO TINYUSB HOST INIT: PASS");
    return true;
}

static bool __attribute__((unused)) perform_reinit(void)
{
    ESP_LOGI(TAG, "ROBUST REINIT ROUND %u BEGIN", (unsigned)(s_reinit_round + 1U));

    const bool host_cleanup = tuh_deinit(TINYUSB_RHPORT);
    const bool phy_cleanup = tinyusb_board_p4_nano_phy_deinit();
    const bool inactive = !tuh_rhport_is_active(TINYUSB_RHPORT) && !tuh_inited();
    ESP_LOGI(TAG, "ROBUST REINIT TEARDOWN host=%s phy=%s inactive=%s",
             host_cleanup ? "PASS" : "FAIL",
             phy_cleanup ? "PASS" : "FAIL",
             inactive ? "PASS" : "FAIL");

    if (!host_cleanup || !phy_cleanup || !inactive) {
        request_cleanup(true, "reinit teardown failed");
        return false;
    }

    ++s_reinit_round;
    s_cleanup_requested = false;
    s_stage_deadline_us = 0;
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!tinyusb_host_init()) {
        request_cleanup(true, "reinit host/PHY init failed");
        return false;
    }

    ESP_LOGI(TAG, "ROBUST REINIT ROUND %u INIT PASS", (unsigned)s_reinit_round);
    set_stage(TEST_STAGE_WAIT_INITIAL_HID, INITIAL_TIMEOUT_MS);
    return true;
}

static void robustness_tick(void)
{
    if (!robustness_profile() || s_cleanup_requested || s_stage_deadline_us == 0) {
        return;
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (now < s_stage_deadline_us) {
        return;
    }

#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_REINIT
    if (s_test_stage == TEST_STAGE_STABLE) {
        if (s_reinit_round < REINIT_ROUNDS) {
            (void)perform_reinit();
        } else {
            ESP_LOGI(TAG, "ROBUST REINIT RESULT: PASS rounds=%u", REINIT_ROUNDS);
            request_cleanup(false, NULL);
        }
        return;
    }
#endif

    if (s_test_stage == TEST_STAGE_STABLE) {
        request_cleanup(false, NULL);
        return;
    }

    request_cleanup(true, "bounded test stage timeout");
}

static void tinyusb_host_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "P4-NANO TINYUSB HOST START");
    ESP_LOGI(TAG, "P4-NANO TINYUSB VERSION: 0.21.0");
    ESP_LOGI(TAG, "P4-NANO TINYUSB PROFILE: %s", profile_name());
    ESP_LOGI(TAG, "P4-NANO TINYUSB RHPORT: %u", TINYUSB_RHPORT);
    ESP_LOGI(TAG, "P4-NANO TINYUSB PHY: HS/UTMI");
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HS_ROOT_DIRECT_FS
    ESP_LOGI(TAG, "P4-NANO TINYUSB ROOT MODE: HS");
    ESP_LOGI(TAG, "P4-NANO TINYUSB SPLIT: direct-FS HS-root behavior under test; no external Hub");
#else
    ESP_LOGI(TAG, "P4-NANO TINYUSB ROOT MODE: FS");
    ESP_LOGI(TAG, "P4-NANO TINYUSB SPLIT: DISABLED root-speed=FS");
#endif

    s_active_hid_daddr = 0;
    s_active_hid_idx = INVALID_HID_INDEX;
    s_last_umount_daddr = 0;
    s_last_umount_idx = INVALID_HID_INDEX;
    s_hid_generation = 0;
    s_hid_mount_count = 0;
    s_reinit_round = 0;
#if TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_KEYBOARD_HOTPLUG || \
    TINYUSB_DIAG_PROFILE == TINYUSB_PROFILE_HUB_HOTPLUG
    s_last_task_before_log_us = 0;
    s_last_task_after_log_us = 0;
    s_tuh_task_calls = 0;
    s_tuh_task_returns = 0;
#endif

    if (!tinyusb_host_init()) {
        vTaskDelete(NULL);
        return;
    }

    if (robustness_profile()) {
        set_stage(TEST_STAGE_WAIT_INITIAL_HID, INITIAL_TIMEOUT_MS);
    }

    for (;;) {
        if (robustness_profile()) {
            hotplug_task_heartbeat_before();
            tuh_task_ext(20, false);
            hotplug_task_heartbeat_after();
            robustness_tick();
        } else {
            tuh_task();
        }

        if (s_cleanup_requested) {
            const bool host_cleanup = tuh_deinit(TINYUSB_RHPORT);
            const bool phy_cleanup = tinyusb_board_p4_nano_phy_deinit();
            const bool inactive = !tuh_rhport_is_active(TINYUSB_RHPORT) && !tuh_inited();
            const bool cleanup_pass = host_cleanup && phy_cleanup && inactive && !s_stale_callback;
            ESP_LOGI(TAG, "P4-NANO TINYUSB CLEANUP RESULT: %s host=%s phy=%s inactive=%s stale=%s",
                     cleanup_pass ? "PASS" : "FAIL",
                     host_cleanup ? "PASS" : "FAIL",
                     phy_cleanup ? "PASS" : "FAIL",
                     inactive ? "PASS" : "FAIL",
                     s_stale_callback ? "FAIL" : "PASS");
            ESP_LOGI(TAG, "ROBUST RUN RESULT: %s reason=%s mounts=%u generations=%u",
                     (!s_test_failed && cleanup_pass) ? "PASS" : "FAIL",
                     s_failure_reason == NULL ? "none" : s_failure_reason,
                     (unsigned)s_hid_mount_count, (unsigned)s_hid_generation);
            vTaskDelete(NULL);
            return;
        }
    }
}

void app_main(void)
{
    xTaskCreate(tinyusb_host_task, "tinyusb_host", 6144, NULL, 5, NULL);
}
