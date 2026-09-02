#!/usr/bin/env python3
"""Mutation gate for the 86R.5C.3-S2 terminal validator."""

from __future__ import annotations

import re

from validate_p4_audio86_pcm_terminal_log import SCENARIOS, validate_terminal


CASES = {
    "reset-full-stop": dict(name="RESET_FULL_STOP", forced=0, error=0,
        finished=1, done=1, pre_o=0, pre_p=0, produced=1933, accepted=1933,
        k=0, p=0, r=0, cut_o=8, cut_p=0, semantic=1933, unappended=13,
        guest=1, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=1, finish_fatal=0, sink_finished=1, success=1,
        controller=3),
    "reset-full-fatal": dict(name="RESET_FULL_FATAL", forced=0, error=86,
        finished=1, done=1, pre_o=0, pre_p=0, produced=1933, accepted=1933,
        k=0, p=0, r=0, cut_o=8, cut_p=0, semantic=1933, unappended=13,
        guest=1, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=1, finish_fatal=0, sink_finished=1, success=1,
        controller=3),
    "reset-full-consumer-fatal": dict(name="RESET_FULL_CONSUMER_FATAL",
        forced=1, error=2, finished=0, done=0, pre_o=8, pre_p=0,
        produced=1920, accepted=0, k=1920, p=0, r=13, cut_o=8, cut_p=0,
        semantic=1933, unappended=13, guest=1, applied=0, ack=0,
        reset_abandoned=1, events=2, finish_calls=0, finish_fatal=0,
        sink_finished=0, success=0, controller=4),
    "partial-stop": dict(name="PARTIAL_STOP", forced=0, error=0,
        finished=1, done=1, pre_o=0, pre_p=0, produced=253, accepted=253,
        k=0, p=0, r=0, cut_o=1, cut_p=13, semantic=253, unappended=0,
        guest=0, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=1, finish_fatal=0, sink_finished=1, success=1,
        controller=3),
    "partial-fatal": dict(name="PARTIAL_FATAL", forced=0, error=86,
        finished=1, done=1, pre_o=0, pre_p=0, produced=253, accepted=253,
        k=0, p=0, r=0, cut_o=1, cut_p=13, semantic=253, unappended=0,
        guest=0, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=1, finish_fatal=0, sink_finished=1, success=1,
        controller=3),
    "partial-consumer-fatal": dict(name="PARTIAL_CONSUMER_FATAL", forced=1,
        error=2, finished=0, done=0, pre_o=1, pre_p=13, produced=253,
        accepted=0, k=240, p=13, r=0, cut_o=1, cut_p=13, semantic=253,
        unappended=0, guest=0, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=0, finish_fatal=0, sink_finished=0, success=0,
        controller=4),
    "post-done-consumer-fatal": dict(name="POST_DONE_CONSUMER_FATAL",
        forced=1, error=2, finished=1, done=1, pre_o=1, pre_p=0,
        produced=2400, accepted=2160, k=240, p=0, r=0, cut_o=1, cut_p=0,
        semantic=2400, unappended=0, guest=0, applied=1, ack=1,
        reset_abandoned=0, events=0, finish_calls=0, finish_fatal=0,
        sink_finished=0, success=0, controller=4),
    "finish-fatal": dict(name="FINISH_FATAL", forced=1, error=2,
        finished=1, done=1, pre_o=0, pre_p=0, produced=2400, accepted=2400,
        k=0, p=0, r=0, cut_o=0, cut_p=0, semantic=2400, unappended=0,
        guest=0, applied=1, ack=1, reset_abandoned=0, events=0,
        finish_calls=1, finish_fatal=1, sink_finished=0, success=0,
        controller=4),
}


