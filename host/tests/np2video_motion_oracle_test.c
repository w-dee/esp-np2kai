#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2video_motion_oracle.h"

static uint8_t guest[NP2VIDEO_MOTION_SOURCE_WIDTH *
                     NP2VIDEO_MOTION_SOURCE_HEIGHT * 2U];
static uint16_t native[NP2VIDEO_MOTION_NATIVE_WIDTH *
                       NP2VIDEO_MOTION_NATIVE_HEIGHT];

static uint16_t *guest_at(unsigned x, unsigned y)
{
    return (uint16_t *)(guest + (size_t)y * NP2VIDEO_MOTION_SOURCE_PITCH_BYTES +
                          (size_t)x * 2U);
}

static void make_guest(unsigned pos, uint16_t color)
{
    unsigned y;
    memset(guest, 0, sizeof(guest));
    for (y = 0U; y < NP2VIDEO_MOTION_SOURCE_HEIGHT; ++y) {
        unsigned x;
        for (x = pos * 8U; x < pos * 8U + 64U; ++x) {
            *guest_at(x, y) = color;
        }
    }
}

static void make_native(const np2video_motion_guest_sample *sample)
{
    uint32_t y_start;
    uint32_t y_end;
    unsigned x;
    assert(np2video_motion_expected_native_band(sample->bar_pos, &y_start,
                                                &y_end));
    memset(native, 0, sizeof(native));
    for (x = 0U; x < NP2VIDEO_MOTION_NATIVE_WIDTH; ++x) {
        uint32_t y;
        for (y = y_start; y <= y_end; ++y) {
            native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + x] =
                sample->bar_color;
        }
    }
}

