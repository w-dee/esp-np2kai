#include "np2runtime/np2runtime.hpp"

namespace np2runtime {

State Lifecycle::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

StopReason Lifecycle::stop_reason() const noexcept
{
    return stop_reason_.load(std::memory_order_acquire);
}

bool Lifecycle::failure() const noexcept
{
    return failure_.load(std::memory_order_acquire);
}

bool Lifecycle::stop_requested() const noexcept
{
    const State current = state();
    return current == State::StopRequested || current == State::Stopping ||
           current == State::Stopped || current == State::Failed;
}

bool Lifecycle::active_state(const State current) noexcept
{
    return current == State::Created || current == State::Initializing ||
           current == State::Ready || current == State::Running ||
           current == State::StopRequested;
}

bool Lifecycle::begin_initialization() noexcept
{
    State expected = State::Created;
    return state_.compare_exchange_strong(expected, State::Initializing,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}

bool Lifecycle::complete_initialization() noexcept
{
    State expected = State::Initializing;
    return state_.compare_exchange_strong(expected, State::Ready,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}

bool Lifecycle::begin_running() noexcept
{
    State expected = State::Ready;
    return state_.compare_exchange_strong(expected, State::Running,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}

bool Lifecycle::request_stop() noexcept
{
    State current = state();
    for (;;) {
        if (current == State::Stopped || current == State::Failed ||
            current == State::Stopping) {
            return false;
        }
        if (current == State::StopRequested) {
            return true;
        }
        if (!active_state(current)) {
            return false;
        }
        stop_reason_.store(StopReason::External, std::memory_order_release);
        if (state_.compare_exchange_weak(current, State::StopRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::mark_failure() noexcept
{
    failure_.store(true, std::memory_order_release);
    stop_reason_.store(StopReason::Fatal, std::memory_order_release);

    State current = state();
    for (;;) {
        if (current == State::Stopped || current == State::Failed) {
            return false;
        }
        if (current == State::Stopping || current == State::StopRequested) {
            return true;
        }
        if (!active_state(current)) {
            return false;
        }
        if (state_.compare_exchange_weak(current, State::StopRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::begin_stopping() noexcept
{
    State current = state();
    for (;;) {
        if (current == State::Stopping) {
            return true;
        }
        if (current == State::Stopped || current == State::Failed ||
            !active_state(current)) {
            return false;
        }
        if (state_.compare_exchange_weak(current, State::Stopping,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::finish_cleanup() noexcept
{
    State expected = State::Stopping;
    const State final_state = failure() ? State::Failed : State::Stopped;
    return state_.compare_exchange_strong(expected, final_state,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

} // namespace np2runtime
