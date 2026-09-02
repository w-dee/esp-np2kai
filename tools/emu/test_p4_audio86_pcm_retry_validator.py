#!/usr/bin/env python3
"""Mutation gate for the 86R.5C.3-S1 RETRY validator."""

from __future__ import annotations

from validate_p4_audio86_pcm_retry_log import validate_retry


def log(scenario: str) -> str:
    values = {
        "retry-stop": ("RETRY_STOP", "0", "0", "1", "1", "0", "2400", "2400", "0", "0", "1", "1", "2400", "9600"),
        "retry-fatal": ("RETRY_FATAL", "0", "86", "1", "1", "0", "2400", "2400", "0", "0", "1", "1", "2400", "9600"),
        "retry-primary-first": ("RETRY_PRIMARY_FIRST", "1", "86", "0", "0", "1", "1920", "0", "1920", "8", "0", "0", "0", "0"),
        "retry-consumer-first": ("RETRY_CONSUMER_FIRST", "1", "2", "0", "0", "1", "1920", "0", "1920", "8", "0", "0", "0", "0"),
    }
    (name, forced, error, finished, done, abort_calls, produced, consumed,
     abandoned, pre_occupancy, worker_resumed, done_empty, accepted_frames,
     accepted_bytes) = values[scenario]
    forced_before = forced
    tail_after = "1" if forced == "0" else "0"
    result = (
        "ESP-ROM:esp32p4\n"
        "main_task: Returned from app_main()\n"
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS\n"
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS\n"
        f"P4_AUDIO86_PCM_LIFECYCLE scenario={name} triggered=1 "
        f"forced_abort={forced} forced_before_wake={forced_before} "
        f"ring_finished={finished} pcm_done={done} worker_quiescent=1 "
        "consumer_ack=1 consumer_quiescent=1 worker_suspended=1 "
        "consumer_suspended=1 worker_deleted_after_suspended=1 "
        "consumer_deleted_after_suspended=1 worker_join_timeout=0 "
        f"consumer_join_timeout=0 sink_abort_calls={abort_calls} "
        f"worker_waiting=0 pre_cleanup_occupancy={pre_occupancy} "
        "pre_cleanup_partial=0 final_occupancy=0 final_partial=0 "
        f"produced_frames={produced} consumed_frames={consumed} "
        f"abandoned_published={abandoned} abandoned_partial=0 "
        f"abandoned_rendered={'0' if forced == '0' else '13'} "
        f"first_error={error} result=PASS\n"
        f"P4_AUDIO86_PCM_RETRY scenario={name} attempts=2 wakes=1 "
        "resubmits=1 identity=1 tail_held=1 accepted_held=1 "
        f"full_occupancy=8 worker_resumed={worker_resumed} "
        "permission_before_wake=1 wait_skipped_ready=0 "
        f"done_only_after_empty={done_empty} tail_before=0 "
        f"tail_after={tail_after} accepted_frames_before=0 "
        f"accepted_frames_after={accepted_frames} accepted_bytes_before=0 "
        f"accepted_bytes_after={accepted_bytes} sequence=0 frame_offset=0 "
        f"valid_frames=240 flags=0 crc32=d065c969 forced_abort={forced} "
        f"first_error={error} result=PASS\n"
    )
    if forced == "0":
        result += (
            f"P4_AUDIO86_PCM_POST_DONE_RETRY scenario={name} attempts=2 "
            "resubmits=1 identity=1 tail_held=1 accepted_held=1 "
            "observed_occupancy=1 not_eos=1 permission_before_wake=1 "
            "tail_before=9 tail_after=10 accepted_frames_before=2160 "
            "accepted_bytes_before=8640 crc32=49d839a8 result=PASS\n"
        )
    return result


def changed(text: str, before: str, after: str) -> str:
    assert text.count(before) >= 1, before
    return text.replace(before, after, 1)


def main() -> None:
    goods = {scenario: log(scenario) for scenario in (
        "retry-stop", "retry-fatal", "retry-primary-first",
        "retry-consumer-first")}
    for scenario, text in goods.items():
        validate_retry(text, scenario)

    mutations = {
        "retry_advances_tail": ("retry-stop", "tail_held=1", "tail_held=0"),
        "retry_accepts_frames": ("retry-stop", "accepted_frames_before=0", "accepted_frames_before=240"),
        "retry_accepts_bytes": ("retry-stop", "accepted_bytes_before=0", "accepted_bytes_before=960"),
        "sequence_changes": ("retry-stop", "sequence=0", "sequence=1"),
        "frame_offset_changes": ("retry-stop", "frame_offset=0", "frame_offset=240"),
        "payload_changes": ("retry-stop", "crc32=d065c969", "crc32=00000000"),
        "valid_frames_changes": ("retry-stop", "valid_frames=240", "valid_frames=239"),
        "stop_forced_abort": ("retry-stop", "forced_abort=0", "forced_abort=1"),
        "fatal_forced_abort": ("retry-fatal", "forced_abort=0", "forced_abort=1"),
        "primary_error_overwritten": ("retry-primary-first", "first_error=86", "first_error=2"),
        "consumer_error_overwritten": ("retry-consumer-first", "first_error=2", "first_error=86"),
        "abort_after_wake": ("retry-primary-first", "forced_before_wake=1", "forced_before_wake=0"),
        "abort_counted_consumed": ("retry-primary-first", "consumed_frames=0", "consumed_frames=240"),
        "abort_abandoned_twice": ("retry-primary-first", "abandoned_published=1920", "abandoned_published=3840"),
        "eos_with_retry": ("retry-stop", "done_only_after_empty=1", "done_only_after_empty=0"),
        "worker_never_resumes": ("retry-stop", "worker_resumed=1", "worker_resumed=0"),
        "slot_not_resubmitted": ("retry-stop", "resubmits=1", "resubmits=0"),
        "slot_identity_false": ("retry-stop", "identity=1", "identity=0"),
        "permission_after_wake": ("retry-stop", "permission_before_wake=1", "permission_before_wake=0"),
        "ring_not_full": ("retry-stop", "full_occupancy=8", "full_occupancy=7"),
        "post_done_retry_missing": ("retry-stop", "observed_occupancy=1", "observed_occupancy=0"),
        "post_done_eos_early": ("retry-stop", "not_eos=1", "not_eos=0"),
        "post_done_identity": ("retry-stop", "crc32=49d839a8", "crc32=00000000"),
    }
    rejected = 0
    for name, (scenario, before, after) in mutations.items():
        try:
            validate_retry(changed(goods[scenario], before, after), scenario)
        except SystemExit:
            rejected += 1
            print(f"5C3S1_VALIDATOR_MUTATION name={name} result=REJECTED")
        else:
            raise SystemExit(f"ERROR: mutation accepted: {name}")
    print("5C3S1_VALIDATOR=PASS")
    print(f"5C3S1_VALIDATOR_MUTATIONS={rejected}_ALL_REJECTED")


if __name__ == "__main__":
    main()
