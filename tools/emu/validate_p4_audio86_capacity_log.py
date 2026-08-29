#!/usr/bin/env python3
"""Machine-check the bounded AUDIO86 P4 capacity log schema."""

from __future__ import annotations

import argparse
import re
import shlex
from pathlib import Path


PCM_SHA = "7f1bc0cdcab519690c0d3580746827199f86dd270868f33ceb01d230e096310e"
CONTROL_SHA = "22fccc625378d2ae4a0715dd94a187a5c04cd5324a42828ac454293f8b1b328d"
SOURCE_SHA = "29783320f21cafc17330b56bc3484e8caf6543044f2f7280dc5703728cad5529"
TRANSPORT_SHA = "b2e50daab772920049b61ee2fec18b2fe46e672147bc67402bf00ee6ed844875"
SHA_FIELDS = ("pcm_sha256", "control_sha256", "source_sha256", "transport_sha256")
FIELD_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*=(?:[^\s]+)$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


def fail(message: str) -> None:
    raise ValueError(message)


def parse(path: Path) -> tuple[dict[str, dict[str, str]], list[str]]:
    records: dict[str, dict[str, str]] = {}
    lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
    for raw in lines:
        if not raw.startswith("AUDIO86_P4_"):
            continue
        tokens = shlex.split(raw)
        if not tokens:
            continue
        head = tokens[0]
        if "=" in head:
            name, value = head.split("=", 1)
            records.setdefault(name, {})["value"] = value
            continue
        fields = records.setdefault(head, {})
        for token in tokens[1:]:
            if not FIELD_RE.fullmatch(token):
                fail(f"malformed field in {head}: {token!r}")
            key, value = token.split("=", 1)
            if key in fields:
                fail(f"duplicate field {head}.{key}")
            fields[key] = value
    return records, lines


def integer(records: dict[str, dict[str, str]], record: str, field: str) -> int:
    try:
        return int(records[record][field], 0)
    except (KeyError, ValueError) as exc:
        fail(f"missing or invalid {record}.{field}")
        raise AssertionError from exc


def require(records: dict[str, dict[str, str]], record: str, *fields: str) -> None:
    if record not in records:
        fail(f"missing {record}")
    for field in fields:
        if field not in records[record]:
            fail(f"missing {record}.{field}")


def validate_smoke(records: dict[str, dict[str, str]]) -> None:
    require(records, "AUDIO86_P4_SMOKE", "profile", "scope")
    if records["AUDIO86_P4_SMOKE"]["profile"] != "P4_NANO_AUDIO86_CAPACITY_PROFILE":
        fail("wrong smoke profile")
    if records["AUDIO86_P4_SMOKE"]["scope"] != "BOOT_SMOKE":
        fail("smoke scope is not BOOT_SMOKE")
    require(records, "AUDIO86_P4_SMOKE_S1", "task_create_failure")
    require(records, "AUDIO86_P4_SMOKE_S2", "worker_wait_peer_error_wake")
    if records["AUDIO86_P4_SMOKE_S1"]["task_create_failure"] != "PASS":
        fail("S1 failed")
    if records["AUDIO86_P4_SMOKE_S2"]["worker_wait_peer_error_wake"] != "PASS":
        fail("S2 failed")
    require(records, "AUDIO86_P4_LIFECYCLE", "terminal")
    if records["AUDIO86_P4_LIFECYCLE"]["terminal"] != "PASS":
        fail("smoke lifecycle did not terminate")
    if records.get("AUDIO86_P4_EMU_SMOKE", {}).get("value") != "PASS":
        fail("missing AUDIO86_P4_EMU_SMOKE=PASS")
    if "AUDIO86_P4_RESULT" in records:
        fail("short smoke must not emit a formal result")


