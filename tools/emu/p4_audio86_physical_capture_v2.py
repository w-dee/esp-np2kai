#!/usr/bin/env python3
"""Profile-neutral capture-status-v2 validation for physical Audio 86."""

from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path

try:
    from .p4_log_safety import fatal_pattern_names
except ImportError:  # Direct script execution.
    from p4_log_safety import fatal_pattern_names


DECIMAL = re.compile(r"0|[1-9][0-9]*")
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


def _canonical_execution_start(
    raw: bytes,
    reset_offset: int,
    terminal_start: int,
    authoritative_prefixes: tuple[bytes, ...],
    execution_label: str,
    errors: list[str],
) -> int | None:
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
                f"unexpected reset/boot inside canonical {execution_label} execution")
    for line in raw_lines(raw, reset_offset, canonical_start):
        stripped = line.content.lstrip()
        require(errors,
                not any(stripped.startswith(prefix)
                        for prefix in authoritative_prefixes) and
                not stripped.startswith(REAL_GUEST_PREFIX),
                f"authoritative {execution_label} evidence precedes canonical boot")
    return canonical_start


def validate_capture_status_v2(
    raw_path: Path,
    status_path: Path,
    pass_line: bytes,
    fail_line: bytes,
    authoritative_prefixes: tuple[bytes, ...],
    execution_label: str,
    errors: list[str],
) -> CaptureRegions | None:
    """Validate capture metadata and return raw/canonical/tail boundaries."""
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
                pass_line.decode("ascii") and
            status.get("terminal_fail_marker_config") ==
                fail_line.decode("ascii"),
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
        if line.content in {pass_line, fail_line}
    ]
    require(errors, len(terminal_lines) == 1,
            "exactly one COMPLETE or FAILED terminal line is required")
    if not terminal_lines:
        return None
    observed = terminal_lines[0]
    require(errors, observed.complete,
            "terminal marker text is not terminated by LF/CRLF")
    terminal_kind = "PASS" if observed.content == pass_line else "FAIL"
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

    canonical_start = _canonical_execution_start(
        raw, reset_offset, terminal_start, authoritative_prefixes,
        execution_label, errors
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


def validate_structural_tail(
    tail: bytes,
    pass_line: bytes,
    fail_line: bytes,
    authoritative_prefixes: tuple[bytes, ...],
    errors: list[str],
) -> None:
    """Reject post-terminal authority, failure, fatal, and reboot evidence."""
    for line in raw_lines(tail):
        stripped = line.content.lstrip()
        require(errors, line.content not in {pass_line, fail_line},
                "additional terminal marker present in post-terminal tail")
        require(errors,
                not any(stripped.startswith(prefix)
                        for prefix in authoritative_prefixes),
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
