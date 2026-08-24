#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "np2_keyboard_input/np2_key_mapping.hpp"

namespace np2_keyboard_input {

inline constexpr std::size_t kMaxActiveSources = 4;
inline constexpr std::size_t kFrontendKeyDomain = 128;
inline constexpr std::size_t kKeyStateWords =
    (static_cast<std::size_t>(Key::Count) + 63U) / 64U;

enum class ShiftMode : std::uint8_t {
    /* The validated/default NP2 mode collapses frontend RSHIFT (0x7d) to
     * effective SHIFT (0x70). */
    Collapsed,
    Separate,
};

enum class EventResult : std::uint8_t {
    Injected,
    Accepted,
    Duplicate,
    Invalid,
    Quarantined,
    SourceCapacityExceeded,
};

enum class InputState : std::uint8_t {
    Ready,
    Desynchronized,
};

struct OwnershipCounters final {
    std::uint32_t accepted_events = 0;
    std::uint32_t press_injected = 0;
    std::uint32_t release_injected = 0;
    std::uint32_t duplicate_suppressed = 0;
    std::uint32_t invalid_rejected = 0;
    std::uint32_t source_disconnects = 0;
    std::uint32_t source_capacity_failures = 0;
    std::uint32_t blocked_events = 0;
    std::uint32_t global_recoveries = 0;
};

using KeySink = void (*)(void *, mapping::FrontendKeyId) noexcept;
using RecoverySink = void (*)(void *) noexcept;

/* A small callback sink keeps ownership tests independent of vendor globals.
 * The firmware bridge supplies the only real sink that calls keystat_*. */
struct Sink final {
    void *context = nullptr;
    KeySink press = nullptr;
    KeySink release = nullptr;
    RecoverySink all_release = nullptr;
};

bool is_valid_event(const Event &event) noexcept;
bool is_toggle_key(Key key) noexcept;
mapping::FrontendKeyId effective_frontend_key_id(
    mapping::FrontendKeyId key, ShiftMode mode) noexcept;

class OwnershipEngine final {
public:
    explicit OwnershipEngine(Sink sink = {},
                             ShiftMode shift_mode = ShiftMode::Collapsed) noexcept;

    OwnershipEngine(const OwnershipEngine &) = delete;
    OwnershipEngine &operator=(const OwnershipEngine &) = delete;

    EventResult process(const Event &event) noexcept;
    bool disconnect_source(SourceId source) noexcept;

    /* Global fail-closed recovery.  It clears project ownership and invokes
     * all_release at most once until rearm().  NP2 allrelease intentionally
     * leaves Caps/Kana toggle state untouched. */
    bool emergency_recover() noexcept;
    bool rearm() noexcept;

    /* Used only before the NP2 core becomes active; it cannot call a sink. */
    void discard_without_injection() noexcept;

    void set_shift_mode(ShiftMode mode) noexcept { shift_mode_ = mode; }
    ShiftMode shift_mode() const noexcept { return shift_mode_; }
    InputState state() const noexcept { return state_; }
    bool desynchronized() const noexcept
    {
        return state_ == InputState::Desynchronized;
    }
    const OwnershipCounters &counters() const noexcept { return counters_; }

private:
    struct SourceSlot final {
        bool active = false;
        SourceId id{};
        std::array<std::uint64_t, kKeyStateWords> held{};
        /* Effective mapping is remembered at Press so a later mode change
         * cannot release a different identity. */
        std::array<std::uint8_t, static_cast<std::size_t>(Key::Count)>
            effective_ids{};
    };

    static bool bit_is_set(const SourceSlot &slot, std::size_t key_index) noexcept;
    static void set_bit(SourceSlot &slot, std::size_t key_index) noexcept;
    static void clear_bit(SourceSlot &slot, std::size_t key_index) noexcept;
    static bool has_held_keys(const SourceSlot &slot) noexcept;

    SourceSlot *find_source(SourceId source) noexcept;
    SourceSlot *allocate_source(SourceId source) noexcept;
    void clear_state() noexcept;
    void press_sink(mapping::FrontendKeyId key) noexcept;
    void release_sink(mapping::FrontendKeyId key) noexcept;
    void recovery_sink() noexcept;

    Sink sink_{};
    ShiftMode shift_mode_ = ShiftMode::Collapsed;
    InputState state_ = InputState::Ready;
    bool recovery_done_ = false;
    std::array<SourceSlot, kMaxActiveSources> sources_{};
    std::array<std::uint16_t, kFrontendKeyDomain> owner_counts_{};
    OwnershipCounters counters_{};
};

} // namespace np2_keyboard_input
