#!/usr/bin/env python3
"""Fail-closed validator for the 86R.5C.3-S1 PCM RETRY profiles."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from validate_p4_audio86_real_guest_log import FATAL_PATTERNS, one, require


SCENARIOS = {
    "retry-stop": ("RETRY_STOP", "0", "0", True),
    "retry-fatal": ("RETRY_FATAL", "0", "86", True),
    "retry-primary-first": ("RETRY_PRIMARY_FIRST", "1", "86", False),
    "retry-consumer-first": ("RETRY_CONSUMER_FIRST", "1", "2", False),
}


def fields(text: str, prefix: str) -> dict[str, str]:
    matches = re.findall(rf"(?m)^{re.escape(prefix)}([^\r\n]+)$", text)
    require(len(matches) == 1, f"{prefix}: expected one marker")
    result: dict[str, str] = {}
    for item in matches[0].split():
        require("=" in item, f"{prefix}: malformed field")
        key, value = item.split("=", 1)
        require(key not in result, f"{prefix}: duplicate field {key}")
        result[key] = value
    return result


def validate_retry(text: str, scenario: str) -> None:
    require(scenario in SCENARIOS, "unknown RETRY scenario")
    for pattern in FATAL_PATTERNS:
        require(pattern.search(text) is None,
                f"raw fatal signature: {pattern.pattern}")
    require("main_task: Returned from app_main()" in text,
            "app_main completion missing")
    require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS",
            "guest result")
    require(one(text, "P4_NANO_AUDIO86_REAL_GUEST_STATUS") == "PASS",
            "main status")

    name, forced, first_error, healthy = SCENARIOS[scenario]
    lifecycle = fields(text, "P4_AUDIO86_PCM_LIFECYCLE ")
    required_lifecycle = {
        "scenario", "triggered", "forced_abort", "forced_before_wake",
        "ring_finished", "pcm_done", "worker_quiescent", "consumer_ack",
        "consumer_quiescent", "worker_suspended", "consumer_suspended",
        "worker_deleted_after_suspended", "consumer_deleted_after_suspended",
        "worker_join_timeout", "consumer_join_timeout", "sink_abort_calls",
        "worker_waiting", "pre_cleanup_occupancy", "pre_cleanup_partial",
        "final_occupancy", "final_partial", "produced_frames",
        "consumed_frames", "abandoned_published", "abandoned_partial",
        "abandoned_rendered", "first_error", "result",
    }
    require(set(lifecycle) == required_lifecycle,
            "PCM lifecycle fields mismatch")
    require(lifecycle["scenario"] == name and lifecycle["triggered"] == "1",
            "RETRY rendezvous not reached")
    require(lifecycle["forced_abort"] == forced and
            lifecycle["first_error"] == first_error,
            "terminal identity mismatch")
    for key in ("worker_quiescent", "consumer_ack", "consumer_quiescent",
                "worker_suspended", "consumer_suspended",
                "worker_deleted_after_suspended",
                "consumer_deleted_after_suspended"):
        require(lifecycle[key] == "1", f"terminal closure: {key}")
    for key in ("worker_join_timeout", "consumer_join_timeout",
                "worker_waiting", "final_occupancy", "final_partial"):
        require(lifecycle[key] == "0", f"terminal residual: {key}")
    require(lifecycle["result"] == "PASS", "lifecycle result")

    retry = fields(text, "P4_AUDIO86_PCM_RETRY ")
    required_retry = {
        "scenario", "attempts", "wakes", "resubmits", "identity",
        "tail_held", "accepted_held", "full_occupancy", "worker_resumed",
        "permission_before_wake", "wait_skipped_ready",
        "done_only_after_empty", "tail_before", "tail_after",
        "accepted_frames_before", "accepted_frames_after",
        "accepted_bytes_before", "accepted_bytes_after", "sequence",
        "frame_offset", "valid_frames", "flags", "crc32", "forced_abort",
        "first_error", "result",
    }
    require(set(retry) == required_retry, "PCM RETRY fields mismatch")
    require(retry["scenario"] == name and retry["attempts"] == "2" and
            retry["resubmits"] == "1", "same-slot resubmit count")
    require(retry["identity"] == "1" and retry["tail_held"] == "1" and
            retry["accepted_held"] == "1", "RETRY ownership/identity")
    require(retry["full_occupancy"] == "8", "full-ring rendezvous")
    require(retry["permission_before_wake"] == "1",
            "permission publication order")
    require(int(retry["wakes"]) > 0 or retry["wait_skipped_ready"] == "1",
            "level-predicate retry progress")
    require(retry["sequence"] == "0" and retry["frame_offset"] == "0" and
            retry["valid_frames"] == "240" and retry["flags"] == "0" and
            retry["crc32"] == "d065c969", "RETRY slot semantic identity")
    require(retry["forced_abort"] == forced and
            retry["first_error"] == first_error and retry["result"] == "PASS",
            "RETRY terminal result")
    require(retry["accepted_frames_before"] == "0" and
            retry["accepted_bytes_before"] == "0",
            "RETRY changed accepted totals")

    produced = int(lifecycle["produced_frames"])
    consumed = int(lifecycle["consumed_frames"])
    abandoned = (int(lifecycle["abandoned_published"]) +
                 int(lifecycle["abandoned_partial"]))
    if healthy:
        require(lifecycle["forced_before_wake"] == "0" and
                lifecycle["sink_abort_calls"] == "0" and
                lifecycle["ring_finished"] == "1" and
                lifecycle["pcm_done"] == "1", "healthy lifecycle flags")
        require(produced == consumed == 2400 and abandoned == 0 and
                lifecycle["abandoned_rendered"] == "0",
                "healthy drain/accounting")
        require(retry["worker_resumed"] == "1" and
                retry["done_only_after_empty"] == "1",
                "worker/EOS progress")
        require(int(retry["tail_after"]) == int(retry["tail_before"]) + 1,
                "RETRY ACCEPT did not advance tail exactly once")
        require(retry["accepted_frames_after"] == "2400" and
                retry["accepted_bytes_after"] == "9600",
                "healthy accepted totals")
        post_done = fields(text, "P4_AUDIO86_PCM_POST_DONE_RETRY ")
        require(post_done == {
            "scenario": name, "attempts": "2", "resubmits": "1",
            "identity": "1", "tail_held": "1", "accepted_held": "1",
            "observed_occupancy": "1", "not_eos": "1",
            "permission_before_wake": "1", "tail_before": "9",
            "tail_after": "10", "accepted_frames_before": "2160",
            "accepted_bytes_before": "8640", "crc32": "49d839a8",
            "result": "PASS",
        }, "post-PCM-done RETRY/EOS evidence")
    else:
        require("P4_AUDIO86_PCM_POST_DONE_RETRY " not in text,
                "forced profile emitted healthy post-done marker")
        require(lifecycle["forced_before_wake"] == "1" and
                lifecycle["sink_abort_calls"] == "1" and
                lifecycle["ring_finished"] == "0" and
                lifecycle["pcm_done"] == "0", "forced-abort lifecycle flags")
        require(lifecycle["pre_cleanup_occupancy"] == "8" and
                produced == consumed + abandoned and consumed == 0 and
                abandoned == 1920, "forced-abort ownership/accounting")
        require(retry["worker_resumed"] == "0" and
                retry["done_only_after_empty"] == "0" and
                retry["tail_after"] == retry["tail_before"],
                "forced RETRY tail/progress")
        require(retry["accepted_frames_after"] == "0" and
                retry["accepted_bytes_after"] == "0",
                "forced RETRY counted as accepted")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--scenario", required=True, choices=tuple(SCENARIOS))
    args = parser.parse_args()
    validate_retry(args.log.read_text(errors="replace"), args.scenario)
    print("5C3S1_VALIDATOR=PASS")


if __name__ == "__main__":
    main()
