#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../main/hid_boot_keyboard.h"

static size_t process_with_capacity(hid_boot_keyboard_state_t *state,
                                    const uint8_t *report, size_t report_length,
                                    hid_boot_keyboard_event_t *events,
                                    size_t event_capacity)
{
    return hid_boot_keyboard_process(state, report, report_length, events,
                                      event_capacity);
}

static size_t process(hid_boot_keyboard_state_t *state, const uint8_t *report,
                      hid_boot_keyboard_event_t *events)
{
    return process_with_capacity(state, report, HID_BOOT_KEYBOARD_REPORT_SIZE,
                                 events, HID_BOOT_KEYBOARD_MAX_EVENTS);
}

static void expect_event(const hid_boot_keyboard_event_t *event,
                         hid_boot_keyboard_event_kind_t kind, uint8_t usage,
                         uint8_t modifiers)
{
    assert(event->kind == kind);
    assert(event->usage == usage);
    assert(event->modifiers == modifiers);
}

static void expect_events_equal(const hid_boot_keyboard_event_t *left,
                                const hid_boot_keyboard_event_t *right,
                                size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        assert(left[i].kind == right[i].kind);
        assert(left[i].usage == right[i].usage);
        assert(left[i].modifiers == right[i].modifiers);
    }
}

static void expect_state_equal(const hid_boot_keyboard_state_t *left,
                               const hid_boot_keyboard_state_t *right)
{
    assert(left->modifiers == right->modifiers);
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        assert(left->keys[i] == right->keys[i]);
    }
}

static void fill_report(uint8_t *report, uint8_t modifiers,
                        const uint8_t keys[HID_BOOT_KEYBOARD_KEY_COUNT])
{
    memset(report, 0, HID_BOOT_KEYBOARD_REPORT_SIZE);
    report[0] = modifiers;
    memcpy(&report[2], keys, HID_BOOT_KEYBOARD_KEY_COUNT);
}

static uint8_t *report_key(uint8_t *report, uint8_t modifiers, uint8_t usage)
{
    const uint8_t keys[HID_BOOT_KEYBOARD_KEY_COUNT] = {usage, 0, 0, 0, 0, 0};
    fill_report(report, modifiers, keys);
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
    const uint8_t duplicate[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x04, 0x04, 0, 0, 0, 0};
    const uint8_t moved[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0, 0x04, 0, 0, 0, 0};
    const uint8_t empty[HID_BOOT_KEYBOARD_KEY_COUNT] = {0, 0, 0, 0, 0, 0};
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

    fill_report(report, 0, duplicate);
    assert(process(&state, report, events) == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, 0x05, 0);
    assert(process(&state, report, events) == 0);

    fill_report(report, 0, moved);
    assert(process(&state, report, events) == 0);

    fill_report(report, 0, duplicate);
    assert(process(&state, report, events) == 0);

    fill_report(report, 0, empty);
    assert(process(&state, report, events) == 1);
    expect_event(&events[0], HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0);
}