def log(scenario: str) -> str:
    c = CASES[scenario]
    aborts = c["forced"]
    return (
        "ESP-ROM:esp32p4\nmain_task: Returned from app_main()\n"
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS\n"
        "P4_NANO_AUDIO86_REAL_GUEST_STATUS=PASS\n5C3_I2S_ACTIVE=NO\n"
        f"P4_AUDIO86_PCM_LIFECYCLE scenario={c['name']} triggered=1 "
        f"forced_abort={c['forced']} forced_before_wake={c['forced']} "
        f"ring_finished={c['finished']} pcm_done={c['done']} worker_quiescent=1 "
        "consumer_ack=1 consumer_quiescent=1 worker_suspended=1 "
        "consumer_suspended=1 worker_deleted_after_suspended=1 "
        "consumer_deleted_after_suspended=1 worker_join_timeout=0 "
        f"consumer_join_timeout=0 sink_abort_calls={aborts} worker_waiting=0 "
        f"pre_cleanup_occupancy={c['pre_o']} pre_cleanup_partial={c['pre_p']} "
        "final_occupancy=0 final_partial=0 "
        f"produced_frames={c['produced']} consumed_frames={c['accepted']} "
        f"abandoned_published={c['k']} abandoned_partial={c['p']} "
        f"abandoned_rendered={c['r']} first_error={c['error']} result=PASS\n"
        f"P4_AUDIO86_PCM_S2_CUTPOINT scenario={c['name']} occupancy={c['cut_o']} "
        f"partial={c['cut_p']} semantic_rendered={c['semantic']} "
        f"unappended={c['unappended']} pcm_done={c['done']} "
        f"ring_finished={c['finished']} sink_finished={c['sink_finished']} "
        f"terminal_success={c['success']}\n"
        f"P4_AUDIO86_PCM_ACCOUNTING semantic_rendered={c['semantic']} "
        f"accepted={c['accepted']} abandoned_published={c['k']} "
        f"abandoned_partial={c['p']} abandoned_rendered={c['r']} "
        f"accounted={c['semantic']} semantic_bytes={c['semantic'] * 4} "
        f"accounted_bytes={c['semantic'] * 4} identity=1\n"
        f"P4_AUDIO86_PCM_RESET_TERMINAL guest_linearized={c['guest']} "
        f"worker_applied={c['applied']} ack_published={c['ack']} "
        f"abandoned={c['reset_abandoned']} event_before_cleanup={c['events']} "
        "horizon_before_cleanup=0 transport_after_cleanup=0\n"
        f"P4_AUDIO86_PCM_FINISH_TERMINAL calls={c['finish_calls']} "
        f"fatal={c['finish_fatal']} sink_finished={c['sink_finished']} "
        f"success_ack={c['success']} terminal_ack=1 forced_abort={c['forced']} "
        f"abort_calls={aborts} controller_state={c['controller']}\n"
    )


def set_field(text: str, prefix: str, key: str, value: str) -> str:
    pattern = rf"(?m)^({re.escape(prefix)}[^\r\n]*\b{re.escape(key)}=)[^ ]+"
    changed, count = re.subn(pattern, rf"\g<1>{value}", text, count=1)
    assert count == 1, (prefix, key)
    return changed


def main() -> None:
    goods = {scenario: log(scenario) for scenario in SCENARIOS}
    for scenario, text in goods.items():
        validate_terminal(text, scenario)

    mutations = [
        ("reset_applies_early", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_RESET_TERMINAL ", "worker_applied", "1"),
        ("reset_stop_abandons", "reset-full-stop",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_rendered", "13"),
        ("healthy_fatal_forces_abort", "reset-full-fatal",
         "P4_AUDIO86_PCM_LIFECYCLE ", "forced_abort", "1"),
        ("reset_consumer_ack", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_RESET_TERMINAL ", "ack_published", "1"),
        ("reset_event_residual_missing", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_RESET_TERMINAL ", "event_before_cleanup", "0"),
        ("reset_horizon_residual", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_RESET_TERMINAL ", "horizon_before_cleanup", "1"),
        ("reset_transport_residual", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_RESET_TERMINAL ", "transport_after_cleanup", "1"),
        ("partial_stop_abandons", "partial-stop",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_partial", "13"),
        ("partial_fatal_abandons", "partial-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_partial", "13"),
        ("partial_p_accepted", "partial-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "accepted", "13"),
        ("partial_p_twice", "partial-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_partial", "26"),
        ("k_p_overlap", "partial-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_published", "253"),
        ("p_r_overlap", "partial-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "abandoned_rendered", "13"),
        ("r_omitted", "reset-full-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "semantic_rendered", "1920"),
        ("post_done_false_eos", "post-done-consumer-fatal",
         "P4_AUDIO86_PCM_S2_CUTPOINT ", "sink_finished", "1"),
        ("post_done_k_consumed", "post-done-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "accepted", "2400"),
        ("finish_success_ack", "finish-fatal",
         "P4_AUDIO86_PCM_FINISH_TERMINAL ", "success_ack", "1"),
        ("finish_reports_finished", "finish-fatal",
         "P4_AUDIO86_PCM_FINISH_TERMINAL ", "sink_finished", "1"),
        ("finish_overwrites_error", "finish-fatal",
         "P4_AUDIO86_PCM_LIFECYCLE ", "first_error", "86"),
        ("total_identity_mismatch", "partial-consumer-fatal",
         "P4_AUDIO86_PCM_ACCOUNTING ", "accounted_bytes", "1008"),
    ]
    rejected = 0
    for name, scenario, prefix, key, value in mutations:
        try:
            validate_terminal(set_field(goods[scenario], prefix, key, value),
                              scenario)
        except SystemExit:
            rejected += 1
            print(f"5C3S2_VALIDATOR_MUTATION name={name} result=REJECTED")
        else:
            raise SystemExit(f"ERROR: mutation accepted: {name}")
    print("5C3S2_VALIDATOR=PASS")
    print(f"5C3S2_VALIDATOR_MUTATIONS={rejected}_ALL_REJECTED")


if __name__ == "__main__":
    main()
