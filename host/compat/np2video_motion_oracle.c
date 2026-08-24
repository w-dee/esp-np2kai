#include "np2video_motion_oracle.h"

#include <string.h>

static const unsigned k_scanlines[] = {0U, 100U, 200U, 300U, 399U};
static const unsigned k_native_columns[] = {0U, 200U, 400U, 600U, 799U};

static uint16_t guest_pixel(const uint8_t *frame, size_t pitch_bytes,
                            unsigned x, unsigned y)
{
    const uint8_t *pixel = frame + (size_t)y * pitch_bytes + (size_t)x * 2U;
    return (uint16_t)pixel[0] | ((uint16_t)pixel[1] << 8);
}

static bool guest_geometry_valid(const uint8_t *frame, size_t pitch_bytes,
                                 const np2video_motion_guest_sample *sample)
{
    return frame != NULL && sample != NULL &&
           pitch_bytes >= NP2VIDEO_MOTION_SOURCE_PITCH_BYTES;
}

const char *np2video_motion_guest_status_name(
    np2video_motion_guest_status status)
{
    switch (status) {
        case NP2VIDEO_MOTION_GUEST_VALID: return "VALID";
        case NP2VIDEO_MOTION_GUEST_NO_BAR: return "NO_BAR";
        case NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS: return "MULTIPLE_RUNS";
        case NP2VIDEO_MOTION_GUEST_WRONG_WIDTH: return "WRONG_WIDTH";
        case NP2VIDEO_MOTION_GUEST_INVALID_ALIGNMENT: return "INVALID_ALIGNMENT";
        case NP2VIDEO_MOTION_GUEST_ROW_MISMATCH: return "ROW_MISMATCH";
        case NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH: return "COLOR_MISMATCH";
        case NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY: return "INVALID_GEOMETRY";
    }
    return "INVALID_GEOMETRY";
}

const char *np2video_motion_native_status_name(
    np2video_motion_native_status status)
{
    switch (status) {
        case NP2VIDEO_MOTION_NATIVE_VALID: return "VALID";
        case NP2VIDEO_MOTION_NATIVE_NO_BAR: return "NATIVE_NO_BAR";
        case NP2VIDEO_MOTION_NATIVE_MULTIPLE_RUNS: return "NATIVE_MULTIPLE_RUNS";
        case NP2VIDEO_MOTION_NATIVE_WRONG_SIZE: return "NATIVE_WRONG_SIZE";
        case NP2VIDEO_MOTION_NATIVE_WRONG_COLOR: return "NATIVE_WRONG_COLOR";
        case NP2VIDEO_MOTION_NATIVE_POSITION_MISMATCH:
            return "NATIVE_POSITION_MISMATCH";
        case NP2VIDEO_MOTION_NATIVE_COLUMN_MISMATCH:
            return "NATIVE_COLUMN_MISMATCH";
        case NP2VIDEO_MOTION_NATIVE_INVALID_GEOMETRY:
            return "NATIVE_INVALID_GEOMETRY";
    }
    return "NATIVE_INVALID_GEOMETRY";
}

bool np2video_motion_guest_detect(
    const uint8_t *frame, size_t pitch_bytes,
    np2video_motion_guest_sample *sample)
{
    unsigned scanline_index;
    unsigned reference_x = 0U;
    uint16_t reference_color = 0U;
    bool reference_set = false;

    if (!guest_geometry_valid(frame, pitch_bytes, sample)) {
        if (sample != NULL) {
            memset(sample, 0, sizeof(*sample));
            sample->status = NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY;
        }
        return false;
    }
    memset(sample, 0, sizeof(*sample));
    sample->status = NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY;

    for (scanline_index = 0U;
         scanline_index < sizeof(k_scanlines) / sizeof(k_scanlines[0]);
         ++scanline_index) {
        const unsigned y = k_scanlines[scanline_index];
        unsigned run_count = 0U;
        unsigned run_start = 0U;
        unsigned run_end = 0U;
        unsigned x;
        bool in_run = false;

        for (x = 0U; x <= NP2VIDEO_MOTION_SOURCE_WIDTH; ++x) {
            const bool nonzero =
                x < NP2VIDEO_MOTION_SOURCE_WIDTH &&
                guest_pixel(frame, pitch_bytes, x, y) != 0U;
            if (nonzero && !in_run) {
                in_run = true;
                run_start = x;
                ++run_count;
            }
            if (!nonzero && in_run) {
                in_run = false;
                run_end = x - 1U;
            }
        }
        if (run_count == 0U) {
            sample->status = NP2VIDEO_MOTION_GUEST_NO_BAR;
            return false;
        }
        if (run_count != 1U) {
            sample->status = NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS;
            return false;
        }
        if (run_end - run_start + 1U != NP2VIDEO_MOTION_BAR_WIDTH_PIXELS) {
            sample->status = NP2VIDEO_MOTION_GUEST_WRONG_WIDTH;
            return false;
        }
        if ((run_start & 7U) != 0U) {
            sample->status = NP2VIDEO_MOTION_GUEST_INVALID_ALIGNMENT;
            return false;
        }
        if (run_start / 8U < NP2VIDEO_MOTION_BAR_MIN_POS ||
            run_start / 8U > NP2VIDEO_MOTION_BAR_MAX_POS) {
            sample->status = NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY;
            return false;
        }
        {
            const uint16_t color = guest_pixel(frame, pitch_bytes,
                                                run_start, y);
            for (x = run_start; x <= run_end; ++x) {
                if (guest_pixel(frame, pitch_bytes, x, y) != color) {
                    sample->status = NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH;
                    return false;
                }
            }
            if (reference_set && color != reference_color) {
                sample->status = NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH;
                return false;
            }
            if (reference_set && run_start != reference_x) {
                sample->status = NP2VIDEO_MOTION_GUEST_ROW_MISMATCH;
                return false;
            }
            if (!reference_set) {
                reference_set = true;
                reference_x = run_start;
                reference_color = color;
                sample->bar_pos = run_start / 8U;
                sample->x_start = run_start;
                sample->x_end = run_end;
                sample->bar_color = color;
            }
        }
    }
    sample->status = NP2VIDEO_MOTION_GUEST_VALID;
    return true;
}

