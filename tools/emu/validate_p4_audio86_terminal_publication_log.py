#!/usr/bin/env python3
"""Validate the R11 production-path terminal RESET publication seam."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


PREFIX = "P4_AUDIO86_TERMINAL_PUBLICATION_TEST"
ACTUAL_PATH = "HLT_BOARD86_RESET_ADAPTER_BINDING_WORKER_RING_CONTROLLER"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def fields(text: str, prefix: str) -> dict[str, str]:
    lines = re.findall(rf"(?m)^{re.escape(prefix)} ([^\r\n]+)$", text)
    require(len(lines) == 1, f"{prefix} must occur exactly once")
    result: dict[str, str] = {}
    for token in lines[0].split():
        require("=" in token, f"malformed {prefix} token: {token}")
        key, value = token.split("=", 1)
        require(key not in result, f"duplicate {prefix} field: {key}")
        result[key] = value
    return result


def one(text: str, key: str) -> str:
    values = re.findall(rf"(?m)^{re.escape(key)}=([^\r\n]+)$", text)
    require(len(values) == 1, f"{key} must occur exactly once")
    return values[0]


def validate_text(text: str, expected_mode: int) -> None:
    text = text.replace("\r", "")
    record = fields(text, PREFIX)
    required = {
        "mode", "actual_path", "hold_ack", "event_visible",
        "terminal_absent_before_release", "notify_before_event",
        "notify_after_event", "pre_ack_pair", "worker_pair",
        "reset_before_remainder", "retained", "q399_before_continuation",
        "q398_accepted", "q399_visible", "q399_accepted",
        "virtual_gap_ms", "service_horizon_ms", "partial_event",
        "partial_wake", "producer_done", "transport_residual",
        "first_error", "result",
    }
    require(set(record) == required, "terminal publication field set")
    require(record["mode"] == str(expected_mode), "test mode")
    require(record["actual_path"] == ACTUAL_PATH, "actual production path")
    require(record["hold_ack"] == "1", "worker hold rendezvous")
    require(record["event_visible"] == "1", "RESET event release visibility")
    require(record["terminal_absent_before_release"] == "1",
            "forced event/terminal release window")
    require(record["notify_before_event"] == record["notify_after_event"],
            "no final RESET wake before terminal release")
    require(record["producer_done"] == "1", "eventual producer completion")
    require(record["transport_residual"] == "0", "transport closure")
    require(record["result"] == "PASS", "seam verdict")

    if expected_mode == 1:
        for key in (
            "pre_ack_pair", "worker_pair", "reset_before_remainder",
            "retained", "q399_before_continuation", "q398_accepted",
            "q399_visible", "q399_accepted",
        ):
            require(record[key] == "1", key)
        require(record["virtual_gap_ms"] == "5", "q240 virtual period")
        require(record["service_horizon_ms"] == "20",
                "completed-buffer service horizon")
        require(int(record["virtual_gap_ms"]) <
                int(record["service_horizon_ms"]), "deadline model")
        require(record["partial_event"] == "0" and
                record["partial_wake"] == "0", "no failure injection")
        require(record["first_error"] == "0", "successful first error")
        require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS",
                "successful outer runtime")
    else:
        require(expected_mode == 2, "known test mode")
        require(record["pre_ack_pair"] == "0" and
                record["worker_pair"] == "0", "terminal not published")
        require(record["partial_event"] == "1", "published RESET retained")
        require(record["partial_wake"] == "1", "failure wake issued")
        require(record["first_error"] == "1", "failure latched")
        require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "FAIL",
                "no false COMPLETE")
        lifecycle = fields(text, "P4_AUDIO86_PCM_LIFECYCLE")
        require(lifecycle["result"] == "FAIL", "PCM lifecycle failure")
        require(lifecycle["final_occupancy"] == "0" and
                lifecycle["final_partial"] == "0", "PCM ownership closure")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--mode", type=int, choices=(1, 2), required=True)
    args = parser.parse_args()
    try:
        validate_text(args.log.read_text(encoding="utf-8", errors="replace"),
                      args.mode)
    except (OSError, ValueError) as error:
        print(f"R11_TERMINAL_PUBLICATION_VALIDATION=FAIL: {error}")
        return 1
    print(f"R11_TERMINAL_PUBLICATION_MODE_{args.mode}=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
