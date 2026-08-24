#include "np2_keyboard_input/ownership.hpp"

#include <algorithm>

namespace np2_keyboard_input {

namespace {

constexpr std::size_t key_index(const Key key) noexcept
{
    return static_cast<std::size_t>(static_cast<std::uint8_t>(key));
}

constexpr std::size_t frontend_index(
    const mapping::FrontendKeyId key) noexcept
{
    return static_cast<std::size_t>(static_cast<std::uint8_t>(key));
}

constexpr bool valid_action(const Action action) noexcept
{
    return action == Action::Press || action == Action::Release;
}

} // namespace

bool is_valid_event(const Event &event) noexcept
{
    const auto key = static_cast<std::uint8_t>(event.key);
    return event.source.valid() && event.key != Key::Unknown &&
           key < static_cast<std::uint8_t>(Key::Count) &&
           valid_action(event.action);
}

bool is_toggle_key(const Key key) noexcept
{
    return key == Key::CapsLock || key == Key::Kana;
}

mapping::FrontendKeyId effective_frontend_key_id(
    const mapping::FrontendKeyId key, const ShiftMode mode) noexcept
{
    if (mode == ShiftMode::Collapsed &&
        key == mapping::FrontendKeyId::RightShift) {
        return mapping::FrontendKeyId::LeftShift;
    }
    return key;
}

OwnershipEngine::OwnershipEngine(const Sink sink,
                                 const ShiftMode shift_mode) noexcept
    : sink_(sink), shift_mode_(shift_mode)
{
}

bool OwnershipEngine::bit_is_set(const SourceSlot &slot,
                                 const std::size_t index) noexcept
{
    return (slot.held[index / 64U] & (UINT64_C(1) << (index % 64U))) != 0U;
}

void OwnershipEngine::set_bit(SourceSlot &slot, const std::size_t index) noexcept
{
    slot.held[index / 64U] |= UINT64_C(1) << (index % 64U);
}

void OwnershipEngine::clear_bit(SourceSlot &slot,
                                const std::size_t index) noexcept
{
    slot.held[index / 64U] &= ~(UINT64_C(1) << (index % 64U));
}

bool OwnershipEngine::has_held_keys(const SourceSlot &slot) noexcept
{
    for (const auto word : slot.held) {
        if (word != 0U) {
            return true;
        }
    }
    return false;
}

OwnershipEngine::SourceSlot *OwnershipEngine::find_source(
    const SourceId source) noexcept
{
    for (auto &slot : sources_) {
        if (slot.active && slot.id == source) {
            return &slot;
        }
    }
    return nullptr;
}

OwnershipEngine::SourceSlot *OwnershipEngine::allocate_source(
    const SourceId source) noexcept
{
    for (auto &slot : sources_) {
        if (!slot.active) {
            slot = SourceSlot{};
            slot.active = true;
            slot.id = source;
            return &slot;
        }
    }
    return nullptr;
}

void OwnershipEngine::clear_state() noexcept
{
    for (auto &slot : sources_) {
        slot = SourceSlot{};
    }
    owner_counts_.fill(0U);
}

void OwnershipEngine::press_sink(const mapping::FrontendKeyId key) noexcept
{
    if (sink_.press != nullptr) {
        sink_.press(sink_.context, key);
    }
    ++counters_.press_injected;
}

void OwnershipEngine::release_sink(const mapping::FrontendKeyId key) noexcept
{
    if (sink_.release != nullptr) {
        sink_.release(sink_.context, key);
    }
    ++counters_.release_injected;
}

void OwnershipEngine::recovery_sink() noexcept
{
    if (sink_.all_release != nullptr) {
        sink_.all_release(sink_.context);
    }
    ++counters_.global_recoveries;
}

EventResult OwnershipEngine::process(const Event &event) noexcept
{
    if (state_ == InputState::Desynchronized) {
        ++counters_.blocked_events;
        return EventResult::Quarantined;
    }
    if (!is_valid_event(event)) {
        ++counters_.invalid_rejected;
        return EventResult::Invalid;
    }

    const auto mapped = mapping::to_frontend_key_id(event.key);
    if (!mapped.has_value()) {
        ++counters_.invalid_rejected;
        return EventResult::Invalid;
    }
    const auto index = key_index(event.key);
    const auto effective = effective_frontend_key_id(*mapped, shift_mode_);
    const auto effective_index = frontend_index(effective);

    SourceSlot *slot = find_source(event.source);
    if (event.action == Action::Press) {
        if (slot == nullptr) {
            slot = allocate_source(event.source);
            if (slot == nullptr) {
                ++counters_.source_capacity_failures;
                state_ = InputState::Desynchronized;
                return EventResult::SourceCapacityExceeded;
            }
        }
        if (bit_is_set(*slot, index)) {
            ++counters_.duplicate_suppressed;
            return EventResult::Duplicate;
        }

        set_bit(*slot, index);
        slot->effective_ids[index] = static_cast<std::uint8_t>(effective);
        ++counters_.accepted_events;

        if (is_toggle_key(event.key)) {
            // Caps/Kana are mechanical toggles.  A valid Press is the
            // complete action; Release only clears this source's local edge.
            press_sink(effective);
            return EventResult::Injected;
        }

        if (owner_counts_[effective_index] == 0U) {
            press_sink(effective);
        }
        ++owner_counts_[effective_index];
        return owner_counts_[effective_index] == 1U
                   ? EventResult::Injected
                   : EventResult::Accepted;
    }

    if (slot == nullptr || !bit_is_set(*slot, index)) {
        ++counters_.duplicate_suppressed;
        return EventResult::Duplicate;
    }
    clear_bit(*slot, index);
    const auto held_effective = static_cast<mapping::FrontendKeyId>(
        slot->effective_ids[index]);
    slot->effective_ids[index] = 0U;
    ++counters_.accepted_events;

    if (!is_toggle_key(event.key)) {
        const auto held_index = frontend_index(held_effective);
        if (owner_counts_[held_index] > 0U) {
            --owner_counts_[held_index];
            if (owner_counts_[held_index] == 0U) {
                release_sink(held_effective);
            }
        }
    }
    if (!has_held_keys(*slot)) {
        *slot = SourceSlot{};
    }
    return EventResult::Accepted;
}

bool OwnershipEngine::disconnect_source(const SourceId source) noexcept
{
    if (!source.valid()) {
        ++counters_.invalid_rejected;
        return false;
    }
    SourceSlot *slot = find_source(source);
    ++counters_.source_disconnects;
    if (slot == nullptr) {
        return false;
    }

    for (std::size_t index = 1U;
         index < static_cast<std::size_t>(Key::Count); ++index) {
        if (!bit_is_set(*slot, index)) {
            continue;
        }
        const auto key = static_cast<Key>(index);
        clear_bit(*slot, index);
        const auto effective = static_cast<mapping::FrontendKeyId>(
            slot->effective_ids[index]);
        slot->effective_ids[index] = 0U;
        if (is_toggle_key(key)) {
            continue;
        }
        const auto effective_index = frontend_index(effective);
        if (owner_counts_[effective_index] > 0U) {
            --owner_counts_[effective_index];
            if (owner_counts_[effective_index] == 0U) {
                release_sink(effective);
            }
        }
    }
    *slot = SourceSlot{};
    return true;
}

bool OwnershipEngine::emergency_recover() noexcept
{
    if (recovery_done_) {
        return false;
    }
    clear_state();
    state_ = InputState::Desynchronized;
    recovery_done_ = true;
    recovery_sink();
    return true;
}

bool OwnershipEngine::rearm() noexcept
{
    if (state_ != InputState::Desynchronized || !recovery_done_) {
        return false;
    }
    clear_state();
    state_ = InputState::Ready;
    recovery_done_ = false;
    return true;
}

void OwnershipEngine::discard_without_injection() noexcept
{
    clear_state();
    state_ = InputState::Ready;
    recovery_done_ = false;
}

} // namespace np2_keyboard_input
