#include "np2runtime/np2runtime.hpp"

namespace np2runtime {

State Lifecycle::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

StopReason Lifecycle::stop_reason() const noexcept
{
    switch (terminal_outcome()) {
    case TerminalOutcome::OpenExternal:
    case TerminalOutcome::FinalizedStoppedExternal:
        return StopReason::External;
    case TerminalOutcome::FatalPending:
    case TerminalOutcome::FinalizedFailed:
        return StopReason::Fatal;
    case TerminalOutcome::OpenNone:
    case TerminalOutcome::FinalizedStoppedNone:
        return StopReason::None;
    }
    return StopReason::None;
}

bool Lifecycle::failure() const noexcept
{
    const TerminalOutcome outcome = terminal_outcome();
    return outcome == TerminalOutcome::FatalPending ||
           outcome == TerminalOutcome::FinalizedFailed;
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

Lifecycle::TerminalOutcome Lifecycle::terminal_outcome() const noexcept
{
    return static_cast<TerminalOutcome>(
        terminal_outcome_.load(std::memory_order_acquire));
}

bool Lifecycle::claim_external_stop() noexcept
{
    std::uint32_t expected = static_cast<std::uint32_t>(
        TerminalOutcome::OpenNone);
    const std::uint32_t desired = static_cast<std::uint32_t>(
        TerminalOutcome::OpenExternal);
    for (;;) {
        const TerminalOutcome current = static_cast<TerminalOutcome>(expected);
        if (current == TerminalOutcome::OpenExternal) {
            return true;
        }
        if (current != TerminalOutcome::OpenNone) {
            return false;
        }
        if (terminal_outcome_.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::claim_fatal() noexcept
{
    std::uint32_t expected = terminal_outcome_.load(
        std::memory_order_acquire);
    const std::uint32_t desired = static_cast<std::uint32_t>(
        TerminalOutcome::FatalPending);
    for (;;) {
        const TerminalOutcome current = static_cast<TerminalOutcome>(expected);
        if (current == TerminalOutcome::FatalPending) {
            return true;
        }
        if (current != TerminalOutcome::OpenNone &&
            current != TerminalOutcome::OpenExternal) {
            return false;
        }
        if (terminal_outcome_.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
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
        (void)claim_external_stop();
        if (state_.compare_exchange_weak(current, State::StopRequested,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
    }
}

bool Lifecycle::mark_failure() noexcept
{
    /* FatalPending is the linearization point for this failure.  Cleanup can
     * publish Stopped only from OpenNone/OpenExternal, so it cannot publish
     * Stopped after this CAS succeeds. */
    if (!claim_fatal()) {
        return false;
    }

    State current = state();
    for (;;) {
        if (current == State::Stopped || current == State::Failed) {
            /* This call already claimed FatalPending.  Cleanup may have
             * finalized it before this thread observed the state. */
            return true;
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
    if (state() != State::Stopping) {
        return false;
    }

    std::uint32_t expected = terminal_outcome_.load(
        std::memory_order_acquire);
    for (;;) {
        const TerminalOutcome current = static_cast<TerminalOutcome>(expected);
        TerminalOutcome desired = TerminalOutcome::FinalizedFailed;
        State final_state = State::Failed;
        switch (current) {
        case TerminalOutcome::OpenNone:
            desired = TerminalOutcome::FinalizedStoppedNone;
            final_state = State::Stopped;
            break;
        case TerminalOutcome::OpenExternal:
            desired = TerminalOutcome::FinalizedStoppedExternal;
            final_state = State::Stopped;
            break;
        case TerminalOutcome::FatalPending:
            desired = TerminalOutcome::FinalizedFailed;
            final_state = State::Failed;
            break;
        case TerminalOutcome::FinalizedStoppedNone:
        case TerminalOutcome::FinalizedStoppedExternal:
        case TerminalOutcome::FinalizedFailed:
            return false;
        default:
            return false;
        }

        const std::uint32_t desired_raw = static_cast<std::uint32_t>(desired);
        if (!terminal_outcome_.compare_exchange_weak(
                expected, desired_raw, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        /* The outcome CAS above is the terminalization linearization point.
         * No other operation can leave Stopping: request_stop() rejects it,
         * mark_failure() only claims FatalPending.  This is therefore the
         * sole state publication from Stopping to its matching terminal
         * state. */
        State state_expected = State::Stopping;
        return state_.compare_exchange_strong(
            state_expected, final_state, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
}

} // namespace np2runtime
