#include "hid_boot_keyboard/hid_boot_keyboard.h"

#include <string.h>

#define HID_BOOT_ERROR_ROLLOVER 0x01U
#define HID_BOOT_POST_FAIL 0x02U
#define HID_BOOT_ERROR_UNDEFINED 0x03U

static uint8_t modifier_usage(unsigned bit)
{
    return (uint8_t)(0xe0U + bit);
}

static bool key_present(const uint8_t *keys, uint8_t usage)
{
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

static bool key_seen_before(const uint8_t *keys, size_t index, uint8_t usage)
{
    for (size_t i = 0; i < index; ++i) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

static void stage_event(hid_boot_keyboard_event_t *events, size_t *count,
                        hid_boot_keyboard_event_kind_t kind, uint8_t usage,
                        uint8_t modifiers)
{
    /* The fixed report shape proves that count stays within this bound. */
    events[*count] = (hid_boot_keyboard_event_t){
        .kind = kind,
        .usage = usage,
        .modifiers = modifiers,
    };
    ++*count;
}

void hid_boot_keyboard_init(hid_boot_keyboard_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool hid_boot_keyboard_is_error_usage(uint8_t usage)
{
    return usage == HID_BOOT_ERROR_ROLLOVER ||
           usage == HID_BOOT_POST_FAIL ||
           usage == HID_BOOT_ERROR_UNDEFINED;
}

size_t hid_boot_keyboard_process(
    hid_boot_keyboard_state_t *state,
    const uint8_t *report,
    size_t report_length,
    hid_boot_keyboard_event_t *events,
    size_t event_capacity)
{
    if (state == NULL || report == NULL ||
        report_length != HID_BOOT_KEYBOARD_REPORT_SIZE) {
        return 0;
    }

    hid_boot_keyboard_event_t staged[HID_BOOT_KEYBOARD_MAX_EVENTS];
    const uint8_t modifiers = report[0];
    const uint8_t *keys = &report[2];
    size_t count = 0;
    bool has_error_usage = false;

    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        if (hid_boot_keyboard_is_error_usage(keys[i])) {
            has_error_usage = true;
            stage_event(staged, &count, HID_BOOT_EVENT_ERROR_USAGE,
                        keys[i], modifiers);
        }
    }

    if (!has_error_usage) {
        const uint8_t previous_modifiers = state->modifiers;
        for (unsigned bit = 0; bit < HID_BOOT_KEYBOARD_MODIFIER_COUNT; ++bit) {
            const uint8_t mask = (uint8_t)(1U << bit);
            if ((previous_modifiers & mask) == 0U && (modifiers & mask) != 0U) {
                stage_event(staged, &count, HID_BOOT_EVENT_MOD_PRESS,
                            modifier_usage(bit), modifiers);
            } else if ((previous_modifiers & mask) != 0U &&
                       (modifiers & mask) == 0U) {
                stage_event(staged, &count, HID_BOOT_EVENT_MOD_RELEASE,
                            modifier_usage(bit), modifiers);
            }
        }

        for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
            const uint8_t usage = state->keys[i];
            if (usage != 0U && !key_seen_before(state->keys, i, usage) &&
                !key_present(keys, usage)) {
                stage_event(staged, &count, HID_BOOT_EVENT_KEY_RELEASE,
                            usage, modifiers);
            }
        }

        for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
            const uint8_t usage = keys[i];
            if (usage != 0U && !key_seen_before(keys, i, usage) &&
                !key_present(state->keys, usage)) {
                stage_event(staged, &count, HID_BOOT_EVENT_KEY_PRESS,
                            usage, modifiers);
            }
        }
    }

    if (count > event_capacity || (count != 0U && events == NULL)) {
        return count;
    }

    if (count != 0U) {
        memcpy(events, staged, count * sizeof(*events));
    }

    if (has_error_usage) {
        return count;
    }

    state->modifiers = modifiers;
    memcpy(state->keys, keys, HID_BOOT_KEYBOARD_KEY_COUNT);
    return count;
}
