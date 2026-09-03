#!/usr/bin/env python3
"""Fail-closed validator for 86R.5D.2 S2 physical UART evidence."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

try:
    from .p4_audio86_physical_capture_v2 import (
        UINT32_HALF_RANGE,
        decimal,
        require,
        validate_capture_status_v2,
        validate_region_fatal_safety,
        validate_structural_tail,
        valid_source_git_sha,
        uint32_delta,
    )
except ImportError:  # Direct script execution.
    from p4_audio86_physical_capture_v2 import (
        UINT32_HALF_RANGE,
        decimal,
        require,
        validate_capture_status_v2,
        validate_region_fatal_safety,
        validate_structural_tail,
        valid_source_git_sha,
        uint32_delta,
    )


ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
RECORD_FIELDS = {
    "5D2_S2_IDENTITY": (
        "schema", "evidence_class", "source_git_sha", "profile", "board",
        "backend", "stimulus", "display",
    ),
    "5D2_S2_START": (
        "schema", "evidence_class", "rate_hz", "channels", "sample_bits",
        "encoding", "i2s_format", "clock_source", "mclk_multiple", "mclk_hz",
        "q_frames", "bytes_per_frame", "physical_unit_bytes", "dma_desc",
        "dma_frames", "ring_capacity", "prefill", "prepare_completed",
        "pa_initial_low", "codec_initialized_muted", "i2s_initialized",
        "muted_warmup_completed", "callbacks_registered", "stream_started",
        "codec_unmute_completed",
    ),
    "5D2_S2_STREAM": (
        "schema", "evidence_class", "semantic_frames", "semantic_bytes",
        "semantic_crc32", "semantic_sha256", "produced_frames",
        "produced_bytes", "produced_slots", "controller_accepted_frames",
        "controller_accepted_bytes", "sink_accepted_frames",
        "sink_accepted_bytes", "physical_units", "full_units",
        "final_partial_units", "final_valid_frames", "padding_frames",
        "padding_bytes", "preloaded_units", "running_units",
        "submit_attempts", "retry_count", "running_q_ovf",
        "final_ring_occupancy", "final_ring_partial", "drops", "overwrite",
        "abandoned_published", "abandoned_partial", "abandoned_rendered",
        "semantic_duration_ms",
    ),
    "5D2_S2_FINISH": (
        "schema", "evidence_class", "controller_state", "sink_state",
        "final_copy_eof_epoch", "drain_completion_eof_epoch",
        "quiescent_eof_epoch", "drain_post_snapshot_eofs",
        "quiescent_post_snapshot_eofs", "drain_duration_ms",
        "finish_completed", "pending_frames", "drained_frames",
        "discarded_frames", "draining_q_ovf", "sticky_error",
        "registered_generation", "terminal_generation", "stale_callbacks",
        "callback_in_flight", "callbacks_active", "codec_final_muted",
        "pa_final_low", "i2s_enabled", "i2s_created", "first_error",
        "forced_abort", "sink_destroyed",
    ),
}
RECORD_ORDER = tuple(RECORD_FIELDS)
CRC32 = re.compile(r"[0-9a-f]{8}")
SHA256 = re.compile(r"[0-9a-f]{64}")
PASS_LINE = b"P4_AUDIO86_PHYSICAL_S2_TERMINAL=COMPLETE"
FAIL_LINE = b"P4_AUDIO86_PHYSICAL_S2_TERMINAL=FAILED"
REAL_GUEST_PASS_LINE = b"P4_AUDIO86_REAL_GUEST_RESULT=PASS"
REAL_GUEST_PREFIX = b"P4_AUDIO86_REAL_GUEST_RESULT="
S1_TERMINAL_PREFIX = b"P4_AUDIO86_PHYSICAL_S1_TERMINAL="
AUTHORITATIVE_PREFIXES = (b"5D2_S2_", b"5D2_S1_", S1_TERMINAL_PREFIX)


def parse_records(raw: bytes, errors: list[str]) -> dict[str, dict[str, str]]:
    found: list[tuple[str, dict[str, str]]] = []
    for raw_line in raw.splitlines():
        if raw_line.lstrip().startswith(S1_TERMINAL_PREFIX):
            errors.append("S1 terminal marker is not S2 evidence")
            continue
        if raw_line.lstrip().startswith(b"5D2_S1_"):
            errors.append("S1 authoritative record is not S2 evidence")
            continue
        if raw_line.lstrip().startswith(b"5D2_S2_") and not raw_line.startswith(
                b"5D2_S2_"):
            errors.append("authoritative record must start in column zero")
            continue
        if not raw_line.startswith(b"5D2_S2_"):
            continue
        try:
            line = raw_line.decode("ascii")
        except UnicodeDecodeError:
            errors.append("non-ASCII 5D2_S2 record")
            continue
        tokens = line.split()
        name = tokens[0]
        if name not in RECORD_FIELDS:
            errors.append(f"unknown record {name}")
            continue
        pairs: list[tuple[str, str]] = []
        malformed = False
        for token in tokens[1:]:
            if token.count("=") != 1:
                malformed = True
                break
            key, value = token.split("=", 1)
            if not key or not value:
                malformed = True
                break
            pairs.append((key, value))
        if malformed:
            errors.append(f"malformed record {name}")
            continue
        keys = tuple(key for key, _ in pairs)
        require(errors, keys == RECORD_FIELDS[name],
                f"{name}: field set/order mismatch")
        require(errors, len(set(keys)) == len(keys),
                f"{name}: duplicate field")
        found.append((name, dict(pairs)))
    require(errors, tuple(name for name, _ in found) == RECORD_ORDER,
            "authoritative record count/order mismatch")
    records: dict[str, dict[str, str]] = {}
    for name, fields in found:
        require(errors, name not in records, f"duplicate record {name}")
        records[name] = fields
    return records


def _load_full_replay_golden(errors: list[str]) -> dict[str, str] | None:
    try:
        document = json.loads(GOLDEN.read_text(encoding="utf-8"))
        values = document["values"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        errors.append(f"invalid full-replay PCM golden: {error}")
        return None
    required = (
        "FULL_REPLAY_PCM_FRAMES", "FULL_REPLAY_PCM_BYTES",
        "FULL_REPLAY_PCM_CRC32", "FULL_REPLAY_PCM_SHA256",
        "FULL_REPLAY_PCM_PEAK",
    )
    if not isinstance(values, dict) or not all(
            isinstance(values.get(key), str) for key in required):
        errors.append("full-replay PCM golden fields missing or malformed")
        return None
    return values


def validate(raw_path: Path, status_path: Path,
             expected_source_sha: str) -> list[str]:
    errors: list[str] = []
    require(errors, valid_source_git_sha(expected_source_sha),
            "expected source SHA must be 40 lowercase hex digits")
    try:
        regions = validate_capture_status_v2(
            raw_path, status_path, PASS_LINE, FAIL_LINE,
            AUTHORITATIVE_PREFIXES, "S2", errors
        )
    except OSError as error:
        return [f"capture read failed: {error}"]
    if regions is None:
        return errors
    validate_region_fatal_safety(regions.canonical, "canonical S2 prefix", errors)
    validate_structural_tail(
        regions.tail, PASS_LINE, FAIL_LINE, AUTHORITATIVE_PREFIXES, errors
    )
    canonical_lines = regions.canonical.splitlines()
    real_guest_lines = [line for line in canonical_lines
                        if line.startswith(REAL_GUEST_PREFIX)]
    require(errors, real_guest_lines == [REAL_GUEST_PASS_LINE],
            "canonical REAL_GUEST_RESULT must be exactly one PASS")
    require(errors, bool(canonical_lines) and canonical_lines[-1] == PASS_LINE,
            "canonical prefix does not end at COMPLETE terminal line")
    finish_indices = [
        index for index, line in enumerate(canonical_lines)
        if line.startswith(b"5D2_S2_FINISH ")
    ]
    real_guest_indices = [
        index for index, line in enumerate(canonical_lines)
        if line.startswith(REAL_GUEST_PREFIX)
    ]
    require(errors,
            len(finish_indices) == 1 and len(real_guest_indices) == 1 and
            finish_indices[0] < real_guest_indices[0] < len(canonical_lines) - 1,
            "canonical finish/result/terminal order mismatch")
    records = parse_records(regions.canonical, errors)
    if set(records) != set(RECORD_ORDER):
        return errors

    identity = records["5D2_S2_IDENTITY"]
    start = records["5D2_S2_START"]
    stream = records["5D2_S2_STREAM"]
    finish = records["5D2_S2_FINISH"]
    for name, fields in records.items():
        require(errors, fields["schema"] == "1", f"{name}: schema mismatch")
        require(errors, fields["evidence_class"] == "PHYSICAL_EXEC",
                f"{name}: evidence class mismatch")
    require(errors, valid_source_git_sha(identity["source_git_sha"]) and
            identity["source_git_sha"] == expected_source_sha,
            "source Git SHA mismatch")
    require(errors, identity == {
        "schema": "1", "evidence_class": "PHYSICAL_EXEC",
        "source_git_sha": expected_source_sha,
        "profile": "AUDIO86_REAL_GUEST_PHYSICAL_I2S",
        "board": "P4_NANO_P4_V1X", "backend": "IDF_I2S0_ES8311",
        "stimulus": "FULL_REPLAY_PCM", "display": "DISABLED",
    }, "physical identity mismatch")

    expected_start = {
        "rate_hz": "48000", "channels": "2", "sample_bits": "16",
        "encoding": "S16LE", "i2s_format": "PHILIPS",
        "clock_source": "APLL", "mclk_multiple": "256",
        "mclk_hz": "12288000", "q_frames": "240",
        "bytes_per_frame": "4", "physical_unit_bytes": "960",
        "dma_desc": "4", "dma_frames": "240", "ring_capacity": "8",
        "prefill": "4", "prepare_completed": "1", "pa_initial_low": "1",
        "codec_initialized_muted": "1", "i2s_initialized": "1",
        "muted_warmup_completed": "1", "callbacks_registered": "1",
        "stream_started": "1", "codec_unmute_completed": "1",
    }
    require(errors, all(start.get(key) == value
                        for key, value in expected_start.items()),
            "start geometry/history mismatch")

    numeric: dict[str, int] = {}
    enum_fields = {
        "schema", "evidence_class", "encoding", "i2s_format", "clock_source",
        "semantic_crc32", "semantic_sha256", "controller_state", "sink_state",
    }
    for label, name, fields in (
        ("start", "5D2_S2_START", start),
        ("stream", "5D2_S2_STREAM", stream),
        ("finish", "5D2_S2_FINISH", finish),
    ):
        for key in RECORD_FIELDS[name]:
            if key not in enum_fields:
                numeric[f"{label}.{key}"] = decimal(fields, key, errors)

    require(errors, CRC32.fullmatch(stream["semantic_crc32"]) is not None,
            "semantic CRC32 format mismatch")
    require(errors, SHA256.fullmatch(stream["semantic_sha256"]) is not None,
            "semantic SHA-256 format mismatch")
    golden = _load_full_replay_golden(errors)
    if golden is None:
        return errors
    try:
        golden_frames = int(golden["FULL_REPLAY_PCM_FRAMES"])
        golden_bytes = int(golden["FULL_REPLAY_PCM_BYTES"])
        golden_peak = int(golden["FULL_REPLAY_PCM_PEAK"])
    except ValueError:
        errors.append("full-replay PCM golden numeric field malformed")
        return errors
    require(errors, golden_frames == 2400 and golden_bytes == 9600 and
            golden_peak == 4148,
            "full-replay PCM golden identity mismatch")
    semantic_frames = numeric["stream.semantic_frames"]
    semantic_bytes = numeric["stream.semantic_bytes"]
    require(errors,
            semantic_frames == golden_frames and
            semantic_bytes == golden_bytes and
            stream["semantic_crc32"] == golden["FULL_REPLAY_PCM_CRC32"] and
            stream["semantic_sha256"] == golden["FULL_REPLAY_PCM_SHA256"],
            "FULL_REPLAY_PCM golden mismatch")

    q_frames = numeric["start.q_frames"]
    bytes_per_frame = numeric["start.bytes_per_frame"]
    unit_bytes = numeric["start.physical_unit_bytes"]
    physical_units = numeric["stream.physical_units"]
    preloaded_units = numeric["stream.preloaded_units"]
    retry_count = numeric["stream.retry_count"]
    require(errors, semantic_bytes == semantic_frames * bytes_per_frame,
            "semantic byte arithmetic mismatch")
    require(errors, unit_bytes == q_frames * bytes_per_frame,
            "physical unit byte arithmetic mismatch")
    require(errors, semantic_frames % q_frames == 0 and
            physical_units == semantic_frames // q_frames,
            "physical unit geometry mismatch")
    require(errors,
            numeric["stream.produced_frames"] == semantic_frames and
            numeric["stream.produced_bytes"] == semantic_bytes and
            numeric["stream.produced_slots"] == physical_units,
            "produced workload geometry mismatch")
    require(errors,
            numeric["stream.controller_accepted_frames"] == semantic_frames and
            numeric["stream.controller_accepted_bytes"] == semantic_bytes and
            numeric["stream.sink_accepted_frames"] == semantic_frames and
            numeric["stream.sink_accepted_bytes"] == semantic_bytes,
            "accepted ownership mismatch")
    require(errors,
            physical_units == 10 and numeric["stream.full_units"] == physical_units and
            numeric["stream.final_partial_units"] == 0 and
            numeric["stream.final_valid_frames"] == 0 and
            numeric["stream.padding_frames"] == 0 and
            numeric["stream.padding_bytes"] == 0,
            "full-unit/padding geometry mismatch")
    running_units = physical_units - preloaded_units
    require(errors, preloaded_units == numeric["start.prefill"] == 4 and
            running_units == 6 and
            numeric["stream.running_units"] == running_units,
            "preload/RUNNING unit arithmetic mismatch")
    require(errors, retry_count >= 0 and
            numeric["stream.submit_attempts"] == physical_units + retry_count,
            "submit/retry arithmetic mismatch")
    require(errors, numeric["stream.running_q_ovf"] == 0,
            "RUNNING queue overflow must be zero")
    require(errors,
            numeric["stream.final_ring_occupancy"] == 0 and
            numeric["stream.final_ring_partial"] == 0 and
            numeric["stream.drops"] == 0 and
            numeric["stream.overwrite"] == 0 and
            numeric["stream.abandoned_published"] == 0 and
            numeric["stream.abandoned_partial"] == 0 and
            numeric["stream.abandoned_rendered"] == 0,
            "ring/drop/abandonment mismatch")
    require(errors, numeric["stream.semantic_duration_ms"] ==
            semantic_frames * 1000 // numeric["start.rate_hz"],
            "semantic duration arithmetic mismatch")

    final_epoch = numeric["finish.final_copy_eof_epoch"]
    drain_epoch = numeric["finish.drain_completion_eof_epoch"]
    quiescent_epoch = numeric["finish.quiescent_eof_epoch"]
    require(errors, 0 <= final_epoch <= 0xFFFFFFFF and
            0 <= drain_epoch <= 0xFFFFFFFF and
            0 <= quiescent_epoch <= 0xFFFFFFFF,
            "EOF epoch outside uint32 range")
    drain_delta = uint32_delta(drain_epoch, final_epoch)
    quiescent_delta = uint32_delta(quiescent_epoch, final_epoch)
    require(errors,
            numeric["finish.drain_post_snapshot_eofs"] == drain_delta and
            numeric["finish.quiescent_post_snapshot_eofs"] == quiescent_delta,
            "reported EOF interval delta mismatch")
    require(errors, drain_delta < UINT32_HALF_RANGE and
            quiescent_delta < UINT32_HALF_RANGE,
            "EOF interval exceeds bounded S2 modular range")
    require(errors, drain_delta >= numeric["start.dma_desc"],
            "four-EOF drain proof mismatch")
    require(errors, quiescent_delta >= drain_delta,
            "quiescent EOF observation precedes drain completion")
    require(errors, numeric["finish.draining_q_ovf"] <= quiescent_delta,
            "drain q_ovf exceeds quiescent post-snapshot EOFs")
    require(errors, numeric["finish.drain_duration_ms"] < 40,
            "drain duration reached physical finish timeout")
    require(errors, finish["controller_state"] == "FINISHED" and
            finish["sink_state"] == "QUIESCENT",
            "terminal state mismatch")
    required_finish = {
        "finish_completed": 1, "pending_frames": 0,
        "drained_frames": semantic_frames, "discarded_frames": 0,
        "sticky_error": 0, "stale_callbacks": 0,
        "callback_in_flight": 0, "callbacks_active": 0,
        "codec_final_muted": 1, "pa_final_low": 1, "i2s_enabled": 0,
        "i2s_created": 0, "first_error": 0, "forced_abort": 0,
        "sink_destroyed": 1,
    }
    require(errors, all(numeric[f"finish.{key}"] == value
                        for key, value in required_finish.items()),
            "finish/quiescence mismatch")
    require(errors, numeric["finish.registered_generation"] > 0 and
            numeric["finish.terminal_generation"] ==
                numeric["finish.registered_generation"] + 1,
            "callback generation mismatch")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--expected-source-sha", required=True)
    args = parser.parse_args()
    errors = validate(args.raw, args.status, args.expected_source_sha)
    if errors:
        for error in errors:
            print(f"S2_PHYSICAL_EVIDENCE_ERROR={error}", file=sys.stderr)
        return 1
    print("S2_PHYSICAL_EVIDENCE_VALIDATION=PASS")
    print("S2_CAPTURE_STATUS_V2_REUSE=PASS")
    print("S2B_REUSES_EXISTING_FULL_REPLAY_GOLDEN=PASS")
    print("S2_STREAM_GEOMETRY_RECOMPUTED=PASS")
    print("S2_CANONICAL_PCM_IDENTITY=PASS")
    print("S2_RETRY_RUNTIME_RULE=PASS")
    print("S2_DRAIN_Q_OVF_INTERVAL_RULE=PASS")
    print("S2_EOF_MODULAR_ARITHMETIC=PASS")
    print("S2_OWNERSHIP_CONSERVATION=PASS")
    print("S2_DRAIN_DURATION_VALIDATION=PASS")
    print("S2_TERMINAL_POLICY_STRICT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
