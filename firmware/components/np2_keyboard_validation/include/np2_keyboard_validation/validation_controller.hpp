#pragma once

#include <cstdint>

namespace np2_keyboard_validation {

enum class ControlObservation : std::uint8_t {
    Transient,
    Accepted,
    Invalid,
};

enum class ControlState : std::uint8_t {
    Uninitialized,
    Ready,
    MakeObserved,
    BreakObserved,
    Fail,
};

enum class ResultObservation : std::uint8_t {
    PreProtocol,
    Uninitialized,
    Running,
    Pass,
    Fail,
    Invalid,
};

enum class EnqueueOutcome : std::uint8_t {
    None,
    Enqueued,
    Failed,
};

enum class State : std::uint8_t {
    WaitingReady,
    PressQueued,
    WaitingMake,
    ReleaseQueued,
    WaitingBreak,
    WaitingResult,
    Complete,
    Failed,
};

enum class Action : std::uint8_t {
    None,
    EnqueuePress,
    EnqueueRelease,
    Complete,
    Fail,
};

enum class FailureReason : std::uint8_t {
    None,
    ReadyTimeout,
    ControlInvalid,
    GuestControlFail,
    PressEnqueueFailed,
    PressDrainTimeout,
    MakeTimeout,
    MakeBeforePressDrain,
    ReleaseEnqueueFailed,
    ReleaseDrainTimeout,
    BreakBeforeReleaseDrain,
    BreakTimeout,
    ResultTimeout,
    ResultInvalid,
    GuestResultFail,
    CounterMismatch,
    KeyboardFault,
    GlobalTimeout,
    RuntimeFatal,
};

struct CounterSnapshot final {
    std::uint32_t enqueued = 0;
    std::uint32_t dequeued = 0;
    std::uint32_t queue_overflows = 0;
    std::uint32_t queue_rejected = 0;
    std::uint32_t blocked_events = 0;
    std::uint32_t recovery_discards = 0;
    std::uint32_t press_injected = 0;
    std::uint32_t release_injected = 0;
    std::uint32_t duplicate_suppressed = 0;
    std::uint32_t invalid_rejected = 0;
    std::uint32_t source_capacity_failures = 0;
    std::uint32_t source_disconnects = 0;
    std::uint32_t global_recoveries = 0;
    bool quarantined = false;
};

struct TimeoutConfig final {
    std::uint64_t global_us = 30'000'000U;
    std::uint64_t ready_us = 5'000'000U;
    std::uint64_t press_drain_us = 5'000'000U;
    std::uint64_t make_us = 5'000'000U;
    std::uint64_t release_drain_us = 5'000'000U;
    std::uint64_t break_us = 5'000'000U;
    std::uint64_t result_us = 5'000'000U;
};

struct InputSnapshot final {
    ControlObservation control_observation = ControlObservation::Transient;
    ControlState control_state = ControlState::Uninitialized;
    std::uint8_t observed_make = 0;
    std::uint8_t observed_break = 0;
    std::uint16_t failure_reason = 0;
    ResultObservation result_observation = ResultObservation::PreProtocol;
    CounterSnapshot counters{};
    EnqueueOutcome enqueue_outcome = EnqueueOutcome::None;
    std::uint64_t now_us = 0;
};

struct StepResult final {
    Action action = Action::None;
    State state = State::WaitingReady;
    FailureReason failure_reason = FailureReason::None;
};

class ValidationController final {
public:
    explicit ValidationController(TimeoutConfig timeouts = {}) noexcept;

    ValidationController(const ValidationController &) = delete;
    ValidationController &operator=(const ValidationController &) = delete;

    void begin(const CounterSnapshot &baseline,
               std::uint64_t now_us) noexcept;
    StepResult observe(const InputSnapshot &input) noexcept;

    /* Runtime setup/fatal paths use this to retain one deterministic terminal
     * reason without making the controller know about FreeRTOS or NP2. */
    StepResult fail(FailureReason reason) noexcept;

    bool started() const noexcept { return started_; }
    State state() const noexcept { return state_; }
    FailureReason failure_reason() const noexcept { return failure_reason_; }
    const CounterSnapshot &baseline() const noexcept { return baseline_; }
    const CounterSnapshot &proof_counters() const noexcept
    {
        return proof_counters_;
    }

private:
    static bool counters_equal(const CounterSnapshot &left,
                               const CounterSnapshot &right) noexcept;
    static bool counter_delta(const CounterSnapshot &current,
                              const CounterSnapshot &baseline,
                              std::uint32_t *delta) noexcept;
    bool error_counters_unchanged(const CounterSnapshot &current) const noexcept;
    bool phase_counters_valid(const CounterSnapshot &current) const noexcept;
    bool deadline_expired(const InputSnapshot &input) const noexcept;
    void set_deadline(std::uint64_t now_us, std::uint64_t duration_us) noexcept;
    StepResult terminal_step(Action action) const noexcept;
    StepResult transition(State state) noexcept;
    StepResult fail_step(FailureReason reason) noexcept;
    StepResult complete_step(const CounterSnapshot &counters) noexcept;

    TimeoutConfig timeouts_{};
    CounterSnapshot baseline_{};
    CounterSnapshot proof_counters_{};
    State state_ = State::WaitingReady;
    FailureReason failure_reason_ = FailureReason::None;
    std::uint64_t global_deadline_us_ = 0;
    std::uint64_t phase_deadline_us_ = 0;
    bool started_ = false;
};

const char *to_string(State state) noexcept;
const char *to_string(Action action) noexcept;
const char *to_string(FailureReason reason) noexcept;

} // namespace np2_keyboard_validation
