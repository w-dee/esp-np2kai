#include "np2runtime/np2runtime.hpp"

namespace np2runtime {

State Lifecycle::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

StopReason Lifecycle::stop_reason() const noexcept
{
    if (failure()) {
        return StopReason::Fatal;
    }
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

int Lifecycle::stop_reason_rank(const StopReason reason) noexcept
{
    switch (reason) {
    case StopReason::None:
        return 0;
    case StopReason::External:
        return 1;
    case StopReason::Fatal:
        return 2;
    }
    return 0;
}

bool Lifecycle::promote_stop_reason(const StopReason requested) noexcept
{
    StopReason current = stop_reason_.load(std::memory_order_acquire);
    for (;;) {
        if (stop_reason_rank(current) >= stop_reason_rank(requested)) {
            return true;
        }
        if (stop_reason_.compare_exchange_weak(
                current, requested, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void Lifecycle::lock_terminalization() noexcept
{
    while (terminalization_lock_.test_and_set(std::memory_order_acquire)) {
    }
}

void Lifecycle::unlock_terminalization() noexcept
{
    terminalization_lock_.clear(std::memory_order_release);
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
        /* Fatal is a monotonic upgrade.  If mark_failure() wins the race,
         * this helper leaves Fatal intact; if it loses, mark_failure() will
         * still promote External to Fatal before returning. */
        if (!failure()) {
            (void)promote_stop_reason(StopReason::External);
        }
        if (state_.compare_exchange_weak(current, State::StopRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::mark_failure() noexcept
{
    /* Serialize the only two operations that can decide a terminal result.
     * This keeps a Stopping->Stopped CAS from racing with a fatal update. */
    lock_terminalization();

    State current = state();
    for (;;) {
        if (current == State::Stopped || current == State::Failed) {
            unlock_terminalization();
            return false;
        }

        (void)promote_stop_reason(StopReason::Fatal);
        failure_.store(true, std::memory_order_release);

        if (current == State::Stopping || current == State::StopRequested) {
            unlock_terminalization();
            return true;
        }
        if (!active_state(current)) {
            unlock_terminalization();
            return false;
        }
        if (state_.compare_exchange_weak(current, State::StopRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            unlock_terminalization();
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
    lock_terminalization();

    State expected = State::Stopping;
    const State final_state = failure() ? State::Failed : State::Stopped;
    const bool completed = state_.compare_exchange_strong(
        expected, final_state, std::memory_order_acq_rel,
        std::memory_order_acquire);
    unlock_terminalization();
    return completed;
}

} // namespace np2runtime
