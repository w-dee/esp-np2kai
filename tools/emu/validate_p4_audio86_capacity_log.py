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


def parse(path: Path) -> tuple[dict[str, list[dict[str, str]]], dict[str, list[str]]]:
    records: dict[str, list[dict[str, str]]] = {}
    markers: dict[str, list[str]] = {}
    for raw in path.read_text(encoding="utf-8", errors="strict").splitlines():
        if not raw.startswith("AUDIO86_P4_"):
            continue
        tokens = shlex.split(raw)
        if not tokens:
            continue
        head = tokens[0]
        if "=" in head:
            name, value = head.split("=", 1)
            markers.setdefault(name, []).append(value)
            continue
        fields: dict[str, str] = {}
        for token in tokens[1:]:
            if not FIELD_RE.fullmatch(token):
                fail(f"malformed_field_{head}")
            key, value = token.split("=", 1)
            if key in fields:
                fail(f"duplicate_field_{head}_{key}")
            fields[key] = value
        records.setdefault(head, []).append(fields)
    return records, markers


def one(records: dict[str, list[dict[str, str]]], name: str) -> dict[str, str]:
    entries = records.get(name, [])
    if len(entries) != 1:
        fail(f"{name}_count_{len(entries)}")
    return entries[0]


def integer(record: dict[str, str], name: str) -> int:
    try:
        return int(record[name], 0)
    except (KeyError, ValueError):
        fail(f"invalid_{name}")
        raise AssertionError


def require(record: dict[str, str], *fields: str) -> None:
    for field in fields:
        if field not in record:
            fail(f"missing_{field}")


def validate_progress(records: dict[str, list[dict[str, str]]], *, full: bool) -> dict[str, int | str] | None:
    entries = records.get("AUDIO86_P4_PROGRESS", [])
    if not entries:
        if full:
            fail("missing_progress")
        return None
    if len(entries) != 1:
        fail("progress_count")
    progress = entries[0]
    require(progress, "planned_quanta", "planned_frames", "planned_bytes",
            "completed_quanta", "completed_frames", "completed_bytes",
            "canonicalized_quanta", "service_sample_count", "failed_quantum")
    values: dict[str, int | str] = {}
    for field in ("planned_quanta", "planned_frames", "planned_bytes",
                  "completed_quanta", "completed_frames", "completed_bytes",
                  "canonicalized_quanta", "service_sample_count"):
        values[field] = integer(progress, field)
    failed = progress["failed_quantum"]
    if failed == "NONE":
        values["failed_quantum"] = failed
    else:
        try:
            failed_value = int(failed, 0)
        except ValueError:
            fail("invalid_failed_quantum")
        if failed_value < 0:
            fail("invalid_failed_quantum")
        values["failed_quantum"] = failed_value

    if values["planned_quanta"] != 12000 or values["planned_frames"] != 2880000 or values["planned_bytes"] != 11520000:
        fail("wrong_planned_geometry")
    if values["completed_quanta"] > values["canonicalized_quanta"] or values["canonicalized_quanta"] > values["planned_quanta"]:
        fail("progress_order")
    if values["completed_frames"] != values["completed_quanta"] * 240:
        fail("completed_frame_geometry")
    if values["completed_bytes"] != values["completed_frames"] * 4:
        fail("completed_byte_geometry")
    if values["service_sample_count"] != values["canonicalized_quanta"]:
        fail("sample_progress_mismatch")
    if isinstance(values["failed_quantum"], int):
        if values["failed_quantum"] >= values["planned_quanta"]:
            fail("failed_quantum_range")
        if values["failed_quantum"] != values["completed_quanta"]:
            fail("failed_quantum_progress_mismatch")
    elif values["completed_quanta"] != values["planned_quanta"]:
        fail("missing_failed_quantum")
    if full and (values["completed_quanta"] != 12000 or
                 values["canonicalized_quanta"] != 12000 or
                 values["service_sample_count"] != 12000 or
                 values["failed_quantum"] != "NONE"):
        fail("incomplete_full_progress")
    return values


