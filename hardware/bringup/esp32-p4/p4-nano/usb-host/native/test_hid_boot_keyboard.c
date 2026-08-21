#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../main/hid_boot_keyboard.h"

static size_t process(hid_boot_keyboard_state_t *state, const uint8_t *report,
                      hid_boot_keyboard_event_t *events)
{
    return hid_boot_keyboard_process(state, report,
                                      HID_BOOT_KEYBOARD_REPORT_SIZE, events,
                                      HID_BOOT_KEYBOARD_MAX_EVENTS);
}

static void expect_event(const hid_boot_keyboard_event_t *event,
                         hid_boot_keyboard_event_kind_t kind, uint8_t usage,
                         uint8_t modifiers)
{
    assert(event->kind == kind);
    assert(event->usage == usage);
    assert(event->modifiers == modifiers);
}

static uint8_t *report_key(uint8_t *report, uint8_t modifiers, uint8_t usage)
{
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_REPORT_SIZE; ++i) {
        report[i] = 0;
    }
    report[0] = modifiers;
    report[2] = usage;
    return report;
}

static void expect_press_release(uint8_t usage)
{
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_init(&state);

    report_key(report, 0, usage);
    size_t count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_PRESS, usage, 0);

    report_key(report, 0, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, usage, 0);
}

static void test_modifiers_and_shift_a(void)
{
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_init(&state);

    report_key(report, 0x02, 0);
    size_t count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_PRESS, 0xe1, 0x02);

    report_key(report, 0x02, 0x04);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_PRESS, 0x04, 0x02);

    report_key(report, 0x02, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0x02);

    report_key(report, 0, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_RELEASE, 0xe1, 0);
}

static void test_ctrl_alt_and_special_keys(void)
{
    const uint8_t usages[] = {0x2c, 0x28, 0x3a, 0x52, 0x51, 0x50, 0x4f};
    for (size_t i = 0; i < sizeof(usages); ++i) {
        expect_press_release(usages[i]);
    }

    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_init(&state);
    report_key(report, 0x01, 0);
    size_t count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_PRESS, 0xe0, 0x01);
    report_key(report, 0, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_RELEASE, 0xe0, 0);
    report_key(report, 0x04, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_PRESS, 0xe2, 0x04);
    report_key(report, 0, 0);
    count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_MOD_RELEASE, 0xe2, 0);
}

static void test_report_state_rules(void)
{
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_init(&state);

    report_key(report, 0, 0x04);
    assert(process(&state, report, events) == 1);
    assert(process(&state, report, events) == 0);

    report_key(report, 0, 0x05);
    assert(process(&state, report, events) == 2);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0);
    expect_event(&events[1], HID_BOOT_EVENT_KEY_PRESS, 0x05, 0);

    report_key(report, 0, 0x04);
    assert(process(&state, report, events) == 2);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, 0x05, 0);
    expect_event(&events[1], HID_BOOT_EVENT_KEY_PRESS, 0x04, 0);

    report[3] = 0x05;
    assert(process(&state, report, events) == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_PRESS, 0x05, 0);
    assert(process(&state, report, events) == 0);
}

static void test_rollover_and_invalid_report(void)
{
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_init(&state);
    report_key(report, 0, 0x04);
    assert(process(&state, report, events) == 1);

    report_key(report, 0, 0x01);
    size_t count = process(&state, report, events);
    assert(count == 1);
    expect_event(&events[0], HID_BOOT_EVENT_ERROR_USAGE, 0x01, 0);
    assert(state.keys[0] == 0x04);

    assert(hid_boot_keyboard_process(&state, report, 7, events,
                                     HID_BOOT_KEYBOARD_MAX_EVENTS) == 0);
    assert(state.keys[0] == 0x04);
}

int main(void)
{
    expect_press_release(0x04);
    expect_press_release(0x1e);
    test_modifiers_and_shift_a();
    test_ctrl_alt_and_special_keys();
    test_report_state_rules();
    test_rollover_and_invalid_report();
    puts("hid_boot_keyboard native tests: PASS");
    return 0;
}
