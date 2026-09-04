#include <atomic>
#include <cassert>
#include <cstdint>
#include <string_view>
#include <thread>

#include "p4_nano_audio86_guest_binding/p4_nano_audio86_owner_progress_diagnostics.hpp"

using namespace p4_nano_audio86_guest_binding;

static OwnerCheckpointInput make_record(std::uint64_t wall,
                                        std::uint64_t cycles,
                                        std::uint64_t frames,
                                        std::uint16_t ip)
{
    OwnerCheckpointInput input{};
    input.wall_time_us = wall;
    input.guest_cycle = cycles;
    input.guest_frame = frames;
    input.cs = 0x1234U;
    input.ip = ip;
    input.flags = 0x0202U;
    input.cx = static_cast<std::uint16_t>(0x8000U - ip);
    input.bp = 59U;
    input.interrupt_enabled = 1U;
    input.next_opcode = 0xe2U;
    input.timer_a_running = 1U;
    input.timer_b_running = 1U;
    input.opna_status = 2U;
    input.pic_pending = 0x0102U;
    input.pic_mask = 0xfbf8U;
    input.next_nevent_id = 5U;
    input.next_nevent_remaining = 12345;
    input.last_guest_io_ordinal = 185U;
    input.last_guest_io_port = 0x0188U;
    input.last_guest_io_frame = frames - 100U;
    input.last_guest_io_cycle = cycles - 1000U;
    input.published_horizon = frames - 1U;
    input.rendered_frame = frames - 2U;
    return input;
}

