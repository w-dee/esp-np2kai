#include "np2_keyboard_validation/validation_controller.hpp"

#include <limits>

namespace np2_keyboard_validation {

namespace {

constexpr std::uint32_t delta_or_max(const std::uint32_t current,
                                     const std::uint32_t baseline) noexcept
{
    return current >= baseline ? current - baseline
                               : std::numeric_limits<std::uint32_t>::max();
}

} // namespace

ValidationController::ValidationController(const TimeoutConfig timeouts) noexcept
    : timeouts_(timeouts)
{
}

void ValidationController::begin(const CounterSnapshot &baseline,
                                 const std::uint64_t now_us) noexcept
{
    baseline_ = baseline;
    proof_counters_ = baseline;
    state_ = State::WaitingReady;
    failure_reason_ = FailureReason::None;
    started_ = true;
    global_deadline_us_ = now_us + timeouts_.global_us;
    set_deadline(now_us, timeouts_.ready_us);
}

bool ValidationController::counters_equal(const CounterSnapshot &left,
                                          const CounterSnapshot &right) noexcept
{
    return left.enqueued == right.enqueued &&
           left.dequeued == right.dequeued &&
           left.queue_overflows == right.queue_overflows &&
           left.queue_rejected == right.queue_rejected &&
           left.blocked_events == right.blocked_events &&
           left.recovery_discards == right.recovery_discards &&
           left.press_injected == right.press_injected &&
           left.release_injected == right.release_injected &&
           left.duplicate_suppressed == right.duplicate_suppressed &&
           left.invalid_rejected == right.invalid_rejected &&
           left.source_capacity_failures == right.source_capacity_failures &&
           left.source_disconnects == right.source_disconnects &&
           left.global_recoveries == right.global_recoveries &&
           left.quarantined == right.quarantined;
}

bool ValidationController::counter_delta(const CounterSnapshot &current,
                                         const CounterSnapshot &baseline,
                                         std::uint32_t *delta) noexcept
{
    if (delta == nullptr || current.enqueued < baseline.enqueued ||
        current.dequeued < baseline.dequeued ||
        current.queue_overflows < baseline.queue_overflows ||
        current.queue_rejected < baseline.queue_rejected ||
        current.blocked_events < baseline.blocked_events ||
        current.recovery_discards < baseline.recovery_discards ||
        current.press_injected < baseline.press_injected ||
        current.release_injected < baseline.release_injected ||
        current.duplicate_suppressed < baseline.duplicate_suppressed ||
        current.invalid_rejected < baseline.invalid_rejected ||
        current.source_capacity_failures < baseline.source_capacity_failures ||
        current.source_disconnects < baseline.source_disconnects ||
        current.global_recoveries < baseline.global_recoveries) {
        return false;
    }
    *delta = delta_or_max(current.enqueued, baseline.enqueued);
    return true;
}

bool ValidationController::error_counters_unchanged(
    const CounterSnapshot &current) const noexcept
{
    return !current.quarantined &&
           current.queue_overflows == baseline_.queue_overflows &&
           current.queue_rejected == baseline_.queue_rejected &&
           current.blocked_events == baseline_.blocked_events &&
           current.recovery_discards == baseline_.recovery_discards &&
           current.duplicate_suppressed == baseline_.duplicate_suppressed &&
           current.invalid_rejected == baseline_.invalid_rejected &&
           current.source_capacity_failures ==
               baseline_.source_capacity_failures &&
           current.source_disconnects == baseline_.source_disconnects &&
           current.global_recoveries == baseline_.global_recoveries;
}

bool ValidationController::phase_counters_valid(
    const CounterSnapshot &current) const noexcept
{
    std::uint32_t ignored = 0;
    if (!counter_delta(current, baseline_, &ignored) ||
        !error_counters_unchanged(current)) {
        return false;
    }

    const auto enqueued = delta_or_max(current.enqueued, baseline_.enqueued);
    const auto dequeued = delta_or_max(current.dequeued, baseline_.dequeued);
    const auto pressed =
        delta_or_max(current.press_injected, baseline_.press_injected);
    const auto released =
        delta_or_max(current.release_injected, baseline_.release_injected);
    if (enqueued > 2U || dequeued > enqueued || pressed > 1U ||
        released > 1U) {
        return false;
    }

    switch (state_) {
    case State::WaitingReady:
        return enqueued == 0U && dequeued == 0U && pressed == 0U &&
               released == 0U;
    case State::PressQueued:
        return enqueued == 1U &&
               ((dequeued == 0U && pressed == 0U) ||
                (dequeued == 1U && pressed == 1U)) &&
               released == 0U;
    case State::WaitingMake:
        return enqueued == 1U && dequeued == 1U && pressed == 1U &&
               released == 0U;
    case State::ReleaseQueued:
        return enqueued == 2U &&
               ((dequeued == 1U && pressed == 1U && released == 0U) ||
                (dequeued == 2U && pressed == 1U && released == 1U));
    case State::WaitingBreak:
    case State::WaitingResult:
    case State::Complete:
        return enqueued == 2U && dequeued == 2U && pressed == 1U &&
               released == 1U;
    case State::Failed:
        return true;
    }
    return false;
}

bool ValidationController::deadline_expired(
    const InputSnapshot &input) const noexcept
{
    if (input.now_us >= phase_deadline_us_) {
        return true;
    }
    return input.now_us >= global_deadline_us_;
}

void ValidationController::set_deadline(const std::uint64_t now_us,
                                        const std::uint64_t duration_us) noexcept
{
    phase_deadline_us_ = now_us + duration_us;
}

StepResult ValidationController::terminal_step(const Action action) const noexcept
{
    return {action, state_, failure_reason_};
}

StepResult ValidationController::transition(const State state) noexcept
{
    state_ = state;
    return {Action::None, state_, failure_reason_};
}

StepResult ValidationController::fail_step(
    const FailureReason reason) noexcept
{
    if (state_ == State::Failed) {
        return terminal_step(Action::None);
    }
    state_ = State::Failed;
    failure_reason_ = reason;
    return terminal_step(Action::Fail);
}

StepResult ValidationController::complete_step(
    const CounterSnapshot &counters) noexcept
{
    if (state_ == State::Complete) {
        return terminal_step(Action::None);
    }
    state_ = State::Complete;
    failure_reason_ = FailureReason::None;
    proof_counters_ = counters;
    return terminal_step(Action::Complete);
}

StepResult ValidationController::fail(const FailureReason reason) noexcept
{
    return fail_step(reason);
}

StepResult ValidationController::observe(const InputSnapshot &input) noexcept
{
    if (!started_) {
        return fail_step(FailureReason::RuntimeFatal);
    }
    if (state_ == State::Complete || state_ == State::Failed) {
        return terminal_step(Action::None);
    }

    if (input.control_observation == ControlObservation::Invalid) {
        return fail_step(FailureReason::ControlInvalid);
    }
    if (input.control_observation == ControlObservation::Accepted &&
        input.control_state == ControlState::Fail) {
        return fail_step(FailureReason::GuestControlFail);
    }
    if (input.result_observation == ResultObservation::Invalid) {
        return fail_step(FailureReason::ResultInvalid);
    }
    if (input.result_observation == ResultObservation::Fail) {
        return fail_step(FailureReason::GuestResultFail);
    }
    if (input.counters.quarantined) {
        return fail_step(FailureReason::KeyboardFault);
    }
    if (!phase_counters_valid(input.counters)) {
        return fail_step(FailureReason::CounterMismatch);
    }

    if (deadline_expired(input)) {
        switch (state_) {
        case State::WaitingReady:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::ReadyTimeout);
        case State::PressQueued:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::PressDrainTimeout);
        case State::WaitingMake:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::MakeTimeout);
        case State::ReleaseQueued:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::ReleaseDrainTimeout);
        case State::WaitingBreak:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::BreakTimeout);
        case State::WaitingResult:
            return fail_step(input.now_us >= global_deadline_us_
                                 ? FailureReason::GlobalTimeout
                                 : FailureReason::ResultTimeout);
        case State::Complete:
        case State::Failed:
            break;
        }
    }

    switch (state_) {
    case State::WaitingReady:
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::Ready) {
            set_deadline(input.now_us, timeouts_.press_drain_us);
            state_ = State::PressQueued;
            return {Action::EnqueuePress, state_, failure_reason_};
        }
        return terminal_step(Action::None);

    case State::PressQueued: {
        if (input.enqueue_outcome == EnqueueOutcome::Failed) {
            return fail_step(FailureReason::PressEnqueueFailed);
        }
        const auto dequeued =
            delta_or_max(input.counters.dequeued, baseline_.dequeued);
        const auto pressed = delta_or_max(input.counters.press_injected,
                                          baseline_.press_injected);
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::MakeObserved &&
            (dequeued != 1U || pressed != 1U)) {
            return fail_step(FailureReason::MakeBeforePressDrain);
        }
        if (dequeued == 1U && pressed == 1U) {
            set_deadline(input.now_us, timeouts_.make_us);
            return transition(State::WaitingMake);
        }
        return terminal_step(Action::None);
    }

    case State::WaitingMake:
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::BreakObserved) {
            return fail_step(FailureReason::MakeBeforePressDrain);
        }
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::MakeObserved) {
            set_deadline(input.now_us, timeouts_.release_drain_us);
            state_ = State::ReleaseQueued;
            return {Action::EnqueueRelease, state_, failure_reason_};
        }
        return terminal_step(Action::None);

    case State::ReleaseQueued: {
        if (input.enqueue_outcome == EnqueueOutcome::Failed) {
            return fail_step(FailureReason::ReleaseEnqueueFailed);
        }
        const auto dequeued =
            delta_or_max(input.counters.dequeued, baseline_.dequeued);
        const auto released = delta_or_max(input.counters.release_injected,
                                           baseline_.release_injected);
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::BreakObserved &&
            (dequeued != 2U || released != 1U)) {
            return fail_step(FailureReason::BreakBeforeReleaseDrain);
        }
        if (dequeued == 2U && released == 1U) {
            set_deadline(input.now_us, timeouts_.break_us);
            return transition(State::WaitingBreak);
        }
        return terminal_step(Action::None);
    }

    case State::WaitingBreak:
        if (input.control_observation == ControlObservation::Accepted &&
            input.control_state == ControlState::BreakObserved) {
            if (input.result_observation == ResultObservation::Pass) {
                return complete_step(input.counters);
            }
            set_deadline(input.now_us, timeouts_.result_us);
            return transition(State::WaitingResult);
        }
        return terminal_step(Action::None);

    case State::WaitingResult:
        if (input.result_observation == ResultObservation::Pass) {
            return complete_step(input.counters);
        }
        return terminal_step(Action::None);

    case State::Complete:
    case State::Failed:
        return terminal_step(Action::None);
    }
    return fail_step(FailureReason::RuntimeFatal);
}

