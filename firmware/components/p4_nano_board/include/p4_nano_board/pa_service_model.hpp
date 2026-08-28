#pragma once

namespace p4_nano_board {

/* Deterministic, hardware-independent PA state machine used by host tests. */
class PaServiceModel {
public:
    enum class State {
        kUninitialized,
        kDisabled,
        kEnabled,
    };

    bool init(bool gpio_configuration_succeeded) noexcept
    {
        if (!gpio_configuration_succeeded) {
            state_ = State::kUninitialized;
            safe_low_ = false;
            return false;
        }
        state_ = State::kDisabled;
        safe_low_ = true;
        return true;
    }

    bool enable() noexcept
    {
        if (state_ == State::kUninitialized) {
            return false;
        }
        state_ = State::kEnabled;
        safe_low_ = false;
        return true;
    }

    bool disable() noexcept
    {
        if (state_ == State::kUninitialized) {
            return false;
        }
        state_ = State::kDisabled;
        safe_low_ = true;
        return true;
    }

    bool shutdown() noexcept
    {
        /* GPIO remains configured as an output so LOW is retained physically. */
        state_ = State::kUninitialized;
        safe_low_ = true;
        return true;
    }

    State state() const noexcept { return state_; }
    bool is_initialized() const noexcept
    {
        return state_ != State::kUninitialized;
    }
    bool is_enabled() const noexcept { return state_ == State::kEnabled; }
    bool is_safe_low() const noexcept { return safe_low_; }

private:
    State state_ = State::kUninitialized;
    bool safe_low_ = false;
};

} // namespace p4_nano_board
