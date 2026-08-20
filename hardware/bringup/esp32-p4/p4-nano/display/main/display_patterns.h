#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_PATTERN_WIDTH 800U
#define DISPLAY_PATTERN_HEIGHT 1280U
#define DISPLAY_PATTERN_BYTES_PER_PIXEL 2U
#define DISPLAY_PATTERN_STRIDE (DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_BYTES_PER_PIXEL)
#define DISPLAY_PATTERN_FRAMEBUFFER_BYTES \
    (DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT * DISPLAY_PATTERN_BYTES_PER_PIXEL)

typedef enum {
    DISPLAY_PATTERN_BLACK = 0,
    DISPLAY_PATTERN_RED,
    DISPLAY_PATTERN_GREEN,
    DISPLAY_PATTERN_BLUE,
    DISPLAY_PATTERN_BARS,
    DISPLAY_PATTERN_CHECKER,
    DISPLAY_PATTERN_BORDER,
    DISPLAY_PATTERN_COUNT,
} display_pattern_kind_t;

bool display_pattern_fill(display_pattern_kind_t pattern, uint16_t *pixels, size_t pixel_count);
bool display_pattern_verify_representative(display_pattern_kind_t pattern, const uint16_t *pixels,
                                           size_t pixel_count);
uint32_t display_pattern_crc32(const uint8_t *data, size_t length);
uint32_t display_pattern_expected_crc32(display_pattern_kind_t pattern);
const char *display_pattern_name(display_pattern_kind_t pattern);