const char *to_string(const State state) noexcept
{
    switch (state) {
    case State::WaitingReady: return "WAITING_READY";
    case State::PressQueued: return "PRESS_QUEUED";
    case State::WaitingMake: return "WAITING_MAKE";
    case State::ReleaseQueued: return "RELEASE_QUEUED";
    case State::WaitingBreak: return "WAITING_BREAK";
    case State::WaitingResult: return "WAITING_RESULT";
    case State::Complete: return "COMPLETE";
    case State::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

const char *to_string(const Action action) noexcept
{
    switch (action) {
    case Action::None: return "NONE";
    case Action::EnqueuePress: return "ENQUEUE_PRESS";
    case Action::EnqueueRelease: return "ENQUEUE_RELEASE";
    case Action::Complete: return "COMPLETE";
    case Action::Fail: return "FAIL";
    }
    return "UNKNOWN";
}

const char *to_string(const FailureReason reason) noexcept
{
    switch (reason) {
    case FailureReason::None: return "NONE";
    case FailureReason::ReadyTimeout: return "READY_TIMEOUT";
    case FailureReason::ControlInvalid: return "CONTROL_INVALID";
    case FailureReason::GuestControlFail: return "GUEST_CONTROL_FAIL";
    case FailureReason::PressEnqueueFailed: return "PRESS_ENQUEUE_FAILED";
    case FailureReason::PressDrainTimeout: return "PRESS_DRAIN_TIMEOUT";
    case FailureReason::MakeTimeout: return "MAKE_TIMEOUT";
    case FailureReason::MakeBeforePressDrain: return "MAKE_BEFORE_PRESS_DRAIN";
    case FailureReason::ReleaseEnqueueFailed: return "RELEASE_ENQUEUE_FAILED";
    case FailureReason::ReleaseDrainTimeout: return "RELEASE_DRAIN_TIMEOUT";
    case FailureReason::BreakBeforeReleaseDrain: return "BREAK_BEFORE_RELEASE_DRAIN";
    case FailureReason::BreakTimeout: return "BREAK_TIMEOUT";
    case FailureReason::ResultTimeout: return "RESULT_TIMEOUT";
    case FailureReason::ResultInvalid: return "RESULT_INVALID";
    case FailureReason::GuestResultFail: return "GUEST_RESULT_FAIL";
    case FailureReason::CounterMismatch: return "COUNTER_MISMATCH";
    case FailureReason::KeyboardFault: return "KEYBOARD_FAULT";
    case FailureReason::GlobalTimeout: return "GLOBAL_TIMEOUT";
    case FailureReason::RuntimeFatal: return "RUNTIME_FATAL";
    }
    return "UNKNOWN";
}

} // namespace np2_keyboard_validation