bool np2video_motion_expected_native_band(uint32_t bar_pos,
                                          uint32_t *y_start,
                                          uint32_t *y_end)
{
    if (y_start == NULL || y_end == NULL ||
        bar_pos < NP2VIDEO_MOTION_BAR_MIN_POS ||
        bar_pos > NP2VIDEO_MOTION_BAR_MAX_POS) {
        return false;
    }
    *y_start = 1152U - 16U * bar_pos;
    *y_end = 1279U - 16U * bar_pos;
    return true;
}

static uint16_t native_pixel(const uint16_t *frame, size_t pitch_pixels,
                             unsigned x, unsigned y)
{
    return frame[(size_t)y * pitch_pixels + x];
}

bool np2video_motion_native_detect(
    const uint16_t *frame, size_t pitch_pixels,
    const np2video_motion_guest_sample *guest,
    np2video_motion_native_sample *sample)
{
    unsigned column_index;
    uint32_t expected_start;
    uint32_t expected_end;
    bool reference_set = false;
    uint32_t reference_start = 0U;
    uint32_t reference_end = 0U;

    if (sample != NULL) {
        memset(sample, 0, sizeof(*sample));
        sample->status = NP2VIDEO_MOTION_NATIVE_INVALID_GEOMETRY;
    }
    if (frame == NULL || sample == NULL || guest == NULL ||
        guest->status != NP2VIDEO_MOTION_GUEST_VALID ||
        pitch_pixels < NP2VIDEO_MOTION_NATIVE_WIDTH ||
        !np2video_motion_expected_native_band(guest->bar_pos,
                                              &expected_start, &expected_end)) {
        return false;
    }
    for (column_index = 0U;
         column_index < sizeof(k_native_columns) / sizeof(k_native_columns[0]);
         ++column_index) {
        const unsigned x = k_native_columns[column_index];
        unsigned run_count = 0U;
        unsigned run_start = 0U;
        unsigned run_end = 0U;
        unsigned y;
        bool in_run = false;
        for (y = 0U; y <= NP2VIDEO_MOTION_NATIVE_HEIGHT; ++y) {
            const bool nonzero =
                y < NP2VIDEO_MOTION_NATIVE_HEIGHT &&
                native_pixel(frame, pitch_pixels, x, y) != 0U;
            if (nonzero && !in_run) {
                in_run = true;
                run_start = y;
                ++run_count;
            }
            if (!nonzero && in_run) {
                in_run = false;
                run_end = y - 1U;
            }
        }
        if (run_count == 0U) {
            sample->status = NP2VIDEO_MOTION_NATIVE_NO_BAR;
            return false;
        }
        if (run_count != 1U) {
            sample->status = NP2VIDEO_MOTION_NATIVE_MULTIPLE_RUNS;
            return false;
        }
        if (run_end - run_start + 1U != NP2VIDEO_MOTION_NATIVE_BAR_HEIGHT) {
            sample->status = NP2VIDEO_MOTION_NATIVE_WRONG_SIZE;
            return false;
        }
        for (y = run_start; y <= run_end; ++y) {
            if (native_pixel(frame, pitch_pixels, x, y) != guest->bar_color) {
                sample->status = NP2VIDEO_MOTION_NATIVE_WRONG_COLOR;
                return false;
            }
        }
        for (y = 0U; y < NP2VIDEO_MOTION_NATIVE_HEIGHT; ++y) {
            if (y < run_start || y > run_end) {
                if (native_pixel(frame, pitch_pixels, x, y) != 0U) {
                    sample->status = NP2VIDEO_MOTION_NATIVE_WRONG_COLOR;
                    return false;
                }
            }
        }
        if (reference_set &&
            (run_start != reference_start || run_end != reference_end)) {
            sample->status = NP2VIDEO_MOTION_NATIVE_COLUMN_MISMATCH;
            return false;
        }
        if (run_start != expected_start || run_end != expected_end) {
            sample->status = NP2VIDEO_MOTION_NATIVE_POSITION_MISMATCH;
            return false;
        }
        if (!reference_set) {
            reference_set = true;
            reference_start = run_start;
            reference_end = run_end;
        }
    }
    sample->status = NP2VIDEO_MOTION_NATIVE_VALID;
    sample->y_start = reference_start;
    sample->y_end = reference_end;
    return true;
}

uint32_t np2video_motion_expected_bar_pos(uint32_t update_count)
{
    const uint32_t remainder = update_count % 120U;
    const uint32_t distance = remainder < (120U - remainder)
                                  ? remainder : (120U - remainder);
    return NP2VIDEO_MOTION_BAR_MIN_POS + distance;
}