def validate_formal(records: dict[str, dict[str, str]]) -> None:
    require(records, "AUDIO86_P4_CONFIG", "git_sha", "profile", "mode", "cpu_hz",
            "tick_hz", "psram_bytes", "psram_mhz", "rate", "quantum_frames",
            "quantum_us", "quanta")
    config = records["AUDIO86_P4_CONFIG"]
    if config["profile"] != "P4_NANO_AUDIO86_CAPACITY_PROFILE":
        fail("wrong profile")
    if config["mode"] not in {"PACED_FORMAL", "PROFILE", "UNPACED"}:
        fail("unknown mode")
    if not re.fullmatch(r"[0-9a-f]{40}", config["git_sha"]):
        fail("invalid git_sha")
    if integer(records, "AUDIO86_P4_CONFIG", "cpu_hz") != 360000000:
        fail("wrong CPU frequency")
    if integer(records, "AUDIO86_P4_CONFIG", "tick_hz") != 100:
        fail("wrong tick rate")
    if integer(records, "AUDIO86_P4_CONFIG", "psram_mhz") != 200:
        fail("wrong PSRAM frequency")
    if integer(records, "AUDIO86_P4_CONFIG", "rate") != 48000:
        fail("wrong sample rate")
    if integer(records, "AUDIO86_P4_CONFIG", "quantum_frames") != 240:
        fail("wrong quantum geometry")
    if integer(records, "AUDIO86_P4_CONFIG", "quantum_us") != 5000:
        fail("wrong period")
    if integer(records, "AUDIO86_P4_CONFIG", "quanta") != 12000:
        fail("wrong quantum count")

    require(records, "AUDIO86_P4_IDENTITY", "frames", "bytes", "quanta", "pcm_crc32",
            *SHA_FIELDS, "control_events", "control_crc32", "source_crc32",
            "transport_events", "transport_crc32", "pcm86_data_runs", "pcm86_supplied",
            "pcm86_consumed", "pcm86_fifo_min", "pcm86_fifo_max", "pcm86_underrun",
            "peak_abs", "clamped_samples", "fm", "psg", "rhythm", "pcm86",
            "mid_quantum_events")
    identity = records["AUDIO86_P4_IDENTITY"]
    expected = {
        "frames": 2880000, "bytes": 11520000, "quanta": 12000,
        "pcm_crc32": "0x58929f1f", "control_events": 10,
        "control_crc32": "0xafa2dd74", "source_crc32": "0x905d2517",
        "transport_events": 333, "transport_crc32": "0x8fc674d3",
        "pcm86_data_runs": 323, "pcm86_supplied": 10584064,
        "pcm86_consumed": 10575000, "pcm86_fifo_min": 4096,
        "pcm86_fifo_max": 36860, "pcm86_underrun": 0, "peak_abs": 4182,
        "clamped_samples": 0, "fm": 1, "psg": 1, "rhythm": 1, "pcm86": 1,
        "mid_quantum_events": 4,
    }
    for key, value in expected.items():
        actual = integer(records, "AUDIO86_P4_IDENTITY", key) if isinstance(value, int) else identity[key].lower()
        if actual != value:
            fail(f"wrong identity {key}: {actual!r}")
    for key, value in {
        "pcm_sha256": PCM_SHA, "control_sha256": CONTROL_SHA,
        "source_sha256": SOURCE_SHA, "transport_sha256": TRANSPORT_SHA,
    }.items():
        if not SHA_RE.fullmatch(identity[key]) or identity[key] != value:
            fail(f"wrong or malformed {key}")

    require(records, "AUDIO86_P4_TRANSPORT", "event_wait_count", "byte_wait_count",
            "worker_wait_count", "event_high_water", "byte_high_water",
            "final_event_occupancy", "final_byte_occupancy")
    if integer(records, "AUDIO86_P4_TRANSPORT", "event_high_water") > 128:
        fail("event ring capacity exceeded")
    if integer(records, "AUDIO86_P4_TRANSPORT", "byte_high_water") > 65536:
        fail("PCM86 byte ring capacity exceeded")
    if integer(records, "AUDIO86_P4_TRANSPORT", "final_event_occupancy") != 0 or integer(records, "AUDIO86_P4_TRANSPORT", "final_byte_occupancy") != 0:
        fail("residual transport occupancy")

    require(records, "AUDIO86_P4_STACK", "coordinator_hwm", "producer_hwm", "worker_hwm")
    if integer(records, "AUDIO86_P4_STACK", "coordinator_hwm") < 256 or integer(records, "AUDIO86_P4_STACK", "producer_hwm") < 256 or integer(records, "AUDIO86_P4_STACK", "worker_hwm") < 512:
        fail("insufficient stack margin")
    require(records, "AUDIO86_P4_WORKER_TIMING", "sample_count", "p99", "max",
            "absolute_deadline_miss_count", "pacing_backlog_count",
            "paced_input_starvation_count")
    if integer(records, "AUDIO86_P4_WORKER_TIMING", "sample_count") != 12000:
        fail("wrong timing sample count")
    if integer(records, "AUDIO86_P4_WORKER_TIMING", "p99") > 3500 or integer(records, "AUDIO86_P4_WORKER_TIMING", "max") >= 4500:
        fail("formal timing gate failed")
    for field in ("absolute_deadline_miss_count", "pacing_backlog_count", "paced_input_starvation_count"):
        if integer(records, "AUDIO86_P4_WORKER_TIMING", field) != 0:
            fail(f"nonzero {field}")
    require(records, "AUDIO86_P4_EVENT_SPLIT", "count", "q0", "q1", "q2", "q3")
    split = records["AUDIO86_P4_EVENT_SPLIT"]
    if integer(records, "AUDIO86_P4_EVENT_SPLIT", "count") != 4 or [int(split[f]) for f in ("q0", "q1", "q2", "q3")] != [0, 2, 4, 6]:
        fail("wrong event split classification")
    require(records, "AUDIO86_P4_PCM86_REFILL", "count", "non_refill_count")
    if integer(records, "AUDIO86_P4_PCM86_REFILL", "count") != 323 or integer(records, "AUDIO86_P4_PCM86_REFILL", "non_refill_count") != 11677:
        fail("wrong PCM86 refill classification")
    require(records, "AUDIO86_P4_LIFECYCLE", "producer_done", "worker_done", "terminal", "first_error")
    lifecycle = records["AUDIO86_P4_LIFECYCLE"]
    if lifecycle["terminal"] != "PASS" or integer(records, "AUDIO86_P4_LIFECYCLE", "producer_done") != 1 or integer(records, "AUDIO86_P4_LIFECYCLE", "worker_done") != 1 or integer(records, "AUDIO86_P4_LIFECYCLE", "first_error") != 0:
        fail("lifecycle terminal failure")
    if records.get("AUDIO86_P4_RESULT", {}).get("value") != "PASS":
        fail("missing final formal PASS")
    if "AUDIO86_P4_FAILURE" in records:
        fail("explicit failure record")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    records, _ = parse(args.log)
    if args.smoke:
        validate_smoke(records)
    else:
        validate_formal(records)
    print("AUDIO86_P4_LOG_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"AUDIO86_P4_LOG_VALIDATION=FAIL: {exc}")
        raise SystemExit(1)
