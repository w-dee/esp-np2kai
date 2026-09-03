#!/usr/bin/env python3
"""Validate the F2 non-physical sustained real-guest ESP-EMU evidence."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def values(text: str, key: str) -> list[str]:
    return re.findall(rf"(?m)^{re.escape(key)}=([^\r\n]+)$", text)


def one(text: str, key: str) -> str:
    found = values(text, key)
    require(len(found) == 1, f"{key} must occur exactly once")
    return found[0]


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


def golden_digest(golden: dict[str, str], source: str) -> tuple[str, str, str]:
    return (
        golden[f"{source}_SERIALIZED_BYTES"],
        golden[f"{source}_CRC32"],
        golden[f"{source}_SHA256"],
    )


def runtime_digest(text: str, name: str) -> tuple[str, str, str]:
    return (
        one(text, f"{name}_SERIALIZED_BYTES"),
        one(text, f"{name}_CRC32"),
        one(text, f"{name}_SHA256"),
    )


def validate(log: Path, golden_path: Path) -> None:
    text = log.read_text(encoding="utf-8", errors="replace").replace("\r", "")
    document = json.loads(golden_path.read_text(encoding="utf-8"))
    golden: dict[str, str] = document["values"]

    require(one(text, "WORKLOAD_ID") == golden["WORKLOAD_ID"], "workload identity")
    require(one(text, "SEMANTIC_DURATION_MS") == golden["SEMANTIC_DURATION_MS"],
            "semantic duration")
    require(one(text, "SUSTAINED_Q240_UNITS") == golden["SUSTAINED_Q240_UNITS"],
            "q240 count")
    require(one(text, "P4_AUDIO86_REAL_GUEST_RESULT") == "PASS", "terminal result")

    fixture = fields(text, "P4_AUDIO86_REAL_GUEST_FIXTURE")
    require(fixture == {
        "bytes": golden["SUSTAINED_GUEST_PROGRAM_SERIALIZED_BYTES"],
        "crc32": golden["SUSTAINED_GUEST_PROGRAM_CRC32"],
    }, "guest program identity")

    mappings = (
        ("GUEST_IO", "GUEST_IO", "GUEST_IO_SEMANTIC_COUNT"),
        ("AUDIO_EVENTS", "AUDIO_EVENTS", "AUDIO_EVENTS_SEMANTIC_COUNT"),
        ("PCM86_DATA_RUNS", "PCM86_DATA_RUNS", "PCM86_DATA_RUNS_SEMANTIC_COUNT"),
        ("TIMER_PIC", "TIMER_PIC", "TIMER_PIC_SEMANTIC_COUNT"),
        ("WORKER_APPLY_TRACE", "WORKER_APPLY_TRACE",
         "WORKER_APPLY_TRACE_SEMANTIC_COUNT"),
        ("FINAL_G_STATE", "FINAL_G_STATE", "FINAL_G_STATE_SEMANTIC_COUNT"),
    )
    for runtime_name, golden_name, count_name in mappings:
        require(runtime_digest(text, runtime_name) == golden_digest(golden, golden_name),
                f"{runtime_name} digest")
        if runtime_name != "FINAL_G_STATE":
            require(one(text, f"{runtime_name}_RECORDS") == golden[count_name],
                    f"{runtime_name} count")

    pcm_golden = (
        golden["FULL_REPLAY_PCM_BYTES"],
        golden["FULL_REPLAY_PCM_CRC32"],
        golden["FULL_REPLAY_PCM_SHA256"],
    )
    require(runtime_digest(text, "FULL_PCM") == pcm_golden, "generated PCM digest")
    require(runtime_digest(text, "ACCEPTED_PCM") == pcm_golden,
            "accepted-once PCM digest")
    require(one(text, "FULL_PCM_FRAMES") == golden["FULL_REPLAY_PCM_FRAMES"],
            "generated PCM frames")
    require(one(text, "ACCEPTED_PCM_FRAMES") == golden["FULL_REPLAY_PCM_FRAMES"],
            "accepted PCM frames")

    reset = fields(text, "P4_AUDIO86_SUSTAINED_RESET")
    require(reset == {
        "frame": golden["FINAL_EVENT_FRAME"],
        "sequence": golden["RESET_SEQUENCE"],
        "ordinal": "1",
        "ring_next_frame": golden["FINAL_EVENT_FRAME"],
        "applied_after_ring": "1",
        "ack_after_apply": "1",
    }, "reset ordering snapshot")
    require(one(text, "PRE_RESET_PCM_FRAMES") == golden["FINAL_EVENT_FRAME"],
            "pre-reset frames")
    require(one(text, "PRE_RESET_PCM_SERIALIZED_BYTES") ==
            str(int(golden["FINAL_EVENT_FRAME"]) * 4), "pre-reset bytes")

    slot = fields(text, "P4_AUDIO86_SUSTAINED_SLOT")
    require(slot["first_sequence"] == "0" and slot["first_offset"] == "0" and
            slot["final_sequence"] == "399" and slot["final_offset"] == "95760" and
            slot["storage"] == "BOUNDED", "bounded slot fingerprints")
    require(re.fullmatch(r"[0-9a-f]{8}", slot["first_crc32"]) is not None and
            re.fullmatch(r"[0-9a-f]{8}", slot["final_crc32"]) is not None,
            "slot fingerprints")

    trace = fields(text, "P4_AUDIO86_SUSTAINED_TRACE")
    require(trace == {
        "io": "246/128", "events": "18/64", "runs": "1/8",
        "timers": "20/64", "applied": "19/32",
        "model": "FIRST_HALF_LAST_HALF_ALL_DIGESTED",
    }, "trace bounded-window model")

    residual = fields(text, "P4_AUDIO86_REAL_GUEST_RESIDUAL")
    require(residual["events"] == "0" and residual["bytes"] == "0" and
            residual["horizon"] == "0" and residual["first_error"] == "0",
            "terminal residuals")
    lifecycle = fields(text, "P4_AUDIO86_PCM_LIFECYCLE")
    for key in ("final_occupancy", "final_partial", "abandoned_published",
                "abandoned_partial", "abandoned_rendered", "first_error"):
        require(lifecycle[key] == "0", f"lifecycle {key}")
    require(lifecycle["produced_frames"] == golden["FULL_REPLAY_PCM_FRAMES"] and
            lifecycle["consumed_frames"] == golden["FULL_REPLAY_PCM_FRAMES"] and
            lifecycle["result"] == "PASS", "lifecycle frame conservation")

    timing = fields(text, "P4_AUDIO86_SUSTAINED_TIMING")
    start = int(timing["stream_started_ms"])
    end = int(timing["drain_completed_ms"])
    require(end >= start and int(timing["stream_wall_ms"]) == end - start,
            "stream monotonic timing")
    require(timing["progress_bound_ms"] == "40" and
            timing["authority"] == "HOST_ONLY", "realtime authority")
    memory = fields(text, "P4_AUDIO86_SUSTAINED_MEMORY")
    require(memory["duration_dependent_pcm_bytes"] == "0" and
            int(memory["evidence_fixed_bytes"]) > 0 and int(memory["ring_bytes"]) > 0,
            "fixed-memory evidence")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument(
        "--golden", type=Path,
        default=Path("host/probe/audio86_guest_sustained_2s_golden.json"),
    )
    args = parser.parse_args()
    try:
        validate(args.log, args.golden)
    except (KeyError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"SUSTAINED_ESP_EMU_SEMANTIC_GATE=FAIL: {error}")
        return 1
    print("SUSTAINED_ESP_EMU_SEMANTIC_GATE=PASS")
    print("SUSTAINED_ESP_EMU_F1_GOLDEN_IDENTITY=PASS")
    print("F2_PHYSICAL_EXEC_PASS=NOT_CLAIMED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
