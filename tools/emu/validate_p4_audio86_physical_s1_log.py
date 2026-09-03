#!/usr/bin/env python3
"""Fail-closed validator for 86R.5D.2 S1 physical UART evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
RECORD_FIELDS = {
    "5D2_S1_IDENTITY": (
        "schema", "evidence_class", "source_git_sha", "profile", "board",
        "backend", "stimulus", "display",
    ),
    "5D2_S1_START": (
        "schema", "evidence_class", "rate_hz", "channels", "sample_bits",
        "encoding", "i2s_format", "clock_source", "mclk_multiple", "mclk_hz",
        "q_frames", "bytes_per_frame", "physical_unit_bytes", "dma_desc",
        "dma_frames", "prepare_completed", "pa_initial_low",
        "codec_initialized_muted", "i2s_initialized", "muted_warmup_completed",
        "callbacks_registered", "stream_started", "codec_unmute_completed",
    ),
    "5D2_S1_PCM": (
        "schema", "evidence_class", "semantic_frames", "semantic_bytes",
        "semantic_crc32", "semantic_sha256", "controller_accepted_frames",
        "controller_accepted_bytes", "sink_accepted_frames", "sink_accepted_bytes",
        "full_units", "final_partial_units", "final_valid_frames",
        "physical_units", "physical_bytes", "padding_frames", "padding_bytes",
        "submit_attempts", "retry_count",
    ),
    "5D2_S1_FINISH": (
        "schema", "evidence_class", "controller_state", "sink_state",
        "final_copy_eof_epoch", "drain_completion_eof_epoch",
        "quiescent_eof_epoch", "drain_post_snapshot_eofs",
        "quiescent_post_snapshot_eofs", "drain_duration_ms",
        "finish_completed", "pending_frames",
        "drained_frames", "discarded_frames", "running_q_ovf",
        "draining_q_ovf", "sticky_error", "registered_generation",
        "terminal_generation", "stale_callbacks", "callback_in_flight",
        "callbacks_active", "codec_final_muted", "pa_final_low", "i2s_enabled",
        "i2s_created", "first_error", "forced_abort", "sink_destroyed",
    ),
}
RECORD_ORDER = tuple(RECORD_FIELDS)
DECIMAL = re.compile(r"0|[1-9][0-9]*")
SHA1 = re.compile(r"[0-9a-f]{40}")
CRC32 = re.compile(r"[0-9a-f]{8}")
SHA256 = re.compile(r"[0-9a-f]{64}")
UINT32_HALF_RANGE = 1 << 31
PASS_LINE = b"P4_AUDIO86_PHYSICAL_S1_TERMINAL=COMPLETE"
FAIL_LINE = b"P4_AUDIO86_PHYSICAL_S1_TERMINAL=FAILED"


def require(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def decimal(fields: dict[str, str], key: str, errors: list[str]) -> int:
    value = fields.get(key, "")
    if DECIMAL.fullmatch(value) is None:
        errors.append(f"{key}: invalid decimal")
        return -1
    return int(value)


def parse_records(raw: bytes, reset_offset: int,
                  errors: list[str]) -> dict[str, dict[str, str]]:
    found: list[tuple[str, dict[str, str]]] = []
    for raw_line in raw[reset_offset:].splitlines():
        if raw_line.lstrip().startswith(b"5D2_S1_") and not raw_line.startswith(
                b"5D2_S1_"):
            errors.append("authoritative record must start in column zero")
            continue
        if not raw_line.startswith(b"5D2_S1_"):
            continue
        try:
            line = raw_line.decode("ascii")
        except UnicodeDecodeError:
            errors.append("non-ASCII 5D2_S1 record")
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


def validate_status(raw_path: Path, status_path: Path,
                    errors: list[str]) -> tuple[bytes, int]:
    raw = raw_path.read_bytes()
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        errors.append(f"invalid capture status: {error}")
        return raw, 0
    require(errors, status.get("schema_version") == 1,
            "capture status schema mismatch")
    require(errors, status.get("raw_bytes") == len(raw),
            "capture raw byte count mismatch")
    require(errors,
            status.get("raw_sha256") == hashlib.sha256(raw).hexdigest(),
            "capture raw SHA-256 mismatch")
    require(errors, status.get("reset_count") == 1,
            "capture reset count mismatch")
    require(errors, status.get("terminal_status") == "PASS" and
            status.get("terminal_marker") == "PASS" and
            status.get("exit_reason") == "TERMINAL_PASS" and
            status.get("state") == "TERMINAL_PASS",
            "capture did not close on the configured COMPLETE line")
    require(errors,
            status.get("terminal_pass_marker_config") ==
                PASS_LINE.decode("ascii") and
            status.get("terminal_fail_marker_config") ==
                FAIL_LINE.decode("ascii"),
            "capture terminal marker configuration mismatch")
    require(errors, status.get("idle_timeout_enabled") is False,
            "capture unexpectedly used an idle timeout")
    require(errors, status.get("serial_error") is None,
            "capture reported a serial/control error")
    require(errors, status.get("final_line_complete") is True,
            "capture ended with an incomplete line")
    reset_offset = status.get("reset_byte_offset")
    if not isinstance(reset_offset, int) or isinstance(reset_offset, bool) or not (
            0 <= reset_offset <= len(raw)):
        errors.append("invalid reset byte offset")
        reset_offset = 0
    canonical = raw[reset_offset:]
    canonical_lines = canonical.splitlines()
    require(errors, canonical_lines.count(PASS_LINE) == 1,
            "COMPLETE terminal line missing or duplicated")
    require(errors, canonical_lines.count(FAIL_LINE) == 0,
            "FAILED terminal line present")
    require(errors, bool(canonical_lines) and canonical_lines[-1] == PASS_LINE,
            "COMPLETE terminal line is not the final captured record")
    return raw, reset_offset


def validate(raw_path: Path, status_path: Path,
             expected_source_sha: str) -> list[str]:
    errors: list[str] = []
    require(errors, SHA1.fullmatch(expected_source_sha) is not None,
            "expected source SHA must be 40 lowercase hex digits")
    try:
        raw, reset_offset = validate_status(raw_path, status_path, errors)
    except OSError as error:
        return [f"capture read failed: {error}"]
    records = parse_records(raw, reset_offset, errors)
    if set(records) != set(RECORD_ORDER):
        return errors
    identity = records["5D2_S1_IDENTITY"]
    start = records["5D2_S1_START"]
    pcm = records["5D2_S1_PCM"]
    finish = records["5D2_S1_FINISH"]
    for name, fields in records.items():
        require(errors, fields["schema"] == "2", f"{name}: schema mismatch")
        require(errors, fields["evidence_class"] == "PHYSICAL_EXEC",
                f"{name}: evidence class mismatch")
    require(errors, SHA1.fullmatch(identity["source_git_sha"]) is not None and
            identity["source_git_sha"] == expected_source_sha,
            "source Git SHA mismatch")
    require(errors, identity == {
        "schema": "2", "evidence_class": "PHYSICAL_EXEC",
        "source_git_sha": expected_source_sha,
        "profile": "AUDIO86_REAL_GUEST_PHYSICAL_I2S_SHORT",
        "board": "P4_NANO_P4_V1X", "backend": "IDF_I2S0_ES8311",
        "stimulus": "PRE_RESET_PCM", "display": "DISABLED",
    }, "physical identity mismatch")
    expected_start = {
        "rate_hz": "48000", "channels": "2", "sample_bits": "16",
        "encoding": "S16LE", "i2s_format": "PHILIPS",
        "clock_source": "APLL", "mclk_multiple": "256",
        "mclk_hz": "12288000", "q_frames": "240",
        "bytes_per_frame": "4", "physical_unit_bytes": "960",
        "dma_desc": "4", "dma_frames": "240",
        "prepare_completed": "1", "pa_initial_low": "1",
        "codec_initialized_muted": "1", "i2s_initialized": "1",
        "muted_warmup_completed": "1", "callbacks_registered": "1",
        "stream_started": "1", "codec_unmute_completed": "1",
    }
    require(errors, all(start.get(key) == value
                        for key, value in expected_start.items()),
            "start geometry/history mismatch")
    numeric = {}
    for name, fields in (("start", start), ("pcm", pcm), ("finish", finish)):
        for key in RECORD_FIELDS[f"5D2_S1_{name.upper()}"]:
            if key not in {"schema", "evidence_class", "encoding", "i2s_format",
                           "clock_source", "semantic_crc32", "semantic_sha256",
                           "controller_state", "sink_state"}:
                numeric[f"{name}.{key}"] = decimal(fields, key, errors)
    require(errors, CRC32.fullmatch(pcm["semantic_crc32"]) is not None,
            "semantic CRC32 format mismatch")
    require(errors, SHA256.fullmatch(pcm["semantic_sha256"]) is not None,
            "semantic SHA-256 format mismatch")
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]
    semantic_frames = numeric["pcm.semantic_frames"]
    semantic_bytes = numeric["pcm.semantic_bytes"]
    q_frames = numeric["start.q_frames"]
    bytes_per_frame = numeric["start.bytes_per_frame"]
    unit_bytes = numeric["start.physical_unit_bytes"]
    final_valid = numeric["pcm.final_valid_frames"]
    require(errors, semantic_frames == int(golden["PRE_RESET_PCM_FRAMES"]) and
            semantic_bytes == int(golden["PRE_RESET_PCM_BYTES"]) and
            pcm["semantic_crc32"] == golden["PRE_RESET_PCM_CRC32"] and
            pcm["semantic_sha256"] == golden["PRE_RESET_PCM_SHA256"],
            "PRE_RESET_PCM golden mismatch")
    require(errors, semantic_bytes == semantic_frames * bytes_per_frame,
            "semantic byte arithmetic mismatch")
    require(errors, unit_bytes == q_frames * bytes_per_frame,
            "physical unit byte arithmetic mismatch")
    require(errors, numeric["pcm.physical_bytes"] ==
            numeric["pcm.physical_units"] * unit_bytes,
            "physical byte arithmetic mismatch")
    require(errors, numeric["pcm.padding_frames"] == q_frames - final_valid and
            numeric["pcm.padding_bytes"] ==
                unit_bytes - final_valid * bytes_per_frame,
            "padding arithmetic mismatch")
    require(errors,
            numeric["pcm.controller_accepted_frames"] == semantic_frames and
            numeric["pcm.controller_accepted_bytes"] == semantic_bytes and
            numeric["pcm.sink_accepted_frames"] == semantic_frames and
            numeric["pcm.sink_accepted_bytes"] == semantic_bytes,
            "accepted ownership mismatch")
    require(errors, numeric["pcm.full_units"] == 0 and
            numeric["pcm.final_partial_units"] == 1 and final_valid == 13 and
            numeric["pcm.physical_units"] == 1 and
            numeric["pcm.submit_attempts"] == 1 and
            numeric["pcm.retry_count"] == 0,
            "short physical unit/submission mismatch")
    final_epoch = numeric["finish.final_copy_eof_epoch"]
    drain_epoch = numeric["finish.drain_completion_eof_epoch"]
    quiescent_epoch = numeric["finish.quiescent_eof_epoch"]
    drain_delta = (drain_epoch - final_epoch) & 0xFFFFFFFF
    quiescent_delta = (quiescent_epoch - final_epoch) & 0xFFFFFFFF
    require(errors, 0 <= final_epoch <= 0xFFFFFFFF and
            0 <= drain_epoch <= 0xFFFFFFFF and
            0 <= quiescent_epoch <= 0xFFFFFFFF,
            "EOF epoch outside uint32 range")
    require(errors,
            numeric["finish.drain_post_snapshot_eofs"] == drain_delta and
            numeric["finish.quiescent_post_snapshot_eofs"] ==
                quiescent_delta,
            "reported EOF interval delta mismatch")
    # S1 is bounded by a short drain timeout and is many orders of magnitude
    # below half the uint32 range.  Constraining both modular distances first
    # makes the following ordinary integer ordering unambiguous.
    require(errors, drain_delta < UINT32_HALF_RANGE and
            quiescent_delta < UINT32_HALF_RANGE,
            "EOF interval exceeds bounded S1 modular range")
    require(errors, drain_delta >= numeric["start.dma_desc"],
            "four-EOF drain proof mismatch")
    require(errors, quiescent_delta >= drain_delta,
            "quiescent EOF observation precedes drain completion")
    require(errors, finish["controller_state"] == "FINISHED" and
            finish["sink_state"] == "QUIESCENT",
            "terminal state mismatch")
    required_finish = {
        "finish_completed": 1, "pending_frames": 0,
        "drained_frames": semantic_frames, "discarded_frames": 0,
        "running_q_ovf": 0, "sticky_error": 0,
        "stale_callbacks": 0,
        "callback_in_flight": 0, "callbacks_active": 0,
        "codec_final_muted": 1, "pa_final_low": 1, "i2s_enabled": 0,
        "i2s_created": 0, "first_error": 0, "forced_abort": 0,
        "sink_destroyed": 1,
    }
    require(errors, all(numeric[f"finish.{key}"] == value
                        for key, value in required_finish.items()),
            "finish/quiescence mismatch")
    require(errors, numeric["finish.draining_q_ovf"] <=
            quiescent_delta,
            "drain q_ovf exceeds quiescent post-snapshot EOFs")
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
            print(f"S1_PHYSICAL_EVIDENCE_ERROR={error}", file=sys.stderr)
        return 1
    print("S1_PHYSICAL_EVIDENCE_VALIDATION=PASS")
    print("S1B_REUSES_EXISTING_PCM_GOLDEN=PASS")
    print("S1_PHYSICAL_DRAIN_PROOF_IMPLEMENTED=PASS")
    print("S1_VALIDATOR_EOF_INTERVALS_RECOMPUTED=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
