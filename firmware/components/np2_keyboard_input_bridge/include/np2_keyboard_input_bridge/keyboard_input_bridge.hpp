#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "np2_keyboard_input/ownership.hpp"

namespace np2_keyboard_input_bridge {

inline constexpr std::size_t kQueueCapacity = 64;
inline constexpr std::size_t kMaxEventsPerOwnerIteration = 8;

enum class CommandKind : std::uint8_t {
    Event,
    SourceDisconnect,
};

/* Fixed-size internal transport command.  It is separate from the public
 * producer Event so disconnect cannot contaminate the producer contract. */
struct InputCommand final {
    CommandKind kind = CommandKind::Event;
    np2_keyboard_input::Event event{};
    np2_keyboard_input::SourceId source{};
};

static_assert(std::is_trivially_copyable_v<InputCommand>);

enum class EnqueueResult : std::uint8_t {
    Enqueued,
    Invalid,
    Full,
    Quarantined,
    NotInitialized,
};

enum class OwnerIterationResult : std::uint8_t {
    Idle,
    Drained,
    Recovered,
    Quarantined,
    NotInitialized,
};

struct BridgeCounters final {
    std::uint32_t enqueued = 0;
    std::uint32_t dequeued = 0;
    std::uint32_t queue_overflows = 0;
    std::uint32_t queue_rejected = 0;
    std::uint32_t blocked_events = 0;
    std::uint32_t recovery_discards = 0;
    np2_keyboard_input::OwnershipCounters ownership{};
};

/*
 * Thin ESP transport and real NP2 sink.  Producers may call enqueue() and
 * disconnect_source() from different tasks; only owner_iteration(), rearm(),
 * set_core_active(), and shutdown() may touch ownership or vendor state, and
 * they must run on the NP2 owner task.
 */
class KeyboardInputBridge final {
public:
    KeyboardInputBridge() noexcept;
    KeyboardInputBridge(const KeyboardInputBridge &) = delete;
    KeyboardInputBridge &operator=(const KeyboardInputBridge &) = delete;

    bool initialize() noexcept;
    EnqueueResult enqueue(const np2_keyboard_input::Event &event) noexcept;
    EnqueueResult disconnect_source(
        np2_keyboard_input::SourceId source) noexcept;

    OwnerIterationResult owner_iteration() noexcept;
    bool rearm() noexcept;
    void set_core_active(bool active) noexcept;
    void shutdown() noexcept;

    bool initialized() const noexcept { return queue_ != nullptr; }
    bool quarantined() const noexcept
    {
        return quarantine_.load(std::memory_order_acquire);
    }
    BridgeCounters counters() const noexcept;

private:
    static void sink_press(void *context,
                           np2_keyboard_input::mapping::FrontendKeyId key) noexcept;
    static void sink_release(void *context,
                             np2_keyboard_input::mapping::FrontendKeyId key) noexcept;
    static void sink_all_release(void *context) noexcept;

    bool can_enqueue() noexcept;
    void recover_overflow() noexcept;

    StaticQueue_t queue_storage_{};
    std::array<std::uint8_t, kQueueCapacity * sizeof(InputCommand)>
        queue_buffer_{};
    QueueHandle_t queue_ = nullptr;
    np2_keyboard_input::OwnershipEngine engine_;
    std::atomic<bool> overflow_latched_{false};
    std::atomic<bool> quarantine_{false};
    std::atomic<bool> producer_closed_{false};
    bool recovery_complete_ = false;
    bool core_active_ = false;
    std::atomic<std::uint32_t> enqueued_{0};
    std::atomic<std::uint32_t> dequeued_{0};
    std::atomic<std::uint32_t> queue_overflows_{0};
    std::atomic<std::uint32_t> queue_rejected_{0};
    std::atomic<std::uint32_t> blocked_events_{0};
    std::atomic<std::uint32_t> recovery_discards_{0};
};

} // namespace np2_keyboard_input_bridge
