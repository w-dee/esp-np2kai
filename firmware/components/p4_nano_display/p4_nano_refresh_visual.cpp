/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_refresh_visual.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_nano_board/p4_nano_board.hpp"
#include "p4_nano_display/p4_nano_display.hpp"
#include "p4_nano_display/p4_nano_display_pattern.hpp"

namespace {

constexpr std::uint16_t kBlack = 0x0000;
constexpr std::uint16_t kRed = 0xf800;
constexpr std::uint16_t kGreen = 0x07e0;
constexpr std::uint16_t kBlue = 0x001f;
constexpr std::uint16_t kWhite = 0xffff;
constexpr std::uint16_t kYellow = 0xffe0;
constexpr std::uint16_t kCyan = 0x07ff;
constexpr std::uint16_t kMagenta = 0xf81f;
constexpr std::uint16_t kGray25 = 0x4208;
constexpr std::uint16_t kGray50 = 0x8410;
constexpr std::uint16_t kGray75 = 0xbdf7;
constexpr std::size_t kMarkerX = 760U;
constexpr std::size_t kMarkerY = 88U;
constexpr std::size_t kMarkerWidth = 16U;
constexpr std::size_t kMarkerHeight = 16U;
constexpr TickType_t kStaticInspectionTicks = pdMS_TO_TICKS(45000U);
constexpr TickType_t kDynamicInspectionTick = pdMS_TO_TICKS(1000U);
constexpr std::size_t kDynamicMarkerUpdates = 15U;

inline std::size_t index_for(std::size_t x, std::size_t y)
{
    return y * p4_nano_display::kNativeWidth + x;
}

void fill_rect(std::uint16_t *pixels, std::size_t x, std::size_t y,
               std::size_t width, std::size_t height, std::uint16_t color)
{
    for (std::size_t row = y; row < y + height; ++row) {
        for (std::size_t column = x; column < x + width; ++column) {
            pixels[index_for(column, row)] = color;
        }
    }
}

void draw_border(std::uint16_t *pixels)
{
    /* Preserve the existing four-edge/four-corner geometry, then make the
     * edge colors unambiguous at points outside the corner blocks. */
    fill_rect(pixels, 96U, 0U, 608U, 8U, kRed);
    fill_rect(pixels, 96U, p4_nano_display::kNativeHeight - 8U,
              608U, 8U, kBlue);
    fill_rect(pixels, 0U, 96U, 8U, 1088U, kWhite);
    fill_rect(pixels, p4_nano_display::kNativeWidth - 8U, 96U,
              8U, 1088U, kGreen);
}

void draw_primary_regions(std::uint16_t *pixels)
{
    fill_rect(pixels, 96U, 128U, 160U, 128U, kRed);
    fill_rect(pixels, 272U, 128U, 160U, 128U, kGreen);
    fill_rect(pixels, 448U, 128U, 160U, 128U, kBlue);
    fill_rect(pixels, 624U, 128U, 128U, 128U, kWhite);
    fill_rect(pixels, 96U, 272U, 160U, 80U, kBlack);
    fill_rect(pixels, 272U, 272U, 160U, 80U, kGray25);
    fill_rect(pixels, 448U, 272U, 160U, 80U, kGray50);
    fill_rect(pixels, 624U, 272U, 128U, 80U, kGray75);
}

void draw_one_pixel_lines(std::uint16_t *pixels)
{
    for (std::size_t x = 96U; x < 752U; ++x) {
        pixels[index_for(x, 376U)] = (x & 1U) != 0U ? kWhite : kBlack;
        pixels[index_for(x, 378U)] = (x & 1U) != 0U ? kBlack : kWhite;
    }
    for (std::size_t y = 400U; y < 528U; ++y) {
        pixels[index_for(376U, y)] = (y & 1U) != 0U ? kWhite : kBlack;
        pixels[index_for(378U, y)] = (y & 1U) != 0U ? kBlack : kWhite;
    }
}

void draw_checkerboard(std::uint16_t *pixels)
{
    constexpr std::size_t kCell = 8U;
    for (std::size_t y = 400U; y < 528U; y += kCell) {
        for (std::size_t x = 96U; x < 352U; x += kCell) {
            const std::uint16_t color = (((x - 96U) / kCell +
                                          (y - 400U) / kCell) & 1U) != 0U
                                            ? kWhite
                                            : kBlack;
            fill_rect(pixels, x, y, kCell, kCell, color);
        }
    }
}

void draw_rectangles_and_orientation(std::uint16_t *pixels)
{
    fill_rect(pixels, 96U, 560U, 128U, 64U, kMagenta);
    fill_rect(pixels, 240U, 552U, 192U, 96U, kCyan);
    fill_rect(pixels, 448U, 544U, 256U, 112U, kYellow);
    fill_rect(pixels, 112U, 680U, 592U, 8U, kWhite);
    fill_rect(pixels, 112U, 688U, 8U, 128U, kWhite);
    fill_rect(pixels, 120U, 808U, 48U, 8U, kWhite);
    /* An asymmetric L plus a single lower-right pixel gives orientation. */
    fill_rect(pixels, 688U, 96U, 8U, 64U, kWhite);
    fill_rect(pixels, 688U, 96U, 64U, 8U, kWhite);
    fill_rect(pixels, 744U, 152U, 8U, 8U, kRed);
}

} // namespace