static void test_worst_case_transactionality(void)
{
    const uint8_t old_keys[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    const uint8_t new_keys[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    uint8_t old_report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    uint8_t new_report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    hid_boot_keyboard_event_t sentinel[HID_BOOT_KEYBOARD_MAX_EVENTS];
    hid_boot_keyboard_event_t retry_events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    hid_boot_keyboard_event_t larger_events[HID_BOOT_KEYBOARD_MAX_EVENTS];

    fill_report(old_report, 0xff, old_keys);
    fill_report(new_report, 0, new_keys);
    hid_boot_keyboard_init(&state);
    assert(process(&state, old_report, events) == 14);

    const hid_boot_keyboard_state_t before = state;
    memset(events, 0xa5, sizeof(events));
    memcpy(sentinel, events, sizeof(events));
    size_t count = process_with_capacity(&state, new_report,
                                         HID_BOOT_KEYBOARD_REPORT_SIZE, events,
                                         HID_BOOT_KEYBOARD_MAX_EVENTS - 1U);
    assert(count == HID_BOOT_KEYBOARD_MAX_EVENTS);
    assert(memcmp(events, sentinel, sizeof(events)) == 0);
    expect_state_equal(&state, &before);

    count = process_with_capacity(&state, new_report,
                                  HID_BOOT_KEYBOARD_REPORT_SIZE, events, 0);
    assert(count == HID_BOOT_KEYBOARD_MAX_EVENTS);
    assert(memcmp(events, sentinel, sizeof(events)) == 0);
    expect_state_equal(&state, &before);

    count = process_with_capacity(&state, new_report,
                                  HID_BOOT_KEYBOARD_REPORT_SIZE, retry_events,
                                  HID_BOOT_KEYBOARD_MAX_EVENTS);
    assert(count == HID_BOOT_KEYBOARD_MAX_EVENTS);
    for (unsigned bit = 0; bit < HID_BOOT_KEYBOARD_MODIFIER_COUNT; ++bit) {
        expect_event(&retry_events[bit], HID_BOOT_EVENT_MOD_RELEASE,
                     (uint8_t)(0xe0U + bit), 0);
    }
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        expect_event(&retry_events[8U + i], HID_BOOT_EVENT_KEY_RELEASE,
                     old_keys[i], 0);
        expect_event(&retry_events[14U + i], HID_BOOT_EVENT_KEY_PRESS,
                     new_keys[i], 0);
    }
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        assert(state.keys[i] == new_keys[i]);
    }
    assert(state.modifiers == 0);

    hid_boot_keyboard_state_t larger_state;
    hid_boot_keyboard_init(&larger_state);
    assert(process(&larger_state, old_report, events) == 14);
    count = process_with_capacity(&larger_state, new_report,
                                  HID_BOOT_KEYBOARD_REPORT_SIZE, larger_events,
                                  HID_BOOT_KEYBOARD_MAX_EVENTS + 1U);
    assert(count == HID_BOOT_KEYBOARD_MAX_EVENTS);
    expect_events_equal(retry_events, larger_events, count);
    assert(larger_state.modifiers == 0);
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        assert(larger_state.keys[i] == new_keys[i]);
    }

    /* A later clean report still diffs from the last committed report. */
    hid_boot_keyboard_state_t alternate_state = before;
    uint8_t alternate_report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    fill_report(alternate_report, 0, old_keys);
    count = process(&alternate_state, alternate_report, events);
    assert(count == HID_BOOT_KEYBOARD_MODIFIER_COUNT);
    for (unsigned bit = 0; bit < HID_BOOT_KEYBOARD_MODIFIER_COUNT; ++bit) {
        expect_event(&events[bit], HID_BOOT_EVENT_MOD_RELEASE,
                     (uint8_t)(0xe0U + bit), 0);
    }

    hid_boot_keyboard_state_t no_buffer_state = before;
    count = process_with_capacity(&no_buffer_state, new_report,
                                  HID_BOOT_KEYBOARD_REPORT_SIZE, NULL,
                                  HID_BOOT_KEYBOARD_MAX_EVENTS);
    assert(count == HID_BOOT_KEYBOARD_MAX_EVENTS);
    expect_state_equal(&no_buffer_state, &before);
}