def validate_config(records: dict[str, list[dict[str, str]]]) -> dict[str, str]:
    config = one(records, "AUDIO86_P4_CONFIG")
    require(config, "git_sha", "profile", "mode", "cpu_hz", "tick_hz",
            "psram_bytes", "psram_mhz", "rate", "quantum_frames",
            "quantum_us", "quanta")
    if config["profile"] != "P4_NANO_AUDIO86_CAPACITY_PROFILE":
        fail("wrong_profile")
    if config["mode"] not in {"PACED_FORMAL", "PROFILE", "UNPACED"}:
        fail("unknown_mode")
    if not re.fullmatch(r"[0-9a-f]{40}", config["git_sha"]):
        fail("invalid_git_sha")
    for field, expected in (("cpu_hz", 360000000), ("tick_hz", 100),
                            ("psram_mhz", 200), ("rate", 48000),
                            ("quantum_frames", 240), ("quantum_us", 5000),
                            ("quanta", 12000)):
        if integer(config, field) != expected:
            fail(f"wrong_{field}")
    return config


def validate_pacing_epoch(records: dict[str, list[dict[str, str]]]) -> None:
    entries = records.get("AUDIO86_P4_PACING_EPOCH", [])
    if not entries:
        return
    epoch = one(records, "AUDIO86_P4_PACING_EPOCH")
    require(epoch, "t0_us", "worker_release_us", "q0_service_start_us",
            "first_timer_callback_us")
    t0 = integer(epoch, "t0_us")
    worker_release = integer(epoch, "worker_release_us")
    q0_start = integer(epoch, "q0_service_start_us")
    first_callback = integer(epoch, "first_timer_callback_us")
    if min(t0, worker_release, q0_start, first_callback) < 0:
        fail("negative_pacing_epoch")
    if worker_release < t0 or q0_start < t0:
        fail("pacing_epoch_order")


def validate_smoke(records: dict[str, list[dict[str, str]]],
                   markers: dict[str, list[str]]) -> None:
    smoke = one(records, "AUDIO86_P4_SMOKE")
    require(smoke, "profile", "scope")
    if smoke["profile"] != "P4_NANO_AUDIO86_CAPACITY_PROFILE" or smoke["scope"] != "BOOT_SMOKE":
        fail("wrong_smoke_profile")
    if one(records, "AUDIO86_P4_SMOKE_S1").get("task_create_failure") != "PASS":
        fail("smoke_s1")
    if one(records, "AUDIO86_P4_SMOKE_S2").get("worker_wait_peer_error_wake") != "PASS":
        fail("smoke_s2")
    if one(records, "AUDIO86_P4_LIFECYCLE").get("terminal") != "PASS":
        fail("smoke_lifecycle")
    if markers.get("AUDIO86_P4_EMU_SMOKE") != ["PASS"]:
        fail("smoke_terminal")
    if "AUDIO86_P4_RESULT" in markers:
        fail("smoke_formal_result")


