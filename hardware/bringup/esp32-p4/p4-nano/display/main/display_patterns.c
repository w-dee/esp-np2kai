#include "display_patterns.h"

#include <string.h>

enum {
    RGB565_BLACK = 0x0000,
    RGB565_BLUE = 0x001f,
    RGB565_GREEN = 0x07e0,
    RGB565_CYAN = 0x07ff,
    RGB565_RED = 0xf800,
    RGB565_MAGENTA = 0xf81f,
    RGB565_YELLOW = 0xffe0,
    RGB565_WHITE = 0xffff,
};

static uint16_t pattern_pixel(display_pattern_kind_t pattern, uint32_t x, uint32_t y)
{
    static const uint16_t bars[] = {
        RGB565_WHITE, RGB565_YELLOW, RGB565_CYAN, RGB565_GREEN,
        RGB565_MAGENTA, RGB565_RED, RGB565_BLUE, RGB565_BLACK,
    };

    switch (pattern) {
    case DISPLAY_PATTERN_BLACK:
        return RGB565_BLACK;
    case DISPLAY_PATTERN_RED:
        return RGB565_RED;
    case DISPLAY_PATTERN_GREEN:
        return RGB565_GREEN;
    case DISPLAY_PATTERN_BLUE:
        return RGB565_BLUE;
    case DISPLAY_PATTERN_BARS:
        return bars[(x * 8U) / DISPLAY_PATTERN_WIDTH];
    case DISPLAY_PATTERN_CHECKER:
        return (((x / 32U) + (y / 32U)) & 1U) != 0U ? RGB565_WHITE : RGB565_BLACK;
    case DISPLAY_PATTERN_BORDER: {
        uint16_t color = RGB565_BLACK;
        const bool outer_border = x == 0U || y == 0U ||
                                  x == DISPLAY_PATTERN_WIDTH - 1U ||
                                  y == DISPLAY_PATTERN_HEIGHT - 1U;
        const bool inset_border = (x >= 8U && x <= 10U) ||
                                  (x >= DISPLAY_PATTERN_WIDTH - 11U &&
                                   x <= DISPLAY_PATTERN_WIDTH - 9U) ||
                                  (y >= 8U && y <= 10U) ||
                                  (y >= DISPLAY_PATTERN_HEIGHT - 11U &&
                                   y <= DISPLAY_PATTERN_HEIGHT - 9U);
        if (outer_border) {
            color = RGB565_WHITE;
        }
        if (inset_border) {
            color = RGB565_YELLOW;
        }

        const bool top = y < 32U;
        const bool bottom = y >= DISPLAY_PATTERN_HEIGHT - 32U;
        const bool left = x < 32U;
        const bool right = x >= DISPLAY_PATTERN_WIDTH - 32U;
        if (top && left) {
            color = RGB565_RED;
        } else if (top && right) {
            color = RGB565_GREEN;
        } else if (bottom && left) {
            color = RGB565_BLUE;
        } else if (bottom && right) {
            color = RGB565_YELLOW;
        }

        const bool center_vertical = x >= DISPLAY_PATTERN_WIDTH / 2U - 2U &&
                                     x <= DISPLAY_PATTERN_WIDTH / 2U + 2U;
        const bool center_horizontal = y >= DISPLAY_PATTERN_HEIGHT / 2U - 2U &&
                                       y <= DISPLAY_PATTERN_HEIGHT / 2U + 2U;
        if (center_vertical || center_horizontal) {
            color = RGB565_CYAN;
        }
        return color;
    }
    default:
        return RGB565_BLACK;
    }
}

bool display_pattern_fill(display_pattern_kind_t pattern, uint16_t *pixels, size_t pixel_count)
{
    if (pixels == NULL || pixel_count != DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT ||
        pattern >= DISPLAY_PATTERN_COUNT) {
        return false;
    }

    for (uint32_t y = 0; y < DISPLAY_PATTERN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < DISPLAY_PATTERN_WIDTH; ++x) {
            pixels[(size_t)y * DISPLAY_PATTERN_WIDTH + x] = pattern_pixel(pattern, x, y);
        }
    }
    return true;
}

bool display_pattern_verify_representative(display_pattern_kind_t pattern, const uint16_t *pixels,
                                           size_t pixel_count)
{
    if (pixels == NULL || pixel_count != DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT ||
        pattern >= DISPLAY_PATTERN_COUNT) {
        return false;
    }

    static const uint32_t coordinates[][2] = {
        {0U, 0U},
        {DISPLAY_PATTERN_WIDTH / 2U, DISPLAY_PATTERN_HEIGHT / 2U},
        {DISPLAY_PATTERN_WIDTH - 1U, DISPLAY_PATTERN_HEIGHT - 1U},
        {123U, 777U},
    };
    for (size_t i = 0; i < sizeof(coordinates) / sizeof(coordinates[0]); ++i) {
        const uint32_t x = coordinates[i][0];
        const uint32_t y = coordinates[i][1];
        if (pixels[(size_t)y * DISPLAY_PATTERN_WIDTH + x] != pattern_pixel(pattern, x, y)) {
            return false;
        }
    }
    return true;
}

uint32_t display_pattern_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    if (data == NULL && length != 0U) {
        return 0U;
    }

    while (length-- > 0U) {
        crc ^= *data++;
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
    }
    return crc ^ UINT32_MAX;
}

const char *display_pattern_name(display_pattern_kind_t pattern)
{
    static const char *const names[DISPLAY_PATTERN_COUNT] = {
        "BLACK", "RED", "GREEN", "BLUE", "BARS", "CHECKER", "BORDER",
    };
    return pattern < DISPLAY_PATTERN_COUNT ? names[pattern] : "UNKNOWN";
}