namespace p4_nano_display {

bool fill_refresh_visual_pattern(std::uint16_t *pixels,
                                 std::size_t pixel_count) noexcept
{
    if (!fill_static_pattern(pixels, pixel_count)) {
        return false;
    }
    draw_border(pixels);
    draw_primary_regions(pixels);
    draw_checkerboard(pixels);
    draw_one_pixel_lines(pixels);
    draw_rectangles_and_orientation(pixels);
    return update_refresh_visual_marker(pixels, pixel_count, false);
}

std::uint32_t refresh_visual_pattern_crc32(const std::uint16_t *pixels,
                                           std::size_t pixel_count) noexcept
{
    if (pixels == nullptr || pixel_count < kNativePixelCount) {
        return 0U;
    }
    return crc32(reinterpret_cast<const std::uint8_t *>(pixels),
                 kNativeFramebufferBytes);
}

bool validate_refresh_visual_pattern(const std::uint16_t *pixels,
                                     std::size_t pixel_count) noexcept
{
    if (pixels == nullptr || pixel_count < kNativePixelCount) {
        return false;
    }
    const auto pixel = [pixels](std::size_t x, std::size_t y) {
        return pixels[index_for(x, y)];
    };
    return pixel(100U, 2U) == kRed &&
           pixel(100U, kNativeHeight - 2U) == kBlue &&
           pixel(2U, 100U) == kWhite &&
           pixel(kNativeWidth - 2U, 100U) == kGreen &&
           pixel(128U, 160U) == kRed &&
           pixel(304U, 160U) == kGreen &&
           pixel(480U, 160U) == kBlue &&
           pixel(672U, 160U) == kWhite &&
           pixel(128U, 312U) == kBlack &&
           pixel(304U, 312U) == kGray25 &&
           pixel(480U, 312U) == kGray50 &&
           pixel(656U, 312U) == kGray75 &&
           pixel(100U, 404U) == kBlack &&
           pixel(104U, 404U) == kWhite &&
           pixel(376U, 404U) == kBlack &&
           pixel(378U, 404U) == kWhite &&
           pixel(688U, 96U) == kWhite &&
           pixel(744U, 152U) == kRed &&
           pixel(kMarkerX, kMarkerY) == kBlack;
}

bool update_refresh_visual_marker(std::uint16_t *pixels,
                                  std::size_t pixel_count,
                                  bool highlighted) noexcept
{
    if (pixels == nullptr || pixel_count < kNativePixelCount ||
        kMarkerX + kMarkerWidth > kNativeWidth ||
        kMarkerY + kMarkerHeight > kNativeHeight) {
        return false;
    }
    fill_rect(pixels, kMarkerX, kMarkerY, kMarkerWidth, kMarkerHeight,
              highlighted ? kYellow : kBlack);
    return true;
}

esp_err_t run_refresh_visual()
{
    DisplaySession resources;
    const RefreshVisualConfig &config = kRefreshVisualConfig;
    std::printf(
        "P4_NANO_REFRESH_VISUAL_CONFIG mode=%s dpi_source=%s "
        "requested_dpi_mhz=%.9f predicted_divider=%" PRIu32 " "
        "predicted_real_dpi_mhz=%.9f htotal=%zu vtotal=%zu "
        "predicted_refresh_hz=%.9f requested_lane_mbps=%.0f lanes=2 "
        "format=RGB565 num_fbs=1\n",
        config.mode, config.dpi_source_name,
        static_cast<double>(config.requested_dpi_mhz),
        config.predicted_divider, static_cast<double>(config.predicted_real_dpi_mhz),
        config.htotal, config.vtotal, static_cast<double>(config.predicted_refresh_hz),
        static_cast<double>(config.requested_lane_mbps));
    std::fflush(stdout);

    esp_err_t ret = display_session_initialize(&resources);
    if (ret != ESP_OK) {
        std::printf("P4_NANO_REFRESH_VISUAL_SOFTWARE_RESULT=FAIL error=%s\n",
                    esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200U));
    bool software_ok = fill_refresh_visual_pattern(resources.framebuffer,
                                                   kNativePixelCount);
    ret = display_session_sync_framebuffer(&resources);
    if (ret != ESP_OK) {
        software_ok = false;
    }
    const std::uint32_t pattern_crc = refresh_visual_pattern_crc32(
        resources.framebuffer, kNativePixelCount);
    if (!validate_refresh_visual_pattern(resources.framebuffer,
                                         kNativePixelCount)) {
        software_ok = false;
    }
    std::printf("P4_NANO_REFRESH_VISUAL_PATTERN crc=0x%08" PRIx32
                " validation=%s\n",
                pattern_crc, software_ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!software_ok) {
        const esp_err_t backlight_off_ret =
            p4_nano_board::display_backlight_set(0U);
        std::printf("P4_NANO_REFRESH_VISUAL_BACKLIGHT_OFF=%s\n",
                    backlight_off_ret == ESP_OK ? "PASS" : "FAIL");
        const esp_err_t cleanup_ret = display_session_cleanup(&resources);
        std::printf("P4_NANO_REFRESH_VISUAL_CLEANUP=%s\n",
                    cleanup_ret == ESP_OK ? "PASS" : "FAIL");
        std::printf("P4_NANO_REFRESH_VISUAL_SOFTWARE_RESULT=FAIL\n");
        std::printf("P4_NANO_REFRESH_VISUAL_HUMAN_VERDICT_REQUIRED=1\n");
        std::fflush(stdout);
        return ESP_FAIL;
    }

    ret = p4_nano_board::display_backlight_set(
        p4_nano_board::kBacklightConservative);
    const bool backlight_on = ret == ESP_OK;
    std::printf("P4_NANO_REFRESH_VISUAL_BACKLIGHT_ON=%s\n",
                backlight_on ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!backlight_on) {
        const esp_err_t backlight_off_ret =
            p4_nano_board::display_backlight_set(0U);
        std::printf("P4_NANO_REFRESH_VISUAL_BACKLIGHT_OFF=%s\n",
                    backlight_off_ret == ESP_OK ? "PASS" : "FAIL");
        const esp_err_t cleanup_ret = display_session_cleanup(&resources);
        std::printf("P4_NANO_REFRESH_VISUAL_CLEANUP=%s\n",
                    cleanup_ret == ESP_OK ? "PASS" : "FAIL");
        std::printf("P4_NANO_REFRESH_VISUAL_SOFTWARE_RESULT=FAIL\n");
        std::printf("P4_NANO_REFRESH_VISUAL_HUMAN_VERDICT_REQUIRED=1\n");
        std::fflush(stdout);
        return ret;
    }

    display_session_reset_vsync(&resources);
    std::printf("P4_NANO_REFRESH_VISUAL_INSPECTION_BEGIN phase=static duration_s=45\n");
    std::fflush(stdout);
    vTaskDelay(kStaticInspectionTicks);

    std::printf("P4_NANO_REFRESH_VISUAL_INSPECTION_PHASE phase=dynamic_marker duration_s=15 nominal_toggle_hz=1\n");
    std::fflush(stdout);
    for (std::size_t update = 0U; update < kDynamicMarkerUpdates; ++update) {
        const bool highlighted = (update & 1U) != 0U;
        if (!update_refresh_visual_marker(resources.framebuffer,
                                          kNativePixelCount, highlighted) ||
            display_session_sync_framebuffer(&resources) != ESP_OK) {
            software_ok = false;
        }
        vTaskDelay(kDynamicInspectionTick);
    }
    std::printf("P4_NANO_REFRESH_VISUAL_INSPECTION_END duration_s=60\n");
    std::fflush(stdout);

    VsyncStatsSnapshot snapshot;
    display_session_snapshot_vsync(&resources, &snapshot);
    const double average_period_us = snapshot.period_count == 0U
                                         ? 0.0
                                         : static_cast<double>(
                                               snapshot.period_total_us) /
                                               static_cast<double>(
                                                   snapshot.period_count);
    const double measured_refresh_hz = average_period_us == 0.0
                                           ? 0.0
                                           : 1000000.0 / average_period_us;
    const double refresh_delta_hz = measured_refresh_hz -
                                    static_cast<double>(config.predicted_refresh_hz);
    const double refresh_error_percent =
        config.predicted_refresh_hz == 0.0F
            ? 0.0
            : refresh_delta_hz * 100.0 /
                  static_cast<double>(config.predicted_refresh_hz);
    const bool count_consistent = snapshot.period_count + 1U ==
                                  snapshot.callback_count;
    const bool vsync_valid = snapshot.callback_registered &&
                             snapshot.callback_count > 1U && count_consistent;
    software_ok = software_ok && vsync_valid;
    std::printf(
        "P4_NANO_REFRESH_VISUAL_VSYNC callback_count=%" PRIu32
        " period_count=%" PRIu32 " average_period_us=%.3f "
        "min_period_us=%" PRIu32 " max_period_us=%" PRIu32
        " measured_refresh_hz=%.9f count_consistency=%s callback_registered=%s\n",
        snapshot.callback_count, snapshot.period_count, average_period_us,
        snapshot.period_min_us, snapshot.period_max_us,
        measured_refresh_hz, count_consistent ? "PASS" : "FAIL",
        snapshot.callback_registered ? "PASS" : "FAIL");
    std::printf(
        "P4_NANO_REFRESH_VISUAL_VSYNC_RESULT predicted_refresh_hz=%.9f "
        "measured_minus_predicted_hz=%.9f percent_error=%.6f\n",
        static_cast<double>(config.predicted_refresh_hz), refresh_delta_hz,
        refresh_error_percent);

    const esp_err_t backlight_off_ret = p4_nano_board::display_backlight_set(0U);
    std::printf("P4_NANO_REFRESH_VISUAL_BACKLIGHT_OFF=%s\n",
                backlight_off_ret == ESP_OK ? "PASS" : "FAIL");
    std::fflush(stdout);
    const esp_err_t cleanup_ret = display_session_cleanup(&resources);
    std::printf("P4_NANO_REFRESH_VISUAL_CLEANUP=%s\n",
                cleanup_ret == ESP_OK ? "PASS" : "FAIL");
    if (backlight_off_ret != ESP_OK || cleanup_ret != ESP_OK) {
        software_ok = false;
    }
    std::printf("P4_NANO_REFRESH_VISUAL_SOFTWARE_RESULT=%s\n",
                software_ok ? "PASS" : "FAIL");
    std::printf("P4_NANO_REFRESH_VISUAL_HUMAN_VERDICT_REQUIRED=1\n");
    std::fflush(stdout);
    return software_ok ? ESP_OK : ESP_FAIL;
}

} // namespace p4_nano_display
