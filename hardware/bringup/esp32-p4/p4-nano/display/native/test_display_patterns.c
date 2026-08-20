#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "display_patterns.h"

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

static const uint32_t expected_crc[DISPLAY_PATTERN_COUNT] = {
    0xb483d8ccU,
    0x67196861U,
    0x43d010a4U,
    0x7743f398U,
    0x22b23526U,
    0xfd8b8a01U,
    0x446766bcU,
};

static uint16_t pixel(const uint16_t *fb, uint32_t x, uint32_t y)
{
    return fb[(size_t)y * DISPLAY_PATTERN_WIDTH + x];
}

int main(void)
{
    assert(DISPLAY_PATTERN_STRIDE == 1600U);
    assert(DISPLAY_PATTERN_FRAMEBUFFER_BYTES == 2048000U);

    uint16_t *fb = malloc(DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
    assert(fb != NULL);

    static const uint16_t solid_colors[] = {
        RGB565_BLACK, RGB565_RED, RGB565_GREEN, RGB565_BLUE,
    };
    for (display_pattern_kind_t pattern = DISPLAY_PATTERN_BLACK;
         pattern < DISPLAY_PATTERN_COUNT; ++pattern) {
        assert(display_pattern_fill(pattern, fb, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT));
        assert(display_pattern_verify_representative(
            pattern, fb, DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT));
        const uint32_t crc = display_pattern_crc32(
            (const uint8_t *)fb, DISPLAY_PATTERN_FRAMEBUFFER_BYTES);
        printf("%s crc32=0x%08x\n", display_pattern_name(pattern), crc);
        assert(expected_crc[pattern] != 0U);
        assert(crc == expected_crc[pattern]);

        if (pattern <= DISPLAY_PATTERN_BLUE) {
            assert(pixel(fb, 0U, 0U) == solid_colors[pattern]);
            assert(pixel(fb, DISPLAY_PATTERN_WIDTH / 2U, DISPLAY_PATTERN_HEIGHT / 2U) ==
                   solid_colors[pattern]);
        }
    }

    assert(display_pattern_fill(DISPLAY_PATTERN_BARS, fb,
                                DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT));
    static const uint16_t bar_colors[] = {
        RGB565_WHITE, RGB565_YELLOW, RGB565_CYAN, RGB565_GREEN,
        RGB565_MAGENTA, RGB565_RED, RGB565_BLUE, RGB565_BLACK,
    };
    for (uint32_t bar = 0; bar < 8U; ++bar) {
        assert(pixel(fb, bar * 100U + 50U, 600U) == bar_colors[bar]);
    }

    assert(display_pattern_fill(DISPLAY_PATTERN_CHECKER, fb,
                                DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT));
    assert(pixel(fb, 0U, 0U) == RGB565_BLACK);
    assert(pixel(fb, 31U, 31U) == RGB565_BLACK);
    assert(pixel(fb, 32U, 0U) == RGB565_WHITE);
    assert(pixel(fb, 0U, 32U) == RGB565_WHITE);

    assert(display_pattern_fill(DISPLAY_PATTERN_BORDER, fb,
                                DISPLAY_PATTERN_WIDTH * DISPLAY_PATTERN_HEIGHT));
    assert(pixel(fb, 100U, 0U) == RGB565_WHITE);
    assert(pixel(fb, 100U, 8U) == RGB565_YELLOW);
    assert(pixel(fb, 10U, 10U) == RGB565_RED);
    assert(pixel(fb, DISPLAY_PATTERN_WIDTH - 10U, 10U) == RGB565_GREEN);
    assert(pixel(fb, 10U, DISPLAY_PATTERN_HEIGHT - 10U) == RGB565_BLUE);
    assert(pixel(fb, DISPLAY_PATTERN_WIDTH - 10U, DISPLAY_PATTERN_HEIGHT - 10U) == RGB565_YELLOW);
    assert(pixel(fb, DISPLAY_PATTERN_WIDTH / 2U, DISPLAY_PATTERN_HEIGHT / 2U) == RGB565_CYAN);

    free(fb);
    puts("display pattern native test: PASS");
    return 0;
}