static void test_error_usage_transactionality(void)
{
    const uint8_t clean_a_keys[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x04, 0, 0, 0, 0, 0};
    const uint8_t clean_b_keys[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x05, 0, 0, 0, 0, 0};
    const uint8_t error_usages[] = {0x01, 0x02, 0x03};
    uint8_t clean_a[HID_BOOT_KEYBOARD_REPORT_SIZE];
    uint8_t clean_b[HID_BOOT_KEYBOARD_REPORT_SIZE];
    uint8_t error_report[HID_BOOT_KEYBOARD_REPORT_SIZE];
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    fill_report(clean_a, 0x01, clean_a_keys);
    fill_report(clean_b, 0, clean_b_keys);

    for (size_t i = 0; i < sizeof(error_usages); ++i) {
        hid_boot_keyboard_state_t state;
        hid_boot_keyboard_init(&state);
        assert(process(&state, clean_a, events) == 2);
        const hid_boot_keyboard_state_t trusted = state;
        const uint8_t error_keys[HID_BOOT_KEYBOARD_KEY_COUNT] =
            {error_usages[i], 0x20, 0x21, 0x22, 0x23, 0x24};
        fill_report(error_report, 0xfe, error_keys);
        size_t count = process(&state, error_report, events);
        assert(count == 1);
        expect_event(&events[0], HID_BOOT_EVENT_ERROR_USAGE,
                     error_usages[i], 0xfe);
        expect_state_equal(&state, &trusted);

        count = process(&state, clean_b, events);
        assert(count == 3);
        expect_event(&events[0], HID_BOOT_EVENT_MOD_RELEASE, 0xe0, 0);
        expect_event(&events[1], HID_BOOT_EVENT_KEY_RELEASE, 0x04, 0);
        expect_event(&events[2], HID_BOOT_EVENT_KEY_PRESS, 0x05, 0);
    }

    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_init(&state);
    assert(process(&state, clean_a, events) == 2);
    const hid_boot_keyboard_state_t trusted = state;
    const uint8_t many_errors[HID_BOOT_KEYBOARD_KEY_COUNT] =
        {0x01, 0x02, 0x03, 0x01, 0x02, 0x03};
    fill_report(error_report, 0x7f, many_errors);
    hid_boot_keyboard_event_t sentinel[HID_BOOT_KEYBOARD_MAX_EVENTS];
    memset(events, 0xa5, sizeof(events));
    memcpy(sentinel, events, sizeof(events));
    size_t count = process_with_capacity(&state, error_report,
                                         HID_BOOT_KEYBOARD_REPORT_SIZE, events,
                                         HID_BOOT_KEYBOARD_KEY_COUNT - 1U);
    assert(count == HID_BOOT_KEYBOARD_KEY_COUNT);
    assert(memcmp(events, sentinel, sizeof(events)) == 0);
    expect_state_equal(&state, &trusted);

    count = process_with_capacity(&state, error_report,
                                  HID_BOOT_KEYBOARD_REPORT_SIZE, events,
                                  HID_BOOT_KEYBOARD_KEY_COUNT);
    assert(count == HID_BOOT_KEYBOARD_KEY_COUNT);
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        expect_event(&events[i], HID_BOOT_EVENT_ERROR_USAGE,
                     many_errors[i], 0x7f);
    }
    expect_state_equal(&state, &trusted);
}

static void test_exact_report_lengths(void)
{
    hid_boot_keyboard_state_t state;
    hid_boot_keyboard_event_t events[HID_BOOT_KEYBOARD_MAX_EVENTS];
    hid_boot_keyboard_event_t sentinel[HID_BOOT_KEYBOARD_MAX_EVENTS];
    uint8_t report[HID_BOOT_KEYBOARD_REPORT_SIZE + 1U];
    const uint8_t keys[HID_BOOT_KEYBOARD_KEY_COUNT] = {0x04, 0, 0, 0, 0, 0};
    fill_report(report, 0x01, keys);
    report[HID_BOOT_KEYBOARD_REPORT_SIZE] = 0xa5;
    hid_boot_keyboard_init(&state);
    assert(process(&state, report, events) == 2);
    const hid_boot_keyboard_state_t before = state;

    memset(events, 0xa5, sizeof(events));
    memcpy(sentinel, events, sizeof(events));
    assert(process_with_capacity(&state, report, 7, events,
                                 HID_BOOT_KEYBOARD_MAX_EVENTS) == 0);
    assert(memcmp(events, sentinel, sizeof(events)) == 0);
    expect_state_equal(&state, &before);

    assert(process_with_capacity(&state, report, 9, events,
                                 HID_BOOT_KEYBOARD_MAX_EVENTS) == 0);
    assert(memcmp(events, sentinel, sizeof(events)) == 0);
    expect_state_equal(&state, &before);
}

static void test_reset_and_error_predicate(void)
{
    hid_boot_keyboard_state_t state = {.modifiers = 0xff,
                                       .keys = {1, 2, 3, 4, 5, 6}};
    hid_boot_keyboard_init(&state);
    assert(state.modifiers == 0);
    for (size_t i = 0; i < HID_BOOT_KEYBOARD_KEY_COUNT; ++i) {
        assert(state.keys[i] == 0);
    }
    assert(hid_boot_keyboard_is_error_usage(0x01));
    assert(hid_boot_keyboard_is_error_usage(0x02));
    assert(hid_boot_keyboard_is_error_usage(0x03));
    assert(!hid_boot_keyboard_is_error_usage(0));
    assert(!hid_boot_keyboard_is_error_usage(0x04));
}

int main(void)
{
    expect_press_release(0x04);
    expect_press_release(0x1e);
    test_modifiers_and_shift_a();
    test_ctrl_alt_and_special_keys();
    test_report_state_rules();
    test_worst_case_transactionality();
    test_error_usage_transactionality();
    test_exact_report_lengths();
    test_reset_and_error_predicate();
    puts("hid_boot_keyboard native tests: PASS");
    return 0;
}
