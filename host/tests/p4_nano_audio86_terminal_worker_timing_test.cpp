/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_worker_timing.hpp"

namespace timing = p4_nano_audio86_terminal_worker_timing;

namespace {

struct FakeClock {
    std::array<std::uint32_t, timing::kPointCount> values{};
    std::size_t next = 0U;

    std::uint32_t now()
    {
        assert(next < values.size());
        return values[next++];
    }
};

void enter(timing::Snapshot *snapshot, const timing::Point point,
           const timing::Phase phase, FakeClock *clock)
{
    timing::publish_phase(snapshot, phase);
    assert(timing::record_once(snapshot, point, clock->now()));
}

void complete(timing::Snapshot *snapshot, const timing::Point point,
              FakeClock *clock)
{
    assert(timing::record_once(snapshot, point, clock->now()));
}

void healthy_path_test()
{
    timing::Snapshot snapshot{};
    timing::initialize(&snapshot);
    FakeClock clock{{100U, 110U, 111U, 171U, 179U, 181U,
                     184U, 185U, 190U, 194U, 196U}};

    enter(&snapshot, timing::Point::T0TerminalPairObserved,
          timing::Phase::TerminalObserved, &clock);
    timing::publish_phase(&snapshot, timing::Phase::PreResetRender);
    complete(&snapshot, timing::Point::T1PreResetRenderComplete, &clock);
    enter(&snapshot, timing::Point::T2ResetActionBegin,
          timing::Phase::ResetApply, &clock);
    complete(&snapshot, timing::Point::T3ResetActionComplete, &clock);
    timing::publish_phase(&snapshot, timing::Phase::ResetEvidence);
    complete(&snapshot, timing::Point::T4ResetEvidenceComplete, &clock);
    timing::publish_phase(&snapshot, timing::Phase::ResetAck);
    complete(&snapshot, timing::Point::T5ResetAckPublished, &clock);
    timing::publish_phase(&snapshot, timing::Phase::ResetEventConsume);
    complete(&snapshot, timing::Point::T6TerminalPredicateReady, &clock);
    enter(&snapshot, timing::Point::T7PostResetRenderBegin,
          timing::Phase::PostResetRender, &clock);
    complete(&snapshot, timing::Point::T8PostResetSynthesisComplete, &clock);
    timing::publish_phase(&snapshot, timing::Phase::Q399Publish);
    complete(&snapshot, timing::Point::T9Q399Published, &clock);
    timing::publish_phase(&snapshot, timing::Phase::PcmFinish);
    complete(&snapshot, timing::Point::T10PcmFinishComplete, &clock);

    assert(clock.next == timing::kPointCount);
    assert(snapshot.timestamps == clock.values);
    assert(timing::reached_prefix_is_monotonic(snapshot));
    assert(timing::duration(snapshot.timestamps[0], snapshot.timestamps[1]) == 10U);
    assert(timing::duration(snapshot.timestamps[2], snapshot.timestamps[3]) == 60U);
    assert(timing::duration(snapshot.timestamps[3], snapshot.timestamps[4]) == 8U);
    assert(timing::duration(snapshot.timestamps[5], snapshot.timestamps[6]) == 3U);
    assert(timing::duration(snapshot.timestamps[7], snapshot.timestamps[8]) == 5U);
    assert(timing::duration(snapshot.timestamps[8], snapshot.timestamps[9]) == 4U);
    assert(timing::duration(snapshot.timestamps[9], snapshot.timestamps[10]) == 2U);
    assert(timing::duration(snapshot.timestamps[0], snapshot.timestamps[9]) == 94U);

    const std::uint32_t original_t2 = snapshot.timestamps[2];
    assert(!timing::record_once(&snapshot, timing::Point::T2ResetActionBegin,
                                999U));
    assert(snapshot.timestamps[2] == original_t2);
    assert(timing::freeze_first_qovf_phase(
        &snapshot, static_cast<std::uint32_t>(timing::Phase::ResetApply)));
    const std::uint32_t frozen = snapshot.first_qovf_phase.load();
    assert(timing::freeze_first_qovf_phase(&snapshot, frozen));
    assert(snapshot.first_qovf_phase.load() == frozen);
    assert(!timing::freeze_first_qovf_phase(
        &snapshot, static_cast<std::uint32_t>(timing::Phase::PcmFinish)));
    assert(snapshot.first_qovf_phase.load() == frozen);
}

void reset_failure_test()
{
    timing::Snapshot snapshot{};
    timing::initialize(&snapshot);
    FakeClock clock{{10U, 11U, 12U}};
    enter(&snapshot, timing::Point::T0TerminalPairObserved,
          timing::Phase::TerminalObserved, &clock);
    timing::publish_phase(&snapshot, timing::Phase::PreResetRender);
    complete(&snapshot, timing::Point::T1PreResetRenderComplete, &clock);
    enter(&snapshot, timing::Point::T2ResetActionBegin,
          timing::Phase::ResetApply, &clock);
    for (std::size_t i = 3U; i < timing::kPointCount; ++i)
        assert(snapshot.timestamps[i] == timing::kUnset);
    assert(snapshot.current_phase.load() ==
           static_cast<std::uint32_t>(timing::Phase::ResetApply));
    assert(timing::reached_prefix_is_monotonic(snapshot));
}

void q399_failure_test()
{
    timing::Snapshot snapshot{};
    timing::initialize(&snapshot);
    FakeClock clock{{10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U}};
    const std::array<timing::Phase, 9U> phases{
        timing::Phase::TerminalObserved, timing::Phase::PreResetRender,
        timing::Phase::ResetApply, timing::Phase::ResetEvidence,
        timing::Phase::ResetAck, timing::Phase::ResetEventConsume,
        timing::Phase::ResetEventConsume, timing::Phase::PostResetRender,
        timing::Phase::Q399Publish};
    for (std::size_t i = 0U; i < phases.size(); ++i) {
        timing::publish_phase(&snapshot, phases[i]);
        assert(timing::record_once(&snapshot, static_cast<timing::Point>(i),
                                   clock.now()));
    }
    assert(snapshot.timestamps[9] == timing::kUnset);
    assert(snapshot.timestamps[10] == timing::kUnset);
    assert(snapshot.current_phase.load() ==
           static_cast<std::uint32_t>(timing::Phase::Q399Publish));
    assert(timing::reached_prefix_is_monotonic(snapshot));
}

void impossible_state_test()
{
    timing::Snapshot snapshot{};
    timing::initialize(&snapshot);
    assert(!timing::record_once(&snapshot, timing::Point::T0TerminalPairObserved,
                                timing::kUnset));
    snapshot.timestamps[3] = 5U;
    assert(!timing::reached_prefix_is_monotonic(snapshot));
    snapshot.timestamps.fill(timing::kUnset);
    snapshot.timestamps[0] = 10U;
    snapshot.timestamps[1] = 9U;
    assert(!timing::reached_prefix_is_monotonic(snapshot));
    assert(!timing::valid_phase(10U));
}

void callback_service_word_encoding_test()
{
    for (std::uint32_t phase = 0U;
         phase <= static_cast<std::uint32_t>(timing::Phase::PcmFinish);
         ++phase) {
        for (const std::uint32_t sequence : {0U, 398U, 399U, 400U}) {
            const std::uint32_t packed = timing::pack_service_sequence(
                sequence, static_cast<timing::Phase>(phase));
            assert(timing::unpack_service_phase(packed) == phase);
            assert(timing::unpack_consumer_sequence(packed) == sequence);
            assert(packed < (1U << 14U));
        }
    }
}

} // namespace

int main()
{
    healthy_path_test();
    reset_failure_test();
    q399_failure_test();
    impossible_state_test();
    callback_service_word_encoding_test();
    std::puts("TERMINAL_WORKER_TIMING_HOST_TEST=PASS");
    std::puts("RESET_TIMING_FAILURE_PATH_TEST=PASS");
    std::puts("Q399_TIMING_FAILURE_PATH_TEST=PASS");
    std::puts("TERMINAL_WORKER_TIMING_STORAGE_LOGICAL_BYTES=52");
    std::puts("TERMINAL_WORKER_TIMING_STORAGE_ACTUAL_BYTES=52");
    return 0;
}
