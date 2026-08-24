#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_BOOT_KEYBOARD_REPORT_SIZE 8U
#define HID_BOOT_KEYBOARD_MODIFIER_COUNT 8U
#define HID_BOOT_KEYBOARD_KEY_COUNT 6U
#define HID_BOOT_KEYBOARD_MAX_EVENTS \
    (HID_BOOT_KEYBOARD_MODIFIER_COUNT + (2U * HID_BOOT_KEYBOARD_KEY_COUNT))

#if defined(__cplusplus)
static_assert(HID_BOOT_KEYBOARD_MAX_EVENTS == 20U,
              "Boot keyboard event bound must cover all report edges");
#else
_Static_assert(HID_BOOT_KEYBOARD_MAX_EVENTS == 20U,
               "Boot keyboard event bound must cover all report edges");
#endif

typedef enum {
    HID_BOOT_EVENT_KEY_PRESS = 0,
    HID_BOOT_EVENT_KEY_RELEASE,
    HID_BOOT_EVENT_MOD_PRESS,
    HID_BOOT_EVENT_MOD_RELEASE,
    HID_BOOT_EVENT_ERROR_USAGE,
} hid_boot_keyboard_event_kind_t;

typedef struct {
    hid_boot_keyboard_event_kind_t kind;
    uint8_t usage;
    uint8_t modifiers;
} hid_boot_keyboard_event_t;

typedef struct {
    uint8_t modifiers;
    uint8_t keys[HID_BOOT_KEYBOARD_KEY_COUNT];
} hid_boot_keyboard_state_t;

void hid_boot_keyboard_init(hid_boot_keyboard_state_t *state);

/*
 * Convert exactly one 8-byte Boot Protocol report into deterministic
 * transitions. The return value is the required/produced event count.
 *
 * If the return value is greater than event_capacity, events is untouched and
 * state is unchanged; the report may be retried with a larger buffer. A
 * non-null events buffer is required whenever the returned count is non-zero.
 * Error usages produce ERROR_USAGE diagnostics but do not modify the trusted
 * keyboard state. A clean report commits state only after all events fit.
 * Invalid-length reports produce no transitions and do not change state.
 */
size_t hid_boot_keyboard_process(
    hid_boot_keyboard_state_t *state,
    const uint8_t *report,
    size_t report_length,
    hid_boot_keyboard_event_t *events,
    size_t event_capacity);

bool hid_boot_keyboard_is_error_usage(uint8_t usage);

#ifdef __cplusplus
}
#endif
