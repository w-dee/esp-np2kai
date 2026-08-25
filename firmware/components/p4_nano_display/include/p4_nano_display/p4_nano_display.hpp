#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

namespace p4_nano_display {

/*
 * The DPI driver's refresh-done callback is delivered from the DSI bridge
 * interrupt path.  Keep the callback state bounded and lock-free: the
 * callback only performs relaxed 32-bit atomic operations and never logs or
 * allocates.  The period total is represented as two 32-bit words so the
 * callback does not need a potentially non-lock-free 64-bit atomic on RV32.
 */
struct VsyncStats final {
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "VSYNC active flag requires a lock-free atomic");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "VSYNC statistics require lock-free 32-bit atomics");

    std::atomic<bool> active{false};
    std::atomic<std::uint32_t> callback_count{0U};
    std::atomic<std::uint32_t> period_count{0U};
    std::atomic<std::uint32_t> last_timestamp_us{0U};
    std::atomic<std::uint32_t> period_min_us{UINT32_MAX};
    std::atomic<std::uint32_t> period_max_us{0U};
    std::atomic<std::uint32_t> period_total_low{0U};
    std::atomic<std::uint32_t> period_total_high{0U};

    void reset() noexcept
    {
        active.store(false, std::memory_order_relaxed);
        callback_count.store(0U, std::memory_order_relaxed);
        period_count.store(0U, std::memory_order_relaxed);
        last_timestamp_us.store(0U, std::memory_order_relaxed);
        period_min_us.store(UINT32_MAX, std::memory_order_relaxed);
        period_max_us.store(0U, std::memory_order_relaxed);
        period_total_low.store(0U, std::memory_order_relaxed);
        period_total_high.store(0U, std::memory_order_relaxed);
    }
};

struct VsyncStatsSnapshot final {
    std::uint32_t callback_count = 0U;
    std::uint32_t period_count = 0U;
    std::uint32_t period_min_us = 0U;
    std::uint32_t period_max_us = 0U;
    std::uint64_t period_total_us = 0U;
    bool callback_registered = false;
};

/* Concrete P4-NANO display ownership used by bounded diagnostics and the
 * first live consumer. The driver-owned framebuffer remains singular. */
struct DisplaySession {
    esp_ldo_channel_handle_t ldo = nullptr;
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    std::uint16_t *framebuffer = nullptr;
    VsyncStats vsync{};
    bool vsync_callback_registered = false;
    /* Used only by the existing source-pattern diagnostic. */
    std::uint16_t *transform_source = nullptr;
};

/* Reuses the qualified Step 7B.2a initialization sequence with backlight OFF.
 * The caller owns the session object and must call display_session_cleanup(). */
esp_err_t display_session_initialize(DisplaySession *session);
esp_err_t display_session_sync_framebuffer(DisplaySession *session);
esp_err_t display_session_cleanup(DisplaySession *session);
void display_session_reset_vsync(DisplaySession *session) noexcept;
void display_session_snapshot_vsync(const DisplaySession *session,
                                    VsyncStatsSnapshot *snapshot) noexcept;

/* Runs the bounded, static native-panel foundation validation. */
esp_err_t run_foundation();

} // namespace p4_nano_display
