#!/usr/bin/env python3
"""Deterministic mutation gate for the 86R.5C.2 PCM output validator."""

from __future__ import annotations

from test_p4_audio86_real_guest_validator import valid_log
from validate_p4_audio86_pcm_output_log import (
    FULL_SHA, PRE_SHA, validate_lifecycle, validate_pcm,
)


def pcm_log() -> str:
    lines = [
        "RING_PRE_RESET_PCM_SHA256=" + PRE_SHA,
        "RING_FULL_PCM_SHA256=" + FULL_SHA,
        "RING_FULL_PCM_FRAMES=2400",
        "P4_AUDIO86_PCM_OUTPUT profile=1 producer_core=0 producer_priority=6 consumer_core=0 consumer_priority=7 consumer_index=0 prefill=4 ring_capacity=8 ring_quantum=240 ring_bytes=7832 consumer_stack=4096 internal=1 psram_fallback=NO i2s_active=0 physical_timing_validated=0",
        "P4_AUDIO86_PCM_RESET rendered=13 ring_owned=13 applied_after_ring=1 ack_after_ring=1 forced_publish=0 first_slot_valid=240",
        "P4_AUDIO86_PCM_COMPLETION ring_finished=1 pcm_done=1 worker_quiescent=1 consumer_ack=1 consumer_quiescent=1 worker_suspended=1 consumer_suspended=1 worker_deleted_after_suspended=1 consumer_deleted_after_suspended=1 worker_join_timeout=0 consumer_join_timeout=0 sink_started=1 sink_finished=1 ring_before_done=1 eos_after_done=1 finish_after_empty=1 ack_after_finish=1",
        "P4_AUDIO86_PCM_RESIDUAL occupancy=0 partial=0 produced_frames=2400 consumed_frames=2400 produced_bytes=9600 consumed_bytes=9600 produced_slots=10 consumed_slots=10 partial_slots=0 drops=0 overwrite=0 sequence_errors=0 offset_errors=0 forced_abort=0 abandoned_published=0 abandoned_partial=0 abandoned_rendered=0 first_submit_occupancy=4",
        "P4_AUDIO86_PCM_DIRECT_RING_EQUAL=1",
        "P4_AUDIO86_PCM_OUTPUT_RESULT=PASS",
        "P4_AUDIO86_PCM_LIFECYCLE scenario=NONE triggered=0 forced_abort=0 forced_before_wake=0 ring_finished=1 pcm_done=1 worker_quiescent=1 consumer_ack=1 consumer_quiescent=1 worker_suspended=1 consumer_suspended=1 worker_deleted_after_suspended=1 consumer_deleted_after_suspended=1 worker_join_timeout=0 consumer_join_timeout=0 sink_abort_calls=0 worker_waiting=0 pre_cleanup_occupancy=0 pre_cleanup_partial=0 final_occupancy=0 final_partial=0 produced_frames=2400 consumed_frames=2400 abandoned_published=0 abandoned_partial=0 abandoned_rendered=0 first_error=0 result=PASS",
    ]
    for slot in range(10):
        lines.append(f"P4_AUDIO86_PCM_SLOT sequence={slot} frame_offset={slot * 240} valid_frames=240 flags=0 crc32=12345678")
    return valid_log() + "\n".join(lines) + "\n"


def changed(text: str, before: str, after: str) -> str:
    assert text.count(before) == 1, before
    return text.replace(before, after)


def changed_last(text: str, before: str, after: str) -> str:
    assert before in text, before
    head, tail = text.rsplit(before, 1)
    return head + after + tail


def forced_log() -> str:
    return valid_log() + (
        "P4_AUDIO86_PCM_LIFECYCLE scenario=CONSUMER_FAILURE_FULL triggered=1 "
        "forced_abort=1 forced_before_wake=1 ring_finished=0 pcm_done=0 "
        "worker_quiescent=1 consumer_ack=1 consumer_quiescent=1 "
        "worker_suspended=1 consumer_suspended=1 "
        "worker_deleted_after_suspended=1 consumer_deleted_after_suspended=1 "
        "worker_join_timeout=0 consumer_join_timeout=0 sink_abort_calls=1 "
        "worker_waiting=0 pre_cleanup_occupancy=8 pre_cleanup_partial=0 "
        "final_occupancy=0 final_partial=0 produced_frames=1920 consumed_frames=0 "
        "abandoned_published=1920 abandoned_partial=0 abandoned_rendered=240 "
        "first_error=2 result=PASS\n"
    )