static void expect_valid(unsigned pos, uint16_t color, uint32_t native_start,
                         uint32_t native_end)
{
    np2video_motion_guest_sample guest_sample;
    np2video_motion_native_sample native_sample;

    make_guest(pos, color);
    assert(np2video_motion_guest_detect(
        guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
    assert(guest_sample.status == NP2VIDEO_MOTION_GUEST_VALID);
    assert(guest_sample.bar_pos == pos);
    assert(guest_sample.x_start == pos * 8U);
    assert(guest_sample.x_end == pos * 8U + 63U);
    assert(guest_sample.bar_color == color);
    make_native(&guest_sample);
    assert(np2video_motion_native_detect(
        native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample, &native_sample));
    assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_VALID);
    assert(native_sample.y_start == native_start);
    assert(native_sample.y_end == native_end);
}

static void expect_guest_failure(np2video_motion_guest_status expected)
{
    np2video_motion_guest_sample sample;
    assert(!np2video_motion_guest_detect(
        guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &sample));
    assert(sample.status == expected);
}

static void expect_native_failure(np2video_motion_native_status expected)
{
    np2video_motion_guest_sample guest_sample;
    np2video_motion_native_sample native_sample;

    make_guest(38U, 0x1234U);
    assert(np2video_motion_guest_detect(
        guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
    make_native(&guest_sample);
    {
        uint32_t y_start;
        uint32_t y_end;
        assert(np2video_motion_expected_native_band(guest_sample.bar_pos,
                                                    &y_start, &y_end));
        memset(native, 0, sizeof(native));
        for (uint32_t y = y_start + 1U; y <= y_end + 1U; ++y) {
            for (unsigned x = 0U; x < NP2VIDEO_MOTION_NATIVE_WIDTH; ++x) {
                native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + x] =
                    guest_sample.bar_color;
            }
        }
    }
    assert(!np2video_motion_native_detect(
        native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample, &native_sample));
    assert(native_sample.status == expected);
}

int main(void)
{
    np2video_motion_guest_sample sample;
    uint32_t y_start;
    uint32_t y_end;

    expect_valid(8U, 0x001fU, 1024U, 1151U);
    expect_valid(38U, 0xf81fU, 544U, 671U);
    expect_valid(68U, 0x000eU, 64U, 191U);
    assert(np2video_motion_expected_bar_pos(0U) == 8U);
    assert(np2video_motion_expected_bar_pos(1U) == 9U);
    assert(np2video_motion_expected_bar_pos(60U) == 68U);
    assert(np2video_motion_expected_bar_pos(61U) == 67U);
    assert(np2video_motion_expected_bar_pos(119U) == 9U);
    assert(np2video_motion_expected_bar_pos(120U) == 8U);
    assert(np2video_motion_expected_bar_pos(121U) == 9U);
    assert(np2video_motion_expected_native_band(7U, &y_start, &y_end) == false);
    assert(np2video_motion_expected_native_band(69U, &y_start, &y_end) == false);

    memset(guest, 0, sizeof(guest));
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_NO_BAR);

    make_guest(38U, 0x1234U);
    for (unsigned x = 200U; x < 264U; ++x) {
        *guest_at(x, 100U) = 0x5678U;
    }
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS);

    make_guest(38U, 0x1234U);
    *guest_at(38U * 8U + 63U, 0U) = 0U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_WRONG_WIDTH);

    make_guest(38U, 0x1234U);
    *guest_at(38U * 8U + 64U, 0U) = 0x1234U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_WRONG_WIDTH);

    make_guest(38U, 0x1234U);
    memmove(guest_at(38U * 8U + 1U, 0U), guest_at(38U * 8U, 0U), 64U * 2U);
    *guest_at(38U * 8U, 0U) = 0U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_INVALID_ALIGNMENT);

    make_guest(38U, 0x1234U);
    memset(guest_at(38U * 8U, 100U), 0, 64U * 2U);
    for (unsigned x = 39U * 8U; x < 39U * 8U + 64U; ++x) {
        *guest_at(x, 100U) = 0x1234U;
    }
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_ROW_MISMATCH);

    make_guest(38U, 0x1234U);
    *guest_at(38U * 8U + 10U, 200U) = 0x5678U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH);

    make_guest(38U, 0x1234U);
    *guest_at(0U, 200U) = 1U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS);

    make_guest(38U, 0x1234U);
    *guest_at(0U, 300U) = 0x1234U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS);

    make_guest(38U, 0x1234U);
    *guest_at(0U, 399U) = 1U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS);

    make_guest(38U, 0x1234U);
    *guest_at(38U * 8U, 399U) = 0x1235U;
    expect_guest_failure(NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH);

    expect_native_failure(NP2VIDEO_MOTION_NATIVE_POSITION_MISMATCH);
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        native[544U * NP2VIDEO_MOTION_NATIVE_WIDTH + 0U] = 0U;
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_WRONG_SIZE);
    }
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        native[544U * NP2VIDEO_MOTION_NATIVE_WIDTH] = 0x5678U;
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_WRONG_COLOR);
    }
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        native[544U * NP2VIDEO_MOTION_NATIVE_WIDTH + 200U] = 0x5678U;
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_WRONG_COLOR);
    }
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        for (unsigned y = 544U; y <= 672U; ++y) {
            native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + 400U] =
                guest_sample.bar_color;
        }
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_WRONG_SIZE);
    }
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        for (unsigned y = 0U; y < 4U; ++y) {
            native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + 600U] =
                guest_sample.bar_color;
        }
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_MULTIPLE_RUNS);
    }
    {
        np2video_motion_guest_sample guest_sample;
        np2video_motion_native_sample native_sample;
        make_guest(38U, 0x1234U);
        assert(np2video_motion_guest_detect(
            guest, NP2VIDEO_MOTION_SOURCE_PITCH_BYTES, &guest_sample));
        make_native(&guest_sample);
        for (unsigned y = 544U; y <= 671U; ++y) {
            native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + 400U] = 0U;
        }
        for (unsigned y = 545U; y <= 672U; ++y) {
            for (unsigned x = 400U; x < 401U; ++x) {
                native[(size_t)y * NP2VIDEO_MOTION_NATIVE_WIDTH + x] =
                    guest_sample.bar_color;
            }
        }
        assert(!np2video_motion_native_detect(
            native, NP2VIDEO_MOTION_NATIVE_WIDTH, &guest_sample,
            &native_sample));
        assert(native_sample.status == NP2VIDEO_MOTION_NATIVE_COLUMN_MISMATCH);
    }

    puts("np2video motion oracle contract: PASS");
    (void)sample;
    return 0;
}
