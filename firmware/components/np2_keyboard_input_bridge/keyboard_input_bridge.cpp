#include "np2_keyboard_input_bridge/keyboard_input_bridge.hpp"

#include <cstdint>

/* Keep the project-owned bridge boundary narrow.  These are the three C ABI
 * entry points consumed from the NP2 keystat provider; REG8 is uint8_t in
 * compiler_base.h.  Including the broad compiler.h/keystat.h headers here
 * would make ESP-IDF infer a direct np2core include dependency and perturb
 * the existing np2core/np2host archive closure. */
extern "C" {
void keystat_keydown(std::uint8_t ref);
void keystat_keyup(std::uint8_t ref);
void keystat_allrelease(void);
}

namespace np2_keyboard_input_bridge {

namespace {

using np2_keyboard_input::mapping::FrontendKeyId;

} // namespace

KeyboardInputBridge::KeyboardInputBridge() noexcept
    : engine_({this, &KeyboardInputBridge::sink_press,
               &KeyboardInputBridge::sink_release,
               &KeyboardInputBridge::sink_all_release})
{
}

bool KeyboardInputBridge::initialize() noexcept
{
    if (queue_ != nullptr) {
        return true;
    }
    queue_ = xQueueCreateStatic(kQueueCapacity, sizeof(InputCommand),
                                queue_buffer_.data(), &queue_storage_);
    if (queue_ == nullptr) {
        return false;
    }
    engine_.discard_without_injection();
    overflow_latched_.store(false, std::memory_order_release);
    quarantine_.store(false, std::memory_order_release);
    producer_closed_.store(false, std::memory_order_release);
    recovery_complete_ = false;
    return true;
}

bool KeyboardInputBridge::can_enqueue() noexcept
{
    if (queue_ == nullptr) {
        ++queue_rejected_;
        return false;
    }
    if (producer_closed_.load(std::memory_order_acquire) ||
        overflow_latched_.load(std::memory_order_acquire) ||
        quarantine_.load(std::memory_order_acquire)) {
        ++blocked_events_;
        return false;
    }
    return true;
}

EnqueueResult KeyboardInputBridge::enqueue(
    const np2_keyboard_input::Event &event) noexcept
{
    if (!np2_keyboard_input::is_valid_event(event)) {
        ++queue_rejected_;
        return EnqueueResult::Invalid;
    }
    if (!can_enqueue()) {
        return queue_ == nullptr ? EnqueueResult::NotInitialized
                                  : EnqueueResult::Quarantined;
    }
    const InputCommand command{CommandKind::Event, event, {}};
    if (xQueueSend(queue_, &command, 0U) != pdTRUE) {
        overflow_latched_.store(true, std::memory_order_release);
        ++queue_overflows_;
        return EnqueueResult::Full;
    }
    ++enqueued_;
    return EnqueueResult::Enqueued;
}

EnqueueResult KeyboardInputBridge::disconnect_source(
    const np2_keyboard_input::SourceId source) noexcept
{
    if (!source.valid()) {
        ++queue_rejected_;
        return EnqueueResult::Invalid;
    }
    if (!can_enqueue()) {
        return queue_ == nullptr ? EnqueueResult::NotInitialized
                                  : EnqueueResult::Quarantined;
    }
    const InputCommand command{CommandKind::SourceDisconnect, {}, source};
    if (xQueueSend(queue_, &command, 0U) != pdTRUE) {
        overflow_latched_.store(true, std::memory_order_release);
        ++queue_overflows_;
        return EnqueueResult::Full;
    }
    ++enqueued_;
    return EnqueueResult::Enqueued;
}

void KeyboardInputBridge::recover_overflow() noexcept
{
    if (recovery_complete_) {
        return;
    }
    if (queue_ != nullptr) {
        const UBaseType_t queued = uxQueueMessagesWaiting(queue_);
        recovery_discards_.fetch_add(static_cast<std::uint32_t>(queued),
                                     std::memory_order_relaxed);
        (void)xQueueReset(queue_);
    }
    (void)engine_.emergency_recover();
    quarantine_.store(true, std::memory_order_release);
    recovery_complete_ = true;
}

OwnerIterationResult KeyboardInputBridge::owner_iteration() noexcept
{
    if (queue_ == nullptr) {
        return OwnerIterationResult::NotInitialized;
    }
    if (overflow_latched_.load(std::memory_order_acquire)) {
        const bool first_recovery = !recovery_complete_;
        recover_overflow();
        return first_recovery ? OwnerIterationResult::Recovered
                              : OwnerIterationResult::Quarantined;
    }
    if (quarantine_.load(std::memory_order_acquire)) {
        return OwnerIterationResult::Quarantined;
    }

    bool drained = false;
    InputCommand command{};
    for (std::size_t index = 0; index < kMaxEventsPerOwnerIteration;
         ++index) {
        if (xQueueReceive(queue_, &command, 0U) != pdTRUE) {
            break;
        }
        drained = true;
        ++dequeued_;
        if (command.kind == CommandKind::Event) {
            const auto result = engine_.process(command.event);
            if (result == np2_keyboard_input::EventResult::SourceCapacityExceeded) {
                quarantine_.store(true, std::memory_order_release);
                (void)engine_.emergency_recover();
                return OwnerIterationResult::Recovered;
            }
        } else if (command.kind == CommandKind::SourceDisconnect) {
            (void)engine_.disconnect_source(command.source);
        } else {
            ++queue_rejected_;
        }
    }
    return drained ? OwnerIterationResult::Drained
                   : OwnerIterationResult::Idle;
}

bool KeyboardInputBridge::rearm() noexcept
{
    if (queue_ == nullptr || producer_closed_.load(std::memory_order_acquire) ||
        !quarantine_.load(std::memory_order_acquire)) {
        return false;
    }
    (void)xQueueReset(queue_);
    if (!engine_.rearm()) {
        return false;
    }
    overflow_latched_.store(false, std::memory_order_release);
    quarantine_.store(false, std::memory_order_release);
    recovery_complete_ = false;
    return true;
}

void KeyboardInputBridge::set_core_active(const bool active) noexcept
{
    core_active_ = active;
}

void KeyboardInputBridge::shutdown() noexcept
{
    producer_closed_.store(true, std::memory_order_release);
    if (queue_ != nullptr) {
        (void)xQueueReset(queue_);
    }
    if (core_active_) {
        (void)engine_.emergency_recover();
    } else {
        engine_.discard_without_injection();
    }
    quarantine_.store(true, std::memory_order_release);
    recovery_complete_ = true;
}

BridgeCounters KeyboardInputBridge::counters() const noexcept
{
    BridgeCounters result{};
    result.enqueued = enqueued_.load(std::memory_order_relaxed);
    result.dequeued = dequeued_.load(std::memory_order_relaxed);
    result.queue_overflows = queue_overflows_.load(std::memory_order_relaxed);
    result.queue_rejected = queue_rejected_.load(std::memory_order_relaxed);
    result.blocked_events = blocked_events_.load(std::memory_order_relaxed);
    result.recovery_discards =
        recovery_discards_.load(std::memory_order_relaxed);
    result.ownership = engine_.counters();
    return result;
}

void KeyboardInputBridge::sink_press(void *context,
                                     const FrontendKeyId key) noexcept
{
    (void)context;
    keystat_keydown(static_cast<std::uint8_t>(key));
}

void KeyboardInputBridge::sink_release(void *context,
                                       const FrontendKeyId key) noexcept
{
    (void)context;
    keystat_keyup(static_cast<std::uint8_t>(key));
}

void KeyboardInputBridge::sink_all_release(void *context) noexcept
{
    (void)context;
    keystat_allrelease();
}

} // namespace np2_keyboard_input_bridge