def main() -> None:
    good = pcm_log()
    validate_pcm(good)
    mutations = {
        "consumer_core": ("consumer_core=0", "consumer_core=1"),
        "consumer_priority": ("consumer_priority=7", "consumer_priority=6"),
        "consumer_index": ("consumer_index=0", "consumer_index=1"),
        "ring_capacity": ("ring_capacity=8", "ring_capacity=7"),
        "slot_sequence": ("sequence=3 frame_offset=720", "sequence=4 frame_offset=720"),
        "frame_offset": ("sequence=3 frame_offset=720", "sequence=3 frame_offset=721"),
        "valid_frames": ("sequence=3 frame_offset=720 valid_frames=240", "sequence=3 frame_offset=720 valid_frames=239"),
        "unexpected_partial": ("partial_slots=0", "partial_slots=1"),
        "payload_hash": ("RING_FULL_PCM_SHA256=" + FULL_SHA, "RING_FULL_PCM_SHA256=" + "0" * 64),
        "pre_reset_hash": ("RING_PRE_RESET_PCM_SHA256=" + PRE_SHA, "RING_PRE_RESET_PCM_SHA256=" + "0" * 64),
        "prefill": ("first_submit_occupancy=4", "first_submit_occupancy=3"),
        "duplicate_consume": ("consumed_slots=10", "consumed_slots=11"),
        "reordered_consume": ("sequence=4 frame_offset=960", "sequence=5 frame_offset=960"),
        "drop": ("drops=0", "drops=1"),
        "overwrite": ("overwrite=0", "overwrite=1"),
        "reset_ack_order": ("ack_after_ring=1", "ack_after_ring=0"),
        "pcm_done_order": ("ring_before_done=1", "ring_before_done=0"),
        "consumer_eos_order": ("eos_after_done=1", "eos_after_done=0"),
        "sink_finish_order": ("finish_after_empty=1", "finish_after_empty=0"),
        "terminal_ack_order": ("ack_after_finish=1", "ack_after_finish=0"),
        "final_occupancy": (
            "P4_AUDIO86_PCM_RESIDUAL occupancy=0",
            "P4_AUDIO86_PCM_RESIDUAL occupancy=1"),
        "worker_quiescent": (
            "P4_AUDIO86_PCM_COMPLETION ring_finished=1 pcm_done=1 "
            "worker_quiescent=1 consumer_ack=1",
            "P4_AUDIO86_PCM_COMPLETION ring_finished=1 pcm_done=1 "
            "worker_quiescent=0 consumer_ack=1"),
        "consumer_quiescent": (
            "P4_AUDIO86_PCM_COMPLETION ring_finished=1 pcm_done=1 "
            "worker_quiescent=1 consumer_ack=1 consumer_quiescent=1 "
            "worker_suspended=1",
            "P4_AUDIO86_PCM_COMPLETION ring_finished=1 pcm_done=1 "
            "worker_quiescent=1 consumer_ack=1 consumer_quiescent=0 "
            "worker_suspended=1"),
        "first_error": (
            "events=0 bytes=0 horizon=0 first_error=0 pcm_fifo=1",
            "events=0 bytes=0 horizon=0 first_error=1 pcm_fifo=1"),
        "i2s_active": ("i2s_active=0", "i2s_active=1"),
        "physical_timing": ("physical_timing_validated=0", "physical_timing_validated=1"),
    }
    for name, (before, after) in mutations.items():
        try:
            validate_pcm(changed(good, before, after))
        except SystemExit:
            print(f"5C2_VALIDATOR_MUTATION name={name} result=REJECTED")
        else:
            raise SystemExit(f"ERROR: mutation accepted: {name}")
    lifecycle_mutations = {
        "quiescent_without_suspended": ("consumer_suspended=1", "consumer_suspended=0"),
        "terminal_ack_missing": ("consumer_ack=1", "consumer_ack=0"),
        "delete_before_suspended": ("consumer_deleted_after_suspended=1", "consumer_deleted_after_suspended=0"),
        "pcm_join_false": ("consumer_quiescent=1", "consumer_quiescent=0"),
        "pcm_join_timeout": ("consumer_join_timeout=0", "consumer_join_timeout=1"),
        "healthy_ring_residual": ("final_occupancy=0", "final_occupancy=1"),
        "healthy_stop_abandon": ("abandoned_published=0", "abandoned_published=1"),
        "healthy_fatal_abandon": ("abandoned_partial=0", "abandoned_partial=1"),
        "worker_still_waiting": ("worker_waiting=0", "worker_waiting=1"),
        "failure_ok_without_quiescence": ("worker_quiescent=1", "worker_quiescent=0"),
    }
    for name, (before, after) in lifecycle_mutations.items():
        try:
            validate_pcm(changed_last(good, before, after))
        except SystemExit:
            print(f"5C2_LIFECYCLE_MUTATION name={name} result=REJECTED")
        else:
            raise SystemExit(f"ERROR: lifecycle mutation accepted: {name}")
    forced = forced_log()
    validate_lifecycle(forced, "consumer-failure-full")
    forced_mutations = {
        "forced_abort_missing": ("forced_abort=1", "forced_abort=0"),
        "forced_abort_after_wake": ("forced_before_wake=1", "forced_before_wake=0"),
        "sink_abort_missing": ("sink_abort_calls=1", "sink_abort_calls=0"),
        "abandonment_mismatch": ("abandoned_published=1920", "abandoned_published=1919"),
        "empty_wait_still_blocked": ("consumer_quiescent=1", "consumer_quiescent=0"),
    }
    for name, (before, after) in forced_mutations.items():
        try:
            validate_lifecycle(changed(forced, before, after),
                               "consumer-failure-full")
        except SystemExit:
            print(f"5C2_LIFECYCLE_MUTATION name={name} result=REJECTED")
        else:
            raise SystemExit(f"ERROR: forced lifecycle mutation accepted: {name}")
    print(f"5C2_VALIDATOR_MUTATIONS={len(mutations)}_ALL_REJECTED")
    total_lifecycle = len(lifecycle_mutations) + len(forced_mutations)
    print(f"5C2_LIFECYCLE_MUTATIONS={total_lifecycle}_ALL_REJECTED")


if __name__ == "__main__":
    main()
