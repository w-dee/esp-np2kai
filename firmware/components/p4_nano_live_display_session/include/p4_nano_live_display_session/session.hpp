#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include <compiler.h>
#include "np2_presentation.h"
#include "p4_nano_display/p4_nano_display.hpp"
#include "p4_nano_live_display_session/session_contract.hpp"
#include "scrnmng.h"

namespace p4_nano_live_display_session {

enum class ConsumeResult : std::uint8_t {
    NoFrame,
    Consumed,
    Failed,
};

enum class FrameStage : std::uint8_t {
    Acquired,
    Transformed,
};

struct FrameObservation final {
    FrameStage stage;
    np2_presentation_frame_view source;
    const std::uint16_t *native_framebuffer;
    std::size_t native_framebuffer_bytes;
};

using FrameObserver = bool (*)(const FrameObservation &observation,
                               void *context);

struct Counters final {
    std::uint32_t submitted = 0U;
    std::uint32_t acquired = 0U;
    std::uint32_t transformed = 0U;
    std::uint32_t released = 0U;
    std::uint32_t coalesced = 0U;
    std::uint32_t dropped = 0U;
};

struct FrameMetadata final {
    std::uint32_t source_generation = 0U;
    std::uint32_t source_update_sequence = 0U;
    std::uint64_t published_sequence = 0U;
};

/* Owns the reusable presentation/display path.  The scrnmng hook borrows
 * this object and is always detached before resources are destroyed. */
class Session final {
public:
    Session() noexcept = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session();

    /* Allocates the fixed PSRAM slots and initializes the display with the
     * backlight off. */
    esp_err_t initialize() noexcept;
    /* Registers the borrowed scrnmng callback; call detach_source() before
     * destroying this object or any of its owned storage. */
    esp_err_t attach_source() noexcept;
    void detach_source() noexcept;
    /* Non-blocking latest-frame poll.  The caller owns scheduler pacing when
     * NoFrame is returned. */
    ConsumeResult consume_one(FrameObserver observer = nullptr,
                              void *observer_context = nullptr) noexcept;
    /* Detaches the hook, releases any lease, then tears down display and
     * slots.  This is also called by the destructor. */
    esp_err_t shutdown() noexcept;

    bool failed() const noexcept
    {
        return failed_.load(std::memory_order_acquire);
    }
    bool visible() const noexcept { return first_frame_.visible; }
    bool native_framebuffer_external() const noexcept;
    const Counters &counters() const noexcept { return counters_; }
    const FrameMetadata &last_frame() const noexcept { return last_frame_; }

private:
    class LeaseGuard;

    static void publish_hook(const SCRNMNG_PUBLISH_VIEW *view,
                             void *context);
    bool validate_frame(const np2_presentation_frame_view &view) const noexcept;
    bool release(np2_presentation_token *token) noexcept;
    void refresh_publisher_counters() noexcept;
    void fail(esp_err_t error) noexcept;
    void free_slots() noexcept;

    np2_presentation_publisher publisher_{};
    np2_presentation_slot_storage slots_[kPresentationSlotCount]{};
    p4_nano_display::DisplaySession display_{};
    FirstFrameGate first_frame_{};
    Counters counters_{};
    FrameMetadata last_frame_{};
    np2_presentation_token outstanding_token_{};
    bool outstanding_lease_ = false;
    bool initialized_ = false;
    bool hook_registered_ = false;
    std::atomic<bool> failed_{false};
    esp_err_t last_error_ = ESP_OK;
};

} // namespace p4_nano_live_display_session
