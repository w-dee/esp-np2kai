#ifndef NP2VIDEO_MOTION_ORACLE_H
#define NP2VIDEO_MOTION_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP2VIDEO_MOTION_SOURCE_WIDTH 640U
#define NP2VIDEO_MOTION_SOURCE_HEIGHT 400U
#define NP2VIDEO_MOTION_SOURCE_PITCH_BYTES 1280U
#define NP2VIDEO_MOTION_NATIVE_WIDTH 800U
#define NP2VIDEO_MOTION_NATIVE_HEIGHT 1280U
#define NP2VIDEO_MOTION_BAR_MIN_POS 8U
#define NP2VIDEO_MOTION_BAR_MAX_POS 68U
#define NP2VIDEO_MOTION_BAR_WIDTH_PIXELS 64U
#define NP2VIDEO_MOTION_NATIVE_BAR_HEIGHT 128U

typedef enum {
    NP2VIDEO_MOTION_GUEST_VALID = 0,
    NP2VIDEO_MOTION_GUEST_NO_BAR,
    NP2VIDEO_MOTION_GUEST_MULTIPLE_RUNS,
    NP2VIDEO_MOTION_GUEST_WRONG_WIDTH,
    NP2VIDEO_MOTION_GUEST_INVALID_ALIGNMENT,
    NP2VIDEO_MOTION_GUEST_ROW_MISMATCH,
    NP2VIDEO_MOTION_GUEST_COLOR_MISMATCH,
    NP2VIDEO_MOTION_GUEST_INVALID_GEOMETRY
} np2video_motion_guest_status;

typedef struct {
    np2video_motion_guest_status status;
    uint32_t bar_pos;
    uint32_t x_start;
    uint32_t x_end;
    uint16_t bar_color;
} np2video_motion_guest_sample;

typedef enum {
    NP2VIDEO_MOTION_NATIVE_VALID = 0,
    NP2VIDEO_MOTION_NATIVE_NO_BAR,
    NP2VIDEO_MOTION_NATIVE_MULTIPLE_RUNS,
    NP2VIDEO_MOTION_NATIVE_WRONG_SIZE,
    NP2VIDEO_MOTION_NATIVE_WRONG_COLOR,
    NP2VIDEO_MOTION_NATIVE_POSITION_MISMATCH,
    NP2VIDEO_MOTION_NATIVE_COLUMN_MISMATCH,
    NP2VIDEO_MOTION_NATIVE_INVALID_GEOMETRY
} np2video_motion_native_status;

typedef struct {
    np2video_motion_native_status status;
    uint32_t y_start;
    uint32_t y_end;
} np2video_motion_native_sample;

const char *np2video_motion_guest_status_name(
    np2video_motion_guest_status status);
const char *np2video_motion_native_status_name(
    np2video_motion_native_status status);

bool np2video_motion_guest_detect(
    const uint8_t *frame, size_t pitch_bytes,
    np2video_motion_guest_sample *sample);

bool np2video_motion_native_detect(
    const uint16_t *frame, size_t pitch_pixels,
    const np2video_motion_guest_sample *guest,
    np2video_motion_native_sample *sample);

bool np2video_motion_expected_native_band(uint32_t bar_pos,
                                          uint32_t *y_start,
                                          uint32_t *y_end);

uint32_t np2video_motion_expected_bar_pos(uint32_t update_count);

#ifdef __cplusplus
}
#endif

#endif
