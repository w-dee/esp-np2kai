/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include "p4_nano_display/p4_nano_display.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_lcd_jd9365_10_1.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_nano_board/p4_nano_board.hpp"
#include "p4_nano_display/p4_nano_display_timing_profiles.hpp"
#if defined(P4_NANO_REFRESH_VISUAL_PROFILE)
#include "p4_nano_display/p4_nano_refresh_visual.hpp"
#endif
#include "p4_nano_display/p4_nano_display_pattern.hpp"
#include "p4_nano_display/p4_nano_display_transform.hpp"
#include "p4_nano_display/p4_nano_display_transform_pattern.hpp"
#include "p4_nano_display/p4_nano_jd9365_safe.h"

namespace {

constexpr char kTag[] = "p4_nano_display";
constexpr std::uint8_t kDsiLaneCount = 2;
#if defined(P4_NANO_REFRESH_VISUAL_PROFILE)
constexpr auto kDpiClockSource =
    p4_nano_display::kRefreshVisualConfig.dpi_clock_source;
constexpr float kDsiLaneBitrateMbps =
    p4_nano_display::kRefreshVisualConfig.requested_lane_mbps;
constexpr float kDpiClockMHz =
    p4_nano_display::kRefreshVisualConfig.requested_dpi_mhz;
#elif defined(P4_NANO_BENCHMARK_DISPLAY_REFRESH_BASELINE_PROFILE)
constexpr auto kDpiClockSource =
    p4_nano_display::kDisplayTimingBaseline.dpi_clock_source;
constexpr float kDsiLaneBitrateMbps =
    p4_nano_display::kDisplayTimingBaseline.requested_lane_mbps;
constexpr float kDpiClockMHz =
    p4_nano_display::kDisplayTimingBaseline.requested_dpi_mhz;
#elif defined(P4_NANO_BENCHMARK_DISPLAY_REFRESH_LOWER2_PROFILE)
constexpr auto kDpiClockSource =
    p4_nano_display::kDisplayTimingLower2.dpi_clock_source;
constexpr float kDsiLaneBitrateMbps =
    p4_nano_display::kDisplayTimingLower2.requested_lane_mbps;
constexpr float kDpiClockMHz =
    p4_nano_display::kDisplayTimingLower2.requested_dpi_mhz;
#else
constexpr auto kDpiClockSource = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
constexpr float kDsiLaneBitrateMbps = 1500.0F;
constexpr float kDpiClockMHz = 80.0F;
#endif
constexpr int kDsiLdoChannel = 3;
constexpr int kDsiLdoMillivolts = 2500;
constexpr std::size_t kFramebufferCount = 1;
constexpr TickType_t kPanelStabilizationDelay = pdMS_TO_TICKS(200);
constexpr TickType_t kStaticPatternHold = pdMS_TO_TICKS(5000);
constexpr TickType_t kTransformDiagnosticHold = pdMS_TO_TICKS(30000);

bool IRAM_ATTR dpi_refresh_done_callback(esp_lcd_panel_handle_t,
                                         esp_lcd_dpi_panel_event_data_t *,
                                         void *user_ctx)
{
    auto *stats = static_cast<p4_nano_display::VsyncStats *>(user_ctx);
    if (stats == nullptr || !stats->active.load(std::memory_order_relaxed)) {
        return false;
    }

    const std::uint32_t now_us = static_cast<std::uint32_t>(
        esp_timer_get_time());
    const std::uint32_t sequence =
        stats->callback_count.fetch_add(1U, std::memory_order_relaxed);
    const std::uint32_t previous_us =
        stats->last_timestamp_us.exchange(now_us, std::memory_order_relaxed);
    if (sequence == 0U) {
        return false;
    }

    const std::uint32_t period_us = now_us - previous_us;
    stats->period_count.fetch_add(1U, std::memory_order_relaxed);

    const std::uint32_t prior_low =
        stats->period_total_low.fetch_add(period_us,
                                          std::memory_order_relaxed);
    if (prior_low > UINT32_MAX - period_us) {
        stats->period_total_high.fetch_add(1U, std::memory_order_relaxed);
    }

    const std::uint32_t prior_min =
        stats->period_min_us.load(std::memory_order_relaxed);
    if (period_us < prior_min) {
        stats->period_min_us.store(period_us, std::memory_order_relaxed);
    }
    const std::uint32_t prior_max =
        stats->period_max_us.load(std::memory_order_relaxed);
    if (period_us > prior_max) {
        stats->period_max_us.store(period_us, std::memory_order_relaxed);
    }
    return false;
}

void report_memory(const char *phase)
{
    ESP_LOGI(kTag,
             "PSRAM phase=%s free=%" PRIu32 " largest=%" PRIu32,
             phase,
             static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<std::uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

esp_err_t sync_framebuffer(std::uint16_t *framebuffer)
{
    return esp_cache_msync(
        framebuffer, p4_nano_display::kNativeFramebufferBytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

#if !defined(P4_NANO_RUNTIME_EMU_BACKEND)
void remember_first_error(esp_err_t *first_error, esp_err_t candidate)
{
    if (*first_error == ESP_OK && candidate != ESP_OK) {
        *first_error = candidate;
    }
}
#endif

esp_err_t cleanup(p4_nano_display::DisplaySession *resources)
{
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
    if (resources->framebuffer != nullptr) {
        heap_caps_free(resources->framebuffer);
        resources->framebuffer = nullptr;
    }
    return ESP_OK;
#else
    esp_err_t first_error = ESP_OK;

    resources->vsync.active.store(false, std::memory_order_relaxed);
    resources->vsync_callback_registered = false;

    /* Backlight-off is intentionally attempted before tearing down hardware. */
    remember_first_error(&first_error, p4_nano_board::display_control_safe_off());
    if (resources->panel != nullptr) {
        remember_first_error(&first_error,
                             esp_lcd_panel_del(resources->panel));
        resources->panel = nullptr;
    }
    if (resources->dbi_io != nullptr) {
        remember_first_error(&first_error,
                             esp_lcd_panel_io_del(resources->dbi_io));
        resources->dbi_io = nullptr;
    }
    if (resources->dsi_bus != nullptr) {
        remember_first_error(&first_error,
                             esp_lcd_del_dsi_bus(resources->dsi_bus));
        resources->dsi_bus = nullptr;
    }
    if (resources->ldo != nullptr) {
        remember_first_error(&first_error,
                             esp_ldo_release_channel(resources->ldo));
        resources->ldo = nullptr;
    }
    remember_first_error(&first_error,
                         p4_nano_board::display_control_deinit());
    if (resources->transform_source != nullptr) {
        heap_caps_free(resources->transform_source);
        resources->transform_source = nullptr;
    }
    resources->framebuffer = nullptr;
    return first_error;
#endif
}

esp_err_t fail_stage(p4_nano_display::DisplaySession *resources,
                     const char *stage, esp_err_t error)
{
    ESP_LOGE(kTag, "foundation stage=%s result=FAIL error=%s", stage,
             esp_err_to_name(error));
    const esp_err_t cleanup_error = cleanup(resources);
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(kTag, "cleanup after stage=%s failed: %s", stage,
                 esp_err_to_name(cleanup_error));
    }
    return error;
}

} // namespace

namespace p4_nano_display {

esp_err_t display_session_initialize(DisplaySession *resources)
{
    if (resources == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    resources->vsync.reset();
    resources->vsync_callback_registered = false;
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
    /* esp-emu has no MIPI-DSI panel model.  The bounded validation profile
     * keeps the real Session transform and external native framebuffer while
     * leaving the production hardware path untouched. */
    resources->framebuffer = static_cast<std::uint16_t *>(heap_caps_calloc(
        1U, kNativeFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (resources->framebuffer == nullptr ||
        !esp_ptr_external_ram(resources->framebuffer)) {
        (void)cleanup(resources);
        return ESP_ERR_NO_MEM;
    }
    std::fill_n(resources->framebuffer, kNativePixelCount,
                static_cast<std::uint16_t>(0x0000));
    return ESP_OK;
#else
    esp_err_t ret = p4_nano_board::display_control_init();
    if (ret != ESP_OK) {
        return fail_stage(resources, "shared_i2c_init", ret);
    }
    ESP_LOGI(kTag, "foundation stage=shared_i2c_init result=PASS");
    ret = p4_nano_board::display_control_panel_power_on();
    if (ret != ESP_OK) {
        return fail_stage(resources, "panel_control_power_on", ret);
    }
    ESP_LOGI(kTag, "foundation stage=panel_control_power_on result=PASS");

    esp_ldo_channel_config_t ldo_config{};
    ldo_config.chan_id = kDsiLdoChannel;
    ldo_config.voltage_mv = kDsiLdoMillivolts;
    ret = esp_ldo_acquire_channel(&ldo_config, &resources->ldo);
    if (ret != ESP_OK) {
        return fail_stage(resources, "dsi_phy_ldo", ret);
    }
    ESP_LOGI(kTag, "foundation stage=dsi_phy_ldo result=PASS channel=%d voltage_mv=%d",
             kDsiLdoChannel, kDsiLdoMillivolts);

    esp_lcd_dsi_bus_config_t bus_config{};
    bus_config.bus_id = 0;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.num_data_lanes = kDsiLaneCount;
    bus_config.lane_bit_rate_mbps = kDsiLaneBitrateMbps;
    ret = esp_lcd_new_dsi_bus(&bus_config, &resources->dsi_bus);
    if (ret != ESP_OK) {
        return fail_stage(resources, "dsi_bus", ret);
    }
    ESP_LOGI(kTag, "foundation stage=dsi_bus result=PASS");

    const esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ret = esp_lcd_new_panel_io_dbi(resources->dsi_bus, &dbi_config,
                                   &resources->dbi_io);
    if (ret != ESP_OK) {
        return fail_stage(resources, "dbi_io", ret);
    }
    ESP_LOGI(kTag, "foundation stage=dbi_io result=PASS");

    esp_lcd_dpi_panel_config_t dpi_config{};
#if defined(P4_NANO_REFRESH_VISUAL_PROFILE) || \
    defined(P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE)
    dpi_config.dpi_clk_src = kDpiClockSource;
#else
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
#endif
    dpi_config.dpi_clock_freq_mhz = kDpiClockMHz;
    dpi_config.virtual_channel = 0;
    dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi_config.num_fbs = kFramebufferCount;
    dpi_config.video_timing.h_size = kNativeWidth;
    dpi_config.video_timing.v_size = kNativeHeight;
    dpi_config.video_timing.hsync_back_porch = 20;
    dpi_config.video_timing.hsync_pulse_width = 20;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 10;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_front_porch = 30;
    dpi_config.flags.use_dma2d = false;

    jd9365_vendor_config_t vendor_config{};
    vendor_config.flags.use_mipi_interface = 1;
    vendor_config.mipi_config.dsi_bus = resources->dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.mipi_config.lane_num = kDsiLaneCount;

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ret = p4_nano_esp_lcd_new_panel_jd9365_safe(
        resources->dbi_io, &panel_config, &resources->panel);
    if (ret != ESP_OK) {
        return fail_stage(resources, "jd9365_panel_create", ret);
    }
    ESP_LOGI(kTag, "foundation stage=jd9365_panel_create result=PASS reset_gpio=NC");

    ret = esp_lcd_dpi_panel_get_frame_buffer(
        resources->panel, kFramebufferCount,
        reinterpret_cast<void **>(&resources->framebuffer));
    if (ret != ESP_OK || resources->framebuffer == nullptr) {
        return fail_stage(resources, "dpi_framebuffer_acquire",
                          ret == ESP_OK ? ESP_ERR_NO_MEM : ret);
    }
    ESP_LOGI(kTag, "foundation stage=dpi_framebuffer_acquire result=PASS");
    ESP_LOGI(kTag, "framebuffer pointer=%p bytes=%zu num_fbs=%zu",
             static_cast<void *>(resources->framebuffer),
             kNativeFramebufferBytes, kFramebufferCount);
    report_memory("after_framebuffer_acquire");

    std::fill_n(resources->framebuffer, kNativePixelCount,
                static_cast<std::uint16_t>(0x0000));
    ret = sync_framebuffer(resources->framebuffer);
    if (ret != ESP_OK) {
        return fail_stage(resources, "black_cache_sync", ret);
    }
    ESP_LOGI(kTag, "framebuffer prime=BLACK cache_sync=PASS");
    ESP_LOGI(kTag, "foundation stage=black_cache_sync result=PASS");

    ret = esp_lcd_panel_reset(resources->panel);
    if (ret != ESP_OK) {
        return fail_stage(resources, "panel_reset", ret);
    }
    ret = esp_lcd_panel_init(resources->panel);
    if (ret != ESP_OK) {
        return fail_stage(resources, "panel_init", ret);
    }
    ret = esp_lcd_panel_disp_on_off(resources->panel, true);
    if (ret != ESP_OK) {
        return fail_stage(resources, "panel_display_on", ret);
    }
    const esp_lcd_dpi_panel_event_callbacks_t dpi_callbacks{
        .on_color_trans_done = nullptr,
        .on_refresh_done = dpi_refresh_done_callback,
    };
    ret = esp_lcd_dpi_panel_register_event_callbacks(
        resources->panel, &dpi_callbacks, &resources->vsync);
    if (ret != ESP_OK) {
        return fail_stage(resources, "dpi_refresh_callback", ret);
    }
    resources->vsync_callback_registered = true;
    resources->vsync.active.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "foundation stage=panel_reset_init_display_on result=PASS");
    ESP_LOGI(kTag, "foundation stage=dpi_refresh_callback result=PASS");
    return ESP_OK;
#endif
}

void print_benchmark_display_config() noexcept
{
    const DisplayTimingProfile *config = nullptr;
#if defined(P4_NANO_BENCHMARK_DISPLAY_REFRESH_BASELINE_PROFILE)
    config = &kDisplayTimingBaseline;
#elif defined(P4_NANO_BENCHMARK_DISPLAY_REFRESH_LOWER2_PROFILE)
    config = &kDisplayTimingLower2;
#else
    std::printf(
        "P4_NANO_BENCHMARK_DISPLAY_CONFIG profile=legacy-default "
        "dpi_source=DEFAULT requested_dpi_mhz=80.000000000 "
        "predicted_divider=0 predicted_real_dpi_mhz=80.000000000 "
        "htotal=880 vtotal=1324 predicted_refresh_hz=68.662455000 "
        "requested_lane_mbps=1500 lanes=2 format=RGB565 num_fbs=1\n");
    std::fflush(stdout);
    return;
#endif
    std::printf(
        "P4_NANO_BENCHMARK_DISPLAY_CONFIG profile=%s dpi_source=%s "
        "requested_dpi_mhz=%.9f predicted_divider=%" PRIu32 " "
        "predicted_real_dpi_mhz=%.9f htotal=%zu vtotal=%zu "
        "predicted_refresh_hz=%.9f requested_lane_mbps=%.0f lanes=2 "
        "format=RGB565 num_fbs=1\n",
        config->name, config->dpi_source_name,
        static_cast<double>(config->requested_dpi_mhz), config->predicted_divider,
        static_cast<double>(config->predicted_real_dpi_mhz), config->htotal,
        config->vtotal, static_cast<double>(config->predicted_refresh_hz),
        static_cast<double>(config->requested_lane_mbps));
    std::fflush(stdout);
}

esp_err_t display_session_sync_framebuffer(DisplaySession *session)
{
    if (session == nullptr || session->framebuffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return sync_framebuffer(session->framebuffer);
}

esp_err_t display_session_cleanup(DisplaySession *session)
{
    if (session == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return cleanup(session);
}

void display_session_reset_vsync(DisplaySession *session) noexcept
{
    if (session == nullptr) {
        return;
    }
    const bool was_active =
        session->vsync.active.exchange(false, std::memory_order_acq_rel);
    session->vsync.reset();
    if (was_active) {
        session->vsync.active.store(true, std::memory_order_release);
    }
}

void display_session_snapshot_vsync(const DisplaySession *session,
                                    VsyncStatsSnapshot *snapshot) noexcept
{
    if (snapshot == nullptr) {
        return;
    }
    *snapshot = {};
    if (session == nullptr) {
        return;
    }

    snapshot->callback_count = session->vsync.callback_count.load(
        std::memory_order_acquire);
    snapshot->period_count = session->vsync.period_count.load(
        std::memory_order_acquire);
    const std::uint32_t minimum = session->vsync.period_min_us.load(
        std::memory_order_acquire);
    snapshot->period_min_us = minimum == UINT32_MAX ? 0U : minimum;
    snapshot->period_max_us = session->vsync.period_max_us.load(
        std::memory_order_acquire);
    const std::uint64_t high = session->vsync.period_total_high.load(
        std::memory_order_acquire);
    const std::uint64_t low = session->vsync.period_total_low.load(
        std::memory_order_acquire);
    snapshot->period_total_us = (high << 32U) | low;
    snapshot->callback_registered = session->vsync_callback_registered;
}

esp_err_t run_foundation()
{
    DisplaySession resources;
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    ESP_LOGI(kTag, "chip revision=%d", chip_info.revision);
#if defined(P4_NANO_REFRESH_VISUAL_PROFILE)
    ESP_LOGI(kTag,
             "configuration native=%zux%zu RGB565 lanes=%u bitrate=%.0fMbps/lane "
             "stride=%zu dpi=%.9fMHz num_fbs=%zu predicted_refresh=%.9fHz",
             kNativeWidth, kNativeHeight, kDsiLaneCount,
             static_cast<double>(kDsiLaneBitrateMbps),
             kNativeStrideBytes, static_cast<double>(kDpiClockMHz),
             kFramebufferCount,
             static_cast<double>(p4_nano_display::kRefreshVisualConfig.predicted_refresh_hz));
#else
    ESP_LOGI(kTag,
             "configuration native=%zux%zu RGB565 lanes=%u bitrate=%.0fMbps/lane "
             "stride=%zu dpi=%.0fMHz num_fbs=%zu nominal_refresh=68.66Hz",
             kNativeWidth, kNativeHeight, kDsiLaneCount,
             static_cast<double>(kDsiLaneBitrateMbps),
             kNativeStrideBytes, static_cast<double>(kDpiClockMHz),
             kFramebufferCount);
#endif
    report_memory("before_display_init");

    esp_err_t ret = display_session_initialize(&resources);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(kPanelStabilizationDelay);
    if (!fill_static_pattern(resources.framebuffer, kNativePixelCount)) {
        return fail_stage(&resources, "static_pattern_fill", ESP_ERR_INVALID_SIZE);
    }
    ret = sync_framebuffer(resources.framebuffer);
    if (ret != ESP_OK) {
        return fail_stage(&resources, "static_pattern_cache_sync", ret);
    }
    const std::uint32_t pattern_crc = static_pattern_crc32(
        resources.framebuffer, kNativePixelCount);
    ESP_LOGI(kTag, "static native pattern=PASS geometry=%zux%zu crc32=0x%08" PRIx32,
             kNativeWidth, kNativeHeight, pattern_crc);
    ESP_LOGI(kTag, "foundation stage=static_pattern_cache_sync result=PASS");

    ret = p4_nano_board::display_backlight_set(
        p4_nano_board::kBacklightConservative);
    if (ret != ESP_OK) {
        return fail_stage(&resources, "backlight_enable", ret);
    }
    ESP_LOGI(kTag, "foundation stage=backlight_enable result=PASS");
    ESP_LOGI(kTag, "backlight state=ON value=0x%02x (conservative, not calibrated)",
             p4_nano_board::kBacklightConservative);

    vTaskDelay(kStaticPatternHold);
    const esp_err_t cleanup_error = cleanup(&resources);
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(kTag, "foundation result=FAIL cleanup=%s",
                 esp_err_to_name(cleanup_error));
        return cleanup_error;
    }
    ESP_LOGI(kTag, "foundation result=PASS backlight=OFF");
    return ESP_OK;
}

esp_err_t run_transform_diagnostic(QuarterTurn rotation)
{
    if (rotation != QuarterTurn::Clockwise &&
        rotation != QuarterTurn::CounterClockwise) {
        return ESP_ERR_INVALID_ARG;
    }

    DisplaySession resources;
    const char *rotation_name =
        rotation == QuarterTurn::Clockwise ? "CW" : "CCW";
    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);
    ESP_LOGI(kTag, "P4_NANO_TRANSFORM_DIAGNOSTIC rotation=%s", rotation_name);
    ESP_LOGI(kTag, "chip revision=%d", chip_info.revision);
    ESP_LOGI(kTag,
             "configuration native=%zux%zu RGB565 lanes=%u bitrate=%.0fMbps/lane "
             "stride=%zu dpi=%.0fMHz num_fbs=%zu",
             kNativeWidth, kNativeHeight, kDsiLaneCount,
             static_cast<double>(kDsiLaneBitrateMbps),
             kNativeStrideBytes, static_cast<double>(kDpiClockMHz),
             kFramebufferCount);
    report_memory("before_source_allocation");

    resources.transform_source = static_cast<std::uint16_t *>(heap_caps_malloc(
        kTransformSourceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (resources.transform_source == nullptr) {
        ESP_LOGE(kTag, "transform source allocation result=FAIL bytes=%zu",
                 kTransformSourceBytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "transform source pointer=%p bytes=%zu geometry=%zux%zu",
             static_cast<void *>(resources.transform_source),
             kTransformSourceBytes, kTransformSourceWidth,
             kTransformSourceHeight);
    report_memory("after_source_allocation");

    esp_err_t ret = display_session_initialize(&resources);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(kPanelStabilizationDelay);
    const std::span<std::uint16_t> source(
        resources.transform_source, kTransformSourcePixelCount);
    if (!fill_transform_source_pattern(source)) {
        return fail_stage(&resources, "transform_source_pattern_fill",
                          ESP_ERR_INVALID_SIZE);
    }
    const std::uint32_t source_crc = transform_source_pattern_crc32(source);
    ESP_LOGI(kTag,
             "source geometry=%zux%zu bytes=%zu crc32=0x%08" PRIx32,
             kTransformSourceWidth, kTransformSourceHeight,
             kTransformSourceBytes, source_crc);

    const std::span<std::uint16_t> destination(
        resources.framebuffer, kTransformDestinationPixelCount);
    if (!transform_to_native(source, destination, rotation)) {
        return fail_stage(&resources, "transform", ESP_ERR_INVALID_SIZE);
    }
    ret = sync_framebuffer(resources.framebuffer);
    if (ret != ESP_OK) {
        return fail_stage(&resources, "transform_cache_sync", ret);
    }
    const std::uint32_t destination_crc = crc32(
        reinterpret_cast<const std::uint8_t *>(resources.framebuffer),
        kNativeFramebufferBytes);
    ESP_LOGI(kTag,
             "destination geometry=%zux%zu bytes=%zu num_fbs=%zu crc32=0x%08" PRIx32,
             kTransformDestinationWidth, kTransformDestinationHeight,
             kTransformDestinationBytes, kFramebufferCount, destination_crc);
    ESP_LOGI(kTag, "transform result=PASS rotation=%s", rotation_name);

    ret = p4_nano_board::display_backlight_set(
        p4_nano_board::kBacklightConservative);
    if (ret != ESP_OK) {
        return fail_stage(&resources, "backlight_enable", ret);
    }
    ESP_LOGI(kTag,
             "backlight state=ON value=0x%02x visible_hold_seconds=30",
             p4_nano_board::kBacklightConservative);

    vTaskDelay(kTransformDiagnosticHold);
    const esp_err_t cleanup_error = cleanup(&resources);
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(kTag, "transform diagnostic final cleanup result=FAIL error=%s",
                 esp_err_to_name(cleanup_error));
        return cleanup_error;
    }
    ESP_LOGI(kTag, "transform diagnostic final cleanup result=PASS backlight=OFF");
    return ESP_OK;
}

} // namespace p4_nano_display