def validate_full_pass(records: dict[str, list[dict[str, str]]]) -> None:
    validate_config(records)
    validate_pacing_epoch(records)
    progress = validate_progress(records, full=True)
    assert progress is not None
    identity = one(records, "AUDIO86_P4_IDENTITY")
    require(identity, "frames", "bytes", "quanta", "pcm_crc32", *SHA_FIELDS,
            "control_events", "control_crc32", "source_crc32",
            "transport_events", "transport_crc32", "pcm86_data_runs",
            "pcm86_supplied", "pcm86_consumed", "pcm86_fifo_min",
            "pcm86_fifo_max", "pcm86_underrun", "peak_abs", "clamped_samples",
            "fm", "psg", "rhythm", "pcm86", "mid_quantum_events")
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
        actual = integer(identity, key) if isinstance(value, int) else identity[key].lower()
        if actual != value:
            fail(f"wrong_identity_{key}")
    for key, value in {"pcm_sha256": PCM_SHA, "control_sha256": CONTROL_SHA,
                       "source_sha256": SOURCE_SHA, "transport_sha256": TRANSPORT_SHA}.items():
        if not SHA_RE.fullmatch(identity[key]) or identity[key] != value:
            fail(f"wrong_or_malformed_{key}")

    transport = one(records, "AUDIO86_P4_TRANSPORT")
    require(transport, "event_wait_count", "byte_wait_count", "worker_wait_count",
            "event_high_water", "byte_high_water", "final_event_occupancy",
            "final_byte_occupancy")
    if integer(transport, "event_high_water") > 128 or integer(transport, "byte_high_water") > 65536:
        fail("transport_capacity")
    if integer(transport, "final_event_occupancy") != 0 or integer(transport, "final_byte_occupancy") != 0:
        fail("transport_residual")

    stack = one(records, "AUDIO86_P4_STACK")
    require(stack, "coordinator_hwm", "producer_hwm", "worker_hwm")
    if integer(stack, "coordinator_hwm") < 256 or integer(stack, "producer_hwm") < 256 or integer(stack, "worker_hwm") < 512:
        fail("stack_margin")
    timing = one(records, "AUDIO86_P4_WORKER_TIMING")
    require(timing, "sample_count", "p99", "max", "absolute_deadline_miss_count",
            "pacing_backlog_count", "paced_input_starvation_count")
    if integer(timing, "sample_count") != 12000 or integer(timing, "p99") > 3500 or integer(timing, "max") >= 4500:
        fail("timing_gate")
    for field in ("absolute_deadline_miss_count", "pacing_backlog_count", "paced_input_starvation_count"):
        if integer(timing, field) != 0:
            fail(f"nonzero_{field}")
    split = one(records, "AUDIO86_P4_EVENT_SPLIT")
    require(split, "count", "q0", "q1", "q2", "q3")
    if integer(split, "count") != 4 or [integer(split, f) for f in ("q0", "q1", "q2", "q3")] != [0, 2, 4, 6]:
        fail("event_split")
    refill = one(records, "AUDIO86_P4_PCM86_REFILL")
    require(refill, "count", "non_refill_count")
    if integer(refill, "count") != 323 or integer(refill, "non_refill_count") != 11677:
        fail("refill_classification")
    if "planned_refill_quanta" in refill and integer(refill, "planned_refill_quanta") != 323:
        fail("planned_refill_classification")
    if "planned_non_refill_quanta" in refill and integer(refill, "planned_non_refill_quanta") != 11677:
        fail("planned_refill_classification")
    if integer(timing, "sample_count") != integer(refill, "count") + integer(refill, "non_refill_count"):
        fail("refill_sample_mismatch")
    lifecycle = one(records, "AUDIO86_P4_LIFECYCLE")
    require(lifecycle, "producer_done", "worker_done", "terminal", "first_error")
    if lifecycle["terminal"] != "PASS" or integer(lifecycle, "producer_done") != 1 or integer(lifecycle, "worker_done") != 1 or integer(lifecycle, "first_error") != 0:
        fail("lifecycle")
    if records.get("AUDIO86_P4_FAILURE"):
        fail("unexpected_failure")
    for allocation in records.get("AUDIO86_P4_ALLOC", []):
        if allocation.get("result") == "FAIL":
            fail("failed_allocation_in_pass")