int main()
{
    OwnerProgressDiagnostics diagnostics;
    diagnostics.reset();
    auto empty = diagnostics.snapshot();
    assert(empty.coherent == 1U);
    assert(empty.checkpoint_count == 0U);
    assert(empty.pattern == OwnerProgressPattern::InsufficientHistory);
    assert(empty.checkpoint_last_result == kOwnerDiagnosticUnavailable);
    assert(empty.checkpoint_last_frame == UINT64_MAX);

    diagnostics.publish_subphase(OwnerSubphase::CpuExec);
    diagnostics.checkpoint_enter(100U, UINT64_C(0x100000002));
    diagnostics.checkpoint_exit(150U, 0U, true);
    diagnostics.checkpoint_enter(200U, UINT64_C(0x200000003));
    diagnostics.checkpoint_exit(400U, 3U, false);
    const auto counters = diagnostics.snapshot();
    assert(counters.subphase == OwnerSubphase::CpuExec);
    assert(counters.checkpoint_call_count == 2U);
    assert(counters.checkpoint_success_count == 1U);
    assert(counters.checkpoint_last_enter_us == 200U);
    assert(counters.checkpoint_last_exit_us == 400U);
    assert(counters.checkpoint_max_duration_us == 200U);
    assert(counters.checkpoint_last_result == 3U);
    assert(counters.checkpoint_last_frame == UINT64_C(0x200000003));
    diagnostics.publish_subphase(OwnerSubphase::ProgressCheckpointWait);
    assert(diagnostics.snapshot().subphase ==
           OwnerSubphase::ProgressCheckpointWait);
    diagnostics.publish_subphase(OwnerSubphase::ProgressCheckpointExit);
    assert(diagnostics.snapshot().subphase ==
           OwnerSubphase::ProgressCheckpointExit);

    for (std::uint32_t index = 0U; index < 6U; ++index) {
        diagnostics.publish_checkpoint(make_record(
            UINT64_C(0x100000000) + index * 250000U,
            UINT64_C(0x200000000) + index * 1000U,
            UINT64_C(0x300000000) + index * 100U,
            static_cast<std::uint16_t>(0x1300U + index)));
    }
    const auto wrapped = diagnostics.snapshot();
    assert(wrapped.coherent == 1U);
    assert(wrapped.checkpoint_count == 4U);
    assert(wrapped.history[0].checkpoint_sequence == 3U);
    assert(wrapped.history[3].checkpoint_sequence == 6U);
    assert(wrapped.history[3].wall_time_us ==
           UINT64_C(0x100000000) + 5U * 250000U);
    assert(wrapped.history[3].guest_cycle ==
           UINT64_C(0x200000000) + 5U * 1000U);
    assert(wrapped.history[3].guest_frame ==
           UINT64_C(0x300000000) + 5U * 100U);
    assert(wrapped.history[3].cs == 0x1234U);
    assert(wrapped.history[3].ip == 0x1305U);
    assert(wrapped.history[3].flags == 0x0202U);
    assert(wrapped.history[3].interrupt_enabled == 1U);
    assert(wrapped.history[3].cx ==
           static_cast<std::uint16_t>(0x8000U - 0x1305U));
    assert(wrapped.history[3].bp == 59U);
    assert(wrapped.history[3].next_opcode == 0xe2U);
    assert(wrapped.history[3].hlt == 0U);
    assert(wrapped.history[3].timer_a_running == 1U);
    assert(wrapped.history[3].timer_b_running == 1U);
    assert(wrapped.history[3].opna_status == 2U);
    assert(wrapped.history[3].pic_pending == 0x0102U);
    assert(wrapped.history[3].pic_mask == 0xfbf8U);
    assert(wrapped.history[3].next_nevent_id == 5U);
    assert(wrapped.history[3].next_nevent_remaining == 12345);
    assert(wrapped.history[3].last_guest_io_ordinal == 185U);
    assert(wrapped.history[3].last_guest_io_port == 0x0188U);
    assert(wrapped.history[3].last_guest_io_frame ==
           UINT64_C(0x300000000) + 5U * 100U - 100U);
    assert(wrapped.history[3].last_guest_io_cycle ==
           UINT64_C(0x200000000) + 5U * 1000U - 1000U);
    assert(wrapped.intervals[2].delta_wall_us == 250000U);
    assert(wrapped.intervals[2].delta_guest_cycles == 1000U);
    assert(wrapped.intervals[2].guest_cycles_per_second == 4000U);
    assert(wrapped.intervals[2].delta_guest_frames == 100U);
    assert(wrapped.intervals[2].guest_frames_per_second == 400U);

    OwnerProgressDiagnostics decelerating;
    decelerating.reset();
    std::uint64_t cycles = 0U;
    const std::uint64_t gains[] = {0U, 3000U, 2000U, 1000U};
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        cycles += gains[index];
        decelerating.publish_checkpoint(make_record(index * 250000U,
                                                     cycles, index * 100U,
                                                     index));
    }
    assert(decelerating.snapshot().pattern ==
           OwnerProgressPattern::Decelerating);

    OwnerProgressDiagnostics stopped;
    stopped.reset();
    stopped.publish_checkpoint(make_record(100U, 2000U, 300U, 1U));
    stopped.publish_checkpoint(make_record(200U, 2000U, 300U, 1U));
    assert(stopped.snapshot().pattern == OwnerProgressPattern::Stopped);
    assert(owner_progress_pattern_name(OwnerProgressPattern::Stopped) ==
           std::string_view("STOPPED"));
    assert(owner_subphase_name(OwnerSubphase::TransactionWait) ==
           std::string_view("TRANSACTION_WAIT"));

    OwnerProgressDiagnostics concurrent;
    concurrent.reset();
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> coherent_reads{0U};
    std::thread reader([&]() {
        start.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
            const auto snapshot = concurrent.snapshot();
            if (snapshot.coherent == 0U)
                continue;
            coherent_reads.fetch_add(1U, std::memory_order_relaxed);
            for (std::uint32_t index = 0U;
                 index < snapshot.checkpoint_count; ++index) {
                const auto &record = snapshot.history[index];
                assert(record.valid == 1U);
                const std::uint64_t generation =
                    record.checkpoint_sequence - 1U;
                assert(record.guest_cycle ==
                       (UINT64_C(0xabcddcba) << 32U) + generation);
                assert(record.guest_frame ==
                       (UINT64_C(0x12344321) << 32U) + generation);
            }
        }
    });
    while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
    for (std::uint32_t generation = 0U; generation < 20000U; ++generation) {
        concurrent.publish_checkpoint(make_record(
            generation + 1U,
            (UINT64_C(0xabcddcba) << 32U) + generation,
            (UINT64_C(0x12344321) << 32U) + generation,
            static_cast<std::uint16_t>(generation)));
        if ((generation & 63U) == 0U)
            std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    reader.join();
    assert(coherent_reads.load(std::memory_order_relaxed) != 0U);
    const auto concurrent_final = concurrent.snapshot();
    assert(concurrent_final.coherent == 1U);
    assert(concurrent_final.history[3].checkpoint_sequence == 20000U);
    assert(concurrent_final.history[3].guest_cycle ==
           (UINT64_C(0xabcddcba) << 32U) + 19999U);
    assert(concurrent_final.history[3].guest_frame ==
           (UINT64_C(0x12344321) << 32U) + 19999U);
    return 0;
}
