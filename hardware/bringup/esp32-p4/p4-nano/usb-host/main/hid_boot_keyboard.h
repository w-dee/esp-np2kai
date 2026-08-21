#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_BOOT_KEYBOARD_REPORT_SIZE 8U
#define HID_BOOT_KEYBOARD_KEY_COUNT 6U
#define HID_BOOT_KEYBOARD_MAX_EVENTS 16U

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
 * Convert one 8-byte Boot Protocol report into deterministic transitions.
 * Invalid-length reports produce no transitions and do not change state.
 * Error usages are returned as events and do not change key state.
 */
size_t hid_boot_keyboard_process(
    hid_boot_keyboard_state_t *state,
    const uint8_t *report,
    size_t report_length,
    hid_boot_keyboard_event_t *events,
    size_t event_capacity);

bool hid_boot_keyboard_is_error_usage(uint8_t usage);