def validate_target_fail(records: dict[str, list[dict[str, str]]]) -> str:
    failure = one(records, "AUDIO86_P4_FAILURE")
    failure_class = failure.get("first_error")
    if failure_class is None:
        fail("missing_failure_class")
    validate_config(records)
    validate_pacing_epoch(records)
    allocations = records.get("AUDIO86_P4_ALLOC", [])
    if failure_class == "CONFIGURATION":
        if allocations or records.get("AUDIO86_P4_MEMORY_PREALLOC"):
            fail("configuration_allocation_evidence")
        return failure_class
    runtime_class = failure_class.isdigit() and 1 <= int(failure_class) <= 20
    if failure_class not in {"ALLOCATION", "SEMAPHORE", "WORKER_TASK",
                             "PRODUCER_TASK", "TIMER"} and not runtime_class:
        fail("unknown_failure_class")
    prealloc = one(records, "AUDIO86_P4_MEMORY_PREALLOC")
    require(prealloc, "requested_context_bytes", "requested_owned_bytes",
            "internal_free_before", "internal_largest_before")
    if not allocations:
        fail("missing_allocations")
    for allocation in allocations:
        require(allocation, "seq", "name", "api", "requested_bytes", "managed_bytes",
                "caps", "free_before", "largest_before", "result", "free_after",
                "largest_after")
    failed = [a for a in allocations if a["result"] == "FAIL"]
    if runtime_class:
        if failed:
            fail("runtime_failure_with_failed_allocation")
        progress = validate_progress(records, full=False)
        if progress is not None:
            timing_entries = records.get("AUDIO86_P4_WORKER_TIMING", [])
            if timing_entries:
                timing = one(records, "AUDIO86_P4_WORKER_TIMING")
                require(timing, "sample_count")
                if integer(timing, "sample_count") != progress["service_sample_count"]:
                    fail("partial_timing_sample_mismatch")
            refill_entries = records.get("AUDIO86_P4_PCM86_REFILL", [])
            if refill_entries:
                refill = one(records, "AUDIO86_P4_PCM86_REFILL")
                require(refill, "count", "non_refill_count")
                if integer(refill, "count") + integer(refill, "non_refill_count") != progress["service_sample_count"]:
                    fail("partial_refill_sample_mismatch")
                if "planned_refill_quanta" in refill and integer(refill, "planned_refill_quanta") != 323:
                    fail("planned_refill_classification")
                if "planned_non_refill_quanta" in refill and integer(refill, "planned_non_refill_quanta") != 11677:
                    fail("planned_refill_classification")
        return failure_class
    if not failed or allocations[-1]["result"] != "FAIL":
        fail("missing_final_failed_allocation")
    expected_field = "allocation" if failure_class == "ALLOCATION" else "resource"
    if failure.get(expected_field) != failed[-1].get("name"):
        fail("failed_resource_mismatch")
    if failure_class == "ALLOCATION" and failed[-1].get("caps") != "INTERNAL|8BIT":
        fail("allocation_caps")
    identity = records.get("AUDIO86_P4_IDENTITY", [])
    if identity:
        if len(identity) != 1:
            fail("identity_count")
        for field in SHA_FIELDS:
            if field in identity[0] and not SHA_RE.fullmatch(identity[0][field]):
                fail(f"malformed_{field}")
    return failure.get("first_error", "ALLOCATION")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    try:
        records, markers = parse(args.log)
        if args.smoke:
            validate_smoke(records, markers)
            print("AUDIO86_P4_LOG_VALIDATION=PASS")
            return 0
        results = markers.get("AUDIO86_P4_RESULT", [])
        if len(results) != 1:
            fail("result_marker_count")
        if results[0] == "PASS":
            validate_full_pass(records)
            print("AUDIO86_P4_LOG_VALIDATION=FULL_FORMAL_PASS_VALIDATION")
            print("AUDIO86_P4_LOG_VALIDATION=PASS")
            return 0
        if results[0] == "FAIL":
            failure_class = validate_target_fail(records)
            progress_status = ("PRESENT" if records.get("AUDIO86_P4_PROGRESS")
                               else "LEGACY_UNAVAILABLE")
            print("AUDIO86_P4_LOG_VALIDATION=WELL_FORMED_TARGET_FAIL_VALIDATION"
                  f" target_result=FAIL failure_class={failure_class}"
                  f" progress={progress_status}")
            return 1
        fail("unknown_result")
    except (OSError, UnicodeError, ValueError) as exc:
        reason = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(exc)).strip("_") or "parse_error"
        print(f"AUDIO86_P4_LOG_VALIDATION=MALFORMED_LOG reason={reason}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
