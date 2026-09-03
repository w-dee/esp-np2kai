#!/usr/bin/env python3
"""Fail-closed validator for 86R.5D.2 S1 physical UART evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    from .p4_log_safety import fatal_pattern_names
except ImportError:  # Direct script execution.
    from p4_log_safety import fatal_pattern_names


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
REAL_GUEST_PASS_LINE = b"P4_AUDIO86_REAL_GUEST_RESULT=PASS"
REAL_GUEST_PREFIX = b"P4_AUDIO86_REAL_GUEST_RESULT="
OUTER_GUEST_FAIL_PREFIX = b"P4_NANO_AUDIO86_REAL_GUEST_STATUS=FAIL"
SECOND_STAGE_BOOT_LINE = re.compile(
    rb"I \([0-9]+\) boot: ESP-IDF .+ 2nd stage bootloader", re.IGNORECASE
)
APP_MAIN_CALL_LINE = re.compile(
    rb"I \([0-9]+\) main_task: Calling app_main\(\)", re.IGNORECASE
)
MIN_POST_TERMINAL_DRAIN_SECONDS = 0.5


@dataclass(frozen=True)
class RawLine:
    start: int
    end: int
    content: bytes
    complete: bool


@dataclass(frozen=True)
class CaptureRegions:
    raw: bytes
    reset_offset: int
    canonical_start_offset: int
    terminal_line_start_offset: int
    terminal_line_end_offset: int
    terminal_kind: str

    @property
    def canonical(self) -> bytes:
        return self.raw[self.canonical_start_offset:self.terminal_line_end_offset]

    @property
    def tail(self) -> bytes:
        return self.raw[self.terminal_line_end_offset:]


def raw_lines(raw: bytes, start: int = 0, end: int | None = None) -> list[RawLine]:
    """Return raw-relative LF/CRLF lines, retaining a possible final fragment."""
    limit = len(raw) if end is None else end
    result: list[RawLine] = []
    cursor = start
    while cursor < limit:
        newline = raw.find(b"\n", cursor, limit)
        if newline < 0:
            result.append(RawLine(cursor, limit, raw[cursor:limit], False))
            break
        content = raw[cursor:newline]
        if content.endswith(b"\r"):
            content = content[:-1]
        result.append(RawLine(cursor, newline + 1, content, True))
        cursor = newline + 1
    return result


def require(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def decimal(fields: dict[str, str], key: str, errors: list[str]) -> int:
    value = fields.get(key, "")
    if DECIMAL.fullmatch(value) is None:
        errors.append(f"{key}: invalid decimal")
        return -1
    return int(value)


def parse_records(raw: bytes, errors: list[str]) -> dict[str, dict[str, str]]:
    found: list[tuple[str, dict[str, str]]] = []
    for raw_line in raw.splitlines():
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


def canonical_execution_start(raw: bytes, reset_offset: int,
                              terminal_start: int,
                              errors: list[str]) -> int | None:
    preterminal = raw_lines(raw, reset_offset, terminal_start)
    app_calls = [line for line in preterminal
                 if APP_MAIN_CALL_LINE.fullmatch(line.content)]
    require(errors, len(app_calls) == 1,
            "fresh canonical app_main start missing or duplicated")
    if len(app_calls) != 1:
        return None
    boot_matches = [
        (line, match)
        for line in preterminal
        if line.end <= app_calls[0].start
        for match in [SECOND_STAGE_BOOT_LINE.search(line.content)]
        if match is not None
    ]
    require(errors, bool(boot_matches), "fresh canonical second-stage boot missing")
    if not boot_matches:
        return None
    boot_line, boot_match = boot_matches[-1]
    canonical_start = boot_line.start + boot_match.start()
    for line in preterminal:
        if line.start < app_calls[0].end:
            continue
        require(errors,
                b"ESP-ROM:esp32p4" not in line.content and
                SECOND_STAGE_BOOT_LINE.search(line.content) is None,
                "unexpected reset/boot inside canonical S1 execution")
    for line in raw_lines(raw, reset_offset, canonical_start):
        stripped = line.content.lstrip()
        require(errors,
                not stripped.startswith(b"5D2_S1_") and
                not stripped.startswith(REAL_GUEST_PREFIX),
                "authoritative S1 evidence precedes canonical boot")
    return canonical_start


def validate_status(raw_path: Path, status_path: Path,
                    errors: list[str]) -> CaptureRegions | None:
    raw = raw_path.read_bytes()
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        errors.append(f"invalid capture status: {error}")
        return None
    if not isinstance(status, dict):
        errors.append("capture status root must be an object")
        return None
    if status.get("schema_version") != 2:
        errors.append("capture status schema mismatch: final acceptance requires v2")
        return None
    require(errors, status.get("raw_bytes") == len(raw),
            "capture raw byte count mismatch")
    require(errors,
            status.get("raw_sha256") == hashlib.sha256(raw).hexdigest(),
            "capture raw SHA-256 mismatch")
    require(errors, status.get("reset_count") == 1,
            "capture reset count mismatch")
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
    drain_seconds = status.get("post_terminal_drain_seconds")
    require(errors,
            isinstance(drain_seconds, (int, float)) and
            not isinstance(drain_seconds, bool) and
            math.isfinite(drain_seconds) and
            drain_seconds >= MIN_POST_TERMINAL_DRAIN_SECONDS,
            "post-terminal drain interval is missing or too short")
    raw_final_line_complete = not raw or raw.endswith(b"\n")
    require(errors,
            status.get("raw_final_line_complete") is raw_final_line_complete,
            "raw final line completeness metadata mismatch")
    reset_offset = status.get("reset_byte_offset")
    if not isinstance(reset_offset, int) or isinstance(reset_offset, bool) or not (
            0 <= reset_offset <= len(raw)):
        errors.append("invalid reset byte offset")
        return None

    terminal_lines = [
        line for line in raw_lines(raw, reset_offset)
        if line.content in {PASS_LINE, FAIL_LINE}
    ]
    require(errors, len(terminal_lines) == 1,
            "exactly one COMPLETE or FAILED terminal line is required")
    if not terminal_lines:
        return None
    observed = terminal_lines[0]
    require(errors, observed.complete,
            "terminal marker text is not terminated by LF/CRLF")
    terminal_kind = "PASS" if observed.content == PASS_LINE else "FAIL"
    expected_exit = f"TERMINAL_{terminal_kind}"
    require(errors,
            status.get("terminal_status") == terminal_kind and
            status.get("terminal_marker") == terminal_kind and
            status.get("exit_reason") == expected_exit and
            status.get("state") == expected_exit,
            "capture terminal metadata disagrees with raw terminal line")
    require(errors, status.get("terminal_line_complete") is observed.complete,
            "terminal line completeness metadata mismatch")

    terminal_start = status.get("terminal_line_start_offset")
    terminal_end = status.get("terminal_line_end_offset")
    offsets_valid = (
        isinstance(terminal_start, int) and not isinstance(terminal_start, bool) and
        isinstance(terminal_end, int) and not isinstance(terminal_end, bool) and
        reset_offset <= terminal_start < terminal_end <= len(raw)
    )
    require(errors, offsets_valid, "invalid terminal line byte offsets")
    if not offsets_valid:
        return None
    require(errors,
            terminal_start == observed.start and terminal_end == observed.end,
            "terminal line byte offsets do not identify the raw terminal line")
    if terminal_start != observed.start or terminal_end != observed.end:
        return None
    require(errors, raw[terminal_end - 1:terminal_end] == b"\n",
            "terminal line end offset does not follow a complete delimiter")
    require(errors, terminal_kind == "PASS", "FAILED terminal line present")

    canonical_start = canonical_execution_start(
        raw, reset_offset, terminal_start, errors
    )
    if canonical_start is None or not observed.complete:
        return None
    return CaptureRegions(
        raw=raw,
        reset_offset=reset_offset,
        canonical_start_offset=canonical_start,
        terminal_line_start_offset=terminal_start,
        terminal_line_end_offset=terminal_end,
        terminal_kind=terminal_kind,
    )


def validate_region_fatal_safety(data: bytes, label: str,
                                 errors: list[str]) -> None:
    text = data.decode("utf-8", errors="replace")
    for pattern in fatal_pattern_names(text):
        errors.append(f"{label} fatal pattern present: {pattern}")


def validate_tail(tail: bytes, errors: list[str]) -> None:
    for line in raw_lines(tail):
        stripped = line.content.lstrip()
        require(errors, line.content not in {PASS_LINE, FAIL_LINE},
                "additional terminal marker present in post-terminal tail")
        require(errors, not stripped.startswith(b"5D2_S1_"),
                "additional authoritative 5D2 record in post-terminal tail")
        require(errors, not stripped.startswith(REAL_GUEST_PREFIX),
                "additional REAL_GUEST_RESULT in post-terminal tail")
        require(errors, not stripped.startswith(OUTER_GUEST_FAIL_PREFIX),
                "post-terminal outer real-guest status is FAIL")
        require(errors,
                b"ESP-ROM:esp32p4" not in line.content and
                SECOND_STAGE_BOOT_LINE.search(line.content) is None,
                "post-terminal reset/boot signature present")
    validate_region_fatal_safety(tail, "post-terminal tail", errors)


def validate(raw_path: Path, status_path: Path,
             expected_source_sha: str) -> list[str]:
    errors: list[str] = []
    require(errors, SHA1.fullmatch(expected_source_sha) is not None,
            "expected source SHA must be 40 lowercase hex digits")
    try:
        regions = validate_status(raw_path, status_path, errors)
    except OSError as error:
        return [f"capture read failed: {error}"]
    if regions is None:
        return errors
    validate_region_fatal_safety(regions.canonical, "canonical S1 prefix", errors)
    validate_tail(regions.tail, errors)
    canonical_lines = regions.canonical.splitlines()
    real_guest_lines = [line for line in canonical_lines
                        if line.startswith(REAL_GUEST_PREFIX)]
    require(errors, real_guest_lines == [REAL_GUEST_PASS_LINE],
            "canonical REAL_GUEST_RESULT must be exactly one PASS")
    require(errors, bool(canonical_lines) and canonical_lines[-1] == PASS_LINE,
            "canonical prefix does not end at COMPLETE terminal line")
    finish_indices = [
        index for index, line in enumerate(canonical_lines)
        if line.startswith(b"5D2_S1_FINISH ")
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
