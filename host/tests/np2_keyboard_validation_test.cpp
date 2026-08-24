#include <cassert>
#include <cstdint>

#include "np2_keyboard_validation/validation_controller.hpp"

namespace {

using namespace np2_keyboard_validation;

InputSnapshot input(const std::uint64_t now,
                    const ControlObservation control_observation =
                        ControlObservation::Transient,
                    const ControlState control_state =
                        ControlState::Uninitialized,
                    const ResultObservation result_observation =
                        ResultObservation::Running,
                    const CounterSnapshot counters = {},
                    const EnqueueOutcome enqueue_outcome =
                        EnqueueOutcome::None) noexcept
{
    return {control_observation, control_state, 0x1d, 0x9d, 0,
            result_observation, counters, enqueue_outcome, now};
}

CounterSnapshot press_queued() noexcept
{
    return {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
}

CounterSnapshot press_drained() noexcept
{
    return {1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, false};
}

CounterSnapshot release_queued() noexcept
{
    return {2, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, false};
}

CounterSnapshot release_drained() noexcept
{
    return {2, 2, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, false};
}

TimeoutConfig short_timeouts() noexcept
{
    return {1'000, 10, 10, 10, 10, 10, 10};
}

void reach_waiting_make(ValidationController &controller)
{
    controller.begin({}, 0);
    assert(controller.observe(input(1)) .action == Action::None);
    assert(controller.observe(input(2, ControlObservation::Accepted,
                                   ControlState::Ready))
               .action == Action::EnqueuePress);
    assert(controller.observe(input(3, ControlObservation::Accepted,
                                   ControlState::Ready, ResultObservation::Running,
                                   press_queued(), EnqueueOutcome::Enqueued))
               .action == Action::None);
    assert(controller.observe(input(4, ControlObservation::Transient,
                                   ControlState::Uninitialized,
                                   ResultObservation::Running, press_drained(),
                                   EnqueueOutcome::None))
               .state == State::WaitingMake);
}

void reach_waiting_break(ValidationController &controller)
{
    reach_waiting_make(controller);
    assert(controller.observe(input(5, ControlObservation::Accepted,
                                   ControlState::MakeObserved,
                                   ResultObservation::Running, press_drained()))
               .action == Action::EnqueueRelease);
    assert(controller.observe(input(6, ControlObservation::Transient,
                                   ControlState::Uninitialized,
                                   ResultObservation::Running, release_queued(),
                                   EnqueueOutcome::Enqueued))
               .action == Action::None);
    assert(controller.observe(input(7, ControlObservation::Transient,
                                   ControlState::Uninitialized,
                                   ResultObservation::Running, release_drained()))
               .state == State::WaitingBreak);
}

void test_normal_order_and_idempotence()
{
    ValidationController controller(short_timeouts());
    reach_waiting_break(controller);
    assert(controller.observe(input(8, ControlObservation::Accepted,
                                   ControlState::BreakObserved,
                                   ResultObservation::Running,
                                   release_drained()))
               .state == State::WaitingResult);
    assert(controller.observe(input(9, ControlObservation::Transient,
                                   ControlState::Uninitialized,
                                   ResultObservation::Pass, release_drained()))
               .action == Action::Complete);
    assert(controller.state() == State::Complete);
    assert(controller.proof_counters().enqueued == 2);
    assert(controller.proof_counters().dequeued == 2);
    assert(controller.proof_counters().press_injected == 1);
    assert(controller.proof_counters().release_injected == 1);
    assert(controller.observe(input(10, ControlObservation::Invalid,
                                   ControlState::Uninitialized,
                                   ResultObservation::Invalid))
               .action == Action::None);
}

void test_boot_ready_repetition_and_spacing()
{
    ValidationController controller(short_timeouts());
    controller.begin({}, 0);
    assert(controller.observe(input(1)).action == Action::None);
    assert(controller.observe(input(2, ControlObservation::Accepted,
                                   ControlState::Ready))
               .action == Action::EnqueuePress);
    assert(controller.observe(input(3, ControlObservation::Accepted,
                                   ControlState::Ready, ResultObservation::Running,
                                   press_queued()))
               .action == Action::None);

    ValidationController before_drain(short_timeouts());
    before_drain.begin({}, 0);
    assert(before_drain.observe(input(1, ControlObservation::Accepted,
                                      ControlState::Ready))
               .action == Action::EnqueuePress);
    assert(before_drain.observe(input(2, ControlObservation::Accepted,
                                      ControlState::MakeObserved,
                                      ResultObservation::Running,
                                      press_queued(),
                                      EnqueueOutcome::Enqueued))
               .failure_reason == FailureReason::MakeBeforePressDrain);
}

void test_enqueue_failures()
{
    ValidationController press(short_timeouts());
    press.begin({}, 0);
    assert(press.observe(input(1, ControlObservation::Accepted,
                               ControlState::Ready))
               .action == Action::EnqueuePress);
    assert(press.observe(input(2, ControlObservation::Transient,
                               ControlState::Uninitialized,
                               ResultObservation::Running, press_queued(),
                               EnqueueOutcome::Failed))
               .failure_reason == FailureReason::PressEnqueueFailed);

    ValidationController release(short_timeouts());
    reach_waiting_make(release);
    assert(release.observe(input(5, ControlObservation::Accepted,
                                 ControlState::MakeObserved,
                                 ResultObservation::Running, press_drained()))
               .action == Action::EnqueueRelease);
    assert(release.observe(input(6, ControlObservation::Transient,
                                 ControlState::Uninitialized,
                                 ResultObservation::Running, release_queued(),
                                 EnqueueOutcome::Failed))
               .failure_reason == FailureReason::ReleaseEnqueueFailed);
}

void test_guest_and_protocol_failures()
{
    ValidationController control_fail(short_timeouts());
    control_fail.begin({}, 0);
    assert(control_fail.observe(input(1, ControlObservation::Accepted,
                                       ControlState::Fail))
               .failure_reason == FailureReason::GuestControlFail);

    ValidationController result_fail(short_timeouts());
    result_fail.begin({}, 0);
    assert(result_fail.observe(input(1, ControlObservation::Transient,
                                     ControlState::Uninitialized,
                                     ResultObservation::Fail))
               .failure_reason == FailureReason::GuestResultFail);

    ValidationController invalid(short_timeouts());
    invalid.begin({}, 0);
    assert(invalid.observe(input(1, ControlObservation::Invalid,
                                 ControlState::Uninitialized,
                                 ResultObservation::PreProtocol))
               .failure_reason == FailureReason::ControlInvalid);

    ValidationController result_invalid(short_timeouts());
    result_invalid.begin({}, 0);
    assert(result_invalid.observe(input(1, ControlObservation::Transient,
                                        ControlState::Uninitialized,
                                        ResultObservation::Invalid))
               .failure_reason == FailureReason::ResultInvalid);
}

void test_counter_faults()
{
    ValidationController counter(short_timeouts());
    counter.begin({}, 0);
    auto bad = CounterSnapshot{};
    bad.queue_overflows = 1;
    assert(counter.observe(input(1, ControlObservation::Transient,
                                 ControlState::Uninitialized,
                                 ResultObservation::Running, bad))
               .failure_reason == FailureReason::CounterMismatch);

    ValidationController quarantine(short_timeouts());
    quarantine.begin({}, 0);
    auto bad_quarantine = CounterSnapshot{};
    bad_quarantine.quarantined = true;
    assert(quarantine.observe(input(1, ControlObservation::Transient,
                                    ControlState::Uninitialized,
                                    ResultObservation::Running,
                                    bad_quarantine))
               .failure_reason == FailureReason::KeyboardFault);
}

void test_timeouts()
{
    ValidationController ready(short_timeouts());
    ready.begin({}, 0);
    assert(ready.observe(input(10)).failure_reason == FailureReason::ReadyTimeout);

    ValidationController press(short_timeouts());
    press.begin({}, 0);
    assert(press.observe(input(1, ControlObservation::Accepted,
                               ControlState::Ready))
               .action == Action::EnqueuePress);
    assert(press.observe(input(11, ControlObservation::Transient,
                               ControlState::Uninitialized,
                               ResultObservation::Running, press_queued(),
                               EnqueueOutcome::Enqueued))
               .failure_reason == FailureReason::PressDrainTimeout);

    ValidationController make(short_timeouts());
    reach_waiting_make(make);
    assert(make.observe(input(14, ControlObservation::Transient,
                              ControlState::Uninitialized,
                              ResultObservation::Running, press_drained()))
               .failure_reason == FailureReason::MakeTimeout);

    ValidationController release(short_timeouts());
    reach_waiting_make(release);
    assert(release.observe(input(5, ControlObservation::Accepted,
                                 ControlState::MakeObserved,
                                 ResultObservation::Running, press_drained()))
               .action == Action::EnqueueRelease);
    assert(release.observe(input(15, ControlObservation::Transient,
                                 ControlState::Uninitialized,
                                 ResultObservation::Running, release_queued(),
                                 EnqueueOutcome::Enqueued))
               .failure_reason == FailureReason::ReleaseDrainTimeout);

    ValidationController break_controller(short_timeouts());
    reach_waiting_break(break_controller);
    assert(break_controller.observe(input(17, ControlObservation::Transient,
                                          ControlState::Uninitialized,
                                          ResultObservation::Running,
                                          release_drained()))
               .failure_reason == FailureReason::BreakTimeout);

    ValidationController result(short_timeouts());
    reach_waiting_break(result);
    assert(result.observe(input(8, ControlObservation::Accepted,
                                ControlState::BreakObserved,
                                ResultObservation::Running,
                                release_drained()))
               .state == State::WaitingResult);
    assert(result.observe(input(18, ControlObservation::Transient,
                                ControlState::Uninitialized,
                                ResultObservation::Running, release_drained()))
               .failure_reason == FailureReason::ResultTimeout);
}

void test_before_release_drain_and_pass_order()
{
    ValidationController controller(short_timeouts());
    reach_waiting_make(controller);
    assert(controller.observe(input(5, ControlObservation::Accepted,
                                     ControlState::MakeObserved,
                                     ResultObservation::Running,
                                     press_drained()))
               .action == Action::EnqueueRelease);
    assert(controller.observe(input(6, ControlObservation::Accepted,
                                    ControlState::BreakObserved,
                                    ResultObservation::Running, release_queued(),
                                    EnqueueOutcome::Enqueued))
               .failure_reason == FailureReason::BreakBeforeReleaseDrain);

    ValidationController pass_first(short_timeouts());
    reach_waiting_make(pass_first);
    assert(pass_first.observe(input(5, ControlObservation::Transient,
                                    ControlState::Uninitialized,
                                    ResultObservation::Pass, press_drained()))
               .action == Action::None);
    assert(pass_first.state() == State::WaitingMake);
    assert(pass_first.observe(input(6, ControlObservation::Accepted,
                                    ControlState::MakeObserved,
                                    ResultObservation::Pass,
                                    press_drained()))
               .action == Action::EnqueueRelease);
}

void test_terminal_failure_and_strings()
{
    ValidationController controller(short_timeouts());
    assert(controller.fail(FailureReason::RuntimeFatal).action == Action::Fail);
    assert(controller.fail(FailureReason::CounterMismatch).action == Action::None);
    assert(controller.state() == State::Failed);
    assert(to_string(FailureReason::RuntimeFatal) != nullptr);
    assert(to_string(State::WaitingResult) != nullptr);
    assert(to_string(Action::EnqueueRelease) != nullptr);
}

} // namespace

int main()
{
    test_normal_order_and_idempotence();
    test_boot_ready_repetition_and_spacing();
    test_enqueue_failures();
    test_guest_and_protocol_failures();
    test_counter_faults();
    test_timeouts();
    test_before_release_drain_and_pass_order();
    test_terminal_failure_and_strings();
    return 0;
}
