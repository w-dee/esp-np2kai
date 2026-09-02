#!/usr/bin/env python3
"""Fail-closed validator for the 86R.5C.3-S2 PCM terminal profiles."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from validate_p4_audio86_real_guest_log import FATAL_PATTERNS, one, require


SCENARIOS = {
    "reset-full-stop": ("RESET_FULL_STOP", "0", "0"),
    "reset-full-fatal": ("RESET_FULL_FATAL", "0", "86"),
    "reset-full-consumer-fatal": ("RESET_FULL_CONSUMER_FATAL", "1", "2"),
    "partial-stop": ("PARTIAL_STOP", "0", "0"),
    "partial-fatal": ("PARTIAL_FATAL", "0", "86"),
    "partial-consumer-fatal": ("PARTIAL_CONSUMER_FATAL", "1", "2"),
    "post-done-consumer-fatal": ("POST_DONE_CONSUMER_FATAL", "1", "2"),
    "finish-fatal": ("FINISH_FATAL", "1", "2"),
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


def exact(actual: dict[str, str], expected: dict[str, str], label: str) -> None:
    require(actual == expected, f"{label}: field/value mismatch")


def validate_terminal(text: str, scenario: str) -> None:
    require(scenario in SCENARIOS, "unknown S2 scenario")
    for pattern in FATAL_PATTERNS:
        require(pattern.search(text) is None,
                f"raw fatal signature: {pattern.pattern}")
    require("main_task: Returned from app_main()" in text,
            "app_main completion missing")
    require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS",
            "guest result")
    require(one(text, "P4_NANO_AUDIO86_REAL_GUEST_STATUS") == "PASS",
            "main status")
    require(one(text, "5C3_I2S_ACTIVE") == "NO", "scope escaped to I2S")

    name, forced, first_error = SCENARIOS[scenario]
    lifecycle = fields(text, "P4_AUDIO86_PCM_LIFECYCLE ")
    lifecycle_keys = {
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
    require(set(lifecycle) == lifecycle_keys, "lifecycle fields mismatch")
    require(lifecycle["scenario"] == name and lifecycle["triggered"] == "1",
            "S2 rendezvous not reached")
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
    require(lifecycle["forced_before_wake"] == forced,
            "forced-abort publication order")
    require(lifecycle["sink_abort_calls"] == forced,
            "abort policy/count mismatch")
    require(lifecycle["result"] == "PASS", "lifecycle result")

    cut = fields(text, "P4_AUDIO86_PCM_S2_CUTPOINT ")
    accounting = fields(text, "P4_AUDIO86_PCM_ACCOUNTING ")
    reset = fields(text, "P4_AUDIO86_PCM_RESET_TERMINAL ")
    finish = fields(text, "P4_AUDIO86_PCM_FINISH_TERMINAL ")
    require(cut["scenario"] == name, "cutpoint scenario")
    require(cut["pcm_done"] == lifecycle["pcm_done"] and
            cut["ring_finished"] == lifecycle["ring_finished"] and
            cut["sink_finished"] == finish["sink_finished"] and
            cut["terminal_success"] == finish["success_ack"],
            "cutpoint/terminal marker agreement")
    require(accounting["identity"] == "1", "reported accounting identity")
    semantic = int(accounting["semantic_rendered"])
    accepted = int(accounting["accepted"])
    abandoned_published = int(accounting["abandoned_published"])
    abandoned_partial = int(accounting["abandoned_partial"])
    abandoned_rendered = int(accounting["abandoned_rendered"])
    require(semantic == accepted + abandoned_published +
            abandoned_partial + abandoned_rendered,
            "semantic=A+K+P+R")
    require(int(accounting["accounted"]) == semantic,
            "reported accounted-frame total")
    require(int(accounting["semantic_bytes"]) == semantic * 4 and
            int(accounting["accounted_bytes"]) == semantic * 4,
            "PCM byte identity")
    require(int(lifecycle["consumed_frames"]) == accepted and
            int(lifecycle["abandoned_published"]) == abandoned_published and
            int(lifecycle["abandoned_partial"]) == abandoned_partial and
            int(lifecycle["abandoned_rendered"]) == abandoned_rendered,
            "lifecycle/accounting counter agreement")

    common_success = {
        "ring_finished": "1", "pcm_done": "1", "pre_cleanup_occupancy": "0",
        "pre_cleanup_partial": "0", "abandoned_published": "0",
        "abandoned_partial": "0", "abandoned_rendered": "0",
    }
    if scenario.startswith("reset-full-"):
        exact({k: cut[k] for k in ("occupancy", "partial", "semantic_rendered",
                                   "unappended")},
              {"occupancy": "8", "partial": "0", "semantic_rendered": "1933",
               "unappended": "13"}, "RESET full-ring cutpoint")
        if scenario == "reset-full-consumer-fatal":
            exact({k: lifecycle[k] for k in ("ring_finished", "pcm_done",
                                              "pre_cleanup_occupancy",
                                              "pre_cleanup_partial",
                                              "produced_frames",
                                              "consumed_frames",
                                              "abandoned_published",
                                              "abandoned_partial",
                                              "abandoned_rendered")},
                  {"ring_finished": "0", "pcm_done": "0",
                   "pre_cleanup_occupancy": "8", "pre_cleanup_partial": "0",
                   "produced_frames": "1920", "consumed_frames": "0",
                   "abandoned_published": "1920", "abandoned_partial": "0",
                   "abandoned_rendered": "13"}, "RESET consumer accounting")
            exact(reset, {"guest_linearized": "1", "worker_applied": "0",
                          "ack_published": "0", "abandoned": "1",
                          "event_before_cleanup": "2",
                          "horizon_before_cleanup": "0",
                          "transport_after_cleanup": "0"},
                  "forced RESET residual contract")
        else:
            for key, value in common_success.items():
                require(lifecycle[key] == value, f"healthy RESET: {key}")
            require(lifecycle["produced_frames"] == "1933" and
                    lifecycle["consumed_frames"] == "1933",
                    "healthy RESET drain")
            exact(reset, {"guest_linearized": "1", "worker_applied": "1",
                          "ack_published": "1", "abandoned": "0",
                          "event_before_cleanup": "0",
                          "horizon_before_cleanup": "0",
                          "transport_after_cleanup": "0"},
                  "healthy RESET durability")
    elif scenario.startswith("partial-"):
        exact({k: cut[k] for k in ("occupancy", "partial", "semantic_rendered",
                                   "unappended")},
              {"occupancy": "1", "partial": "13", "semantic_rendered": "253",
               "unappended": "0"}, "partial cutpoint")
        if scenario == "partial-consumer-fatal":
            require(abandoned_partial == 13 and abandoned_published == 240 and
                    accepted == 0 and abandoned_rendered == 0,
                    "partial forced-abort disjoint accounting")
            require(lifecycle["pre_cleanup_occupancy"] == "1" and
                    lifecycle["pre_cleanup_partial"] == "13" and
                    lifecycle["ring_finished"] == "0" and
                    lifecycle["pcm_done"] == "0",
                    "partial remains unpublished before cleanup")
        else:
            for key, value in common_success.items():
                require(lifecycle[key] == value, f"healthy partial: {key}")
            require(accepted == semantic == 253 and
                    lifecycle["produced_frames"] == "253",
                    "healthy partial publication/drain")
    elif scenario == "post-done-consumer-fatal":
        require(cut["occupancy"] == "1" and cut["partial"] == "0" and
                cut["semantic_rendered"] == "2400" and
                lifecycle["ring_finished"] == "1" and
                lifecycle["pcm_done"] == "1" and
                lifecycle["pre_cleanup_occupancy"] == "1" and
                accepted == 2160 and abandoned_published == 240 and
                abandoned_partial == abandoned_rendered == 0,
                "post-done residual failure")
    else:
        require(cut["occupancy"] == "0" and cut["partial"] == "0" and
                cut["semantic_rendered"] == "2400" and
                lifecycle["ring_finished"] == "1" and
                lifecycle["pcm_done"] == "1" and accepted == semantic == 2400 and
                abandoned_published == abandoned_partial == abandoned_rendered == 0,
                "finish-FATAL accepted PCM accounting")

    if scenario == "finish-fatal":
        exact(finish, {"calls": "1", "fatal": "1", "sink_finished": "0",
                       "success_ack": "0", "terminal_ack": "1",
                       "forced_abort": "1", "abort_calls": "1",
                       "controller_state": "4"}, "finish-FATAL terminalization")
    elif forced == "1":
        require(finish["sink_finished"] == "0" and
                finish["success_ack"] == "0" and
                finish["terminal_ack"] == "1" and
                finish["forced_abort"] == "1" and
                finish["abort_calls"] == "1" and
                finish["controller_state"] == "4",
                "forced terminalization")
    else:
        require(finish["calls"] == "1" and finish["fatal"] == "0" and
                finish["sink_finished"] == "1" and
                finish["success_ack"] == "1" and
                finish["terminal_ack"] == "1" and
                finish["forced_abort"] == "0" and
                finish["abort_calls"] == "0" and
                finish["controller_state"] == "3",
                "healthy terminalization")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--scenario", required=True, choices=tuple(SCENARIOS))
    args = parser.parse_args()
    validate_terminal(args.log.read_text(errors="replace"), args.scenario)
    print("5C3S2_VALIDATOR=PASS")


if __name__ == "__main__":
    main()
