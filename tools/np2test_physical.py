#!/usr/bin/env python3
"""Validate the formal NP2TEST result stream from a physical UART.

This harness is intentionally observation-only. It never sends UART bytes,
uses File Transfer, flashes firmware, or changes SD contents.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import re
import sys
import time
from typing import Iterable


EXPECTED_FIXTURE_PATH = (
    "/sdcard/files/np2-fixtures/"
    "np2test-a2-20260821-r1-"
    "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3.hdm"
)
EXPECTED_FIXTURE_SIZE = "1261568"
EXPECTED_FIXTURE_SHA256 = (
    "3b73667d235615e89205fbdab04d3e6cf9c2f9a1f3a1de82cdb2b3862aa394b3"
)
EXPECTED_CRC = "0x58f5b827"

_KEY_VALUE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")


@dataclass(frozen=True)
class Marker:
    line_number: int
    text: str
    fields: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class ValidationReport:
    ok: bool
    reasons: tuple[str, ...]
    lines_seen: int
    terminal_result: str | None


def _fields(text: str) -> dict[str, str]:
    return {match.group(1): match.group(2) for match in _KEY_VALUE.finditer(text)}


def _marker(text: str, prefix: str, line_number: int) -> Marker | None:
    position = text.find(prefix)
    if position < 0:
        return None
    suffix = text[position + len(prefix):].strip()
    return Marker(line_number, text, _fields(suffix))


class NP2TestLogParser:
    """Collect relevant markers from arbitrary interleaved UART text."""

    def __init__(self, fixture_path: str = EXPECTED_FIXTURE_PATH) -> None:
        self.fixture_path = fixture_path
        self._buffer = bytearray()
        self.lines_seen = 0
        self._markers: dict[str, list[Marker]] = {
            "mount": [],
            "profile": [],
            "verify": [],
            "disk": [],
            "memory": [],
            "fdd": [],
            "pass": [],
            "result": [],
        }
        self.failure_lines: list[str] = []

    @property
    def terminal_result(self) -> str | None:
        results = self._markers["result"]
        if not results:
            return None
        return results[-1].fields.get("value")

    @property
    def terminal_seen(self) -> bool:
        return bool(self._markers["result"])

    def feed(self, data: bytes) -> None:
        self._buffer.extend(data)
        while True:
            try:
                newline = self._buffer.index(b"\n")
            except ValueError:
                break
            line = bytes(self._buffer[:newline])
            del self._buffer[:newline + 1]
            self.feed_line(line)

    def finish(self) -> None:
        if self._buffer:
            self.feed_line(bytes(self._buffer))
            self._buffer.clear()

    def feed_line(self, data: bytes) -> None:
        self.lines_seen += 1
        text = data.decode("utf-8", errors="replace").rstrip("\r")

        patterns = (
            ("mount", "NP2TEST_SD_MOUNTED "),
            ("profile", "NP2TEST "),
            ("verify", "NP2TEST_VFS_FIXTURE_VERIFY "),
            ("disk", "NP2TEST_DISK_SOURCE "),
            ("memory", "NP2TEST_MEMORY "),
            ("fdd", "NP2TEST_FDD_READY "),
            ("pass", "NP2TEST_PASS "),
        )
        for name, prefix in patterns:
            marker = _marker(text, prefix, self.lines_seen)
            if marker is not None:
                self._markers[name].append(marker)

        result_match = re.search(r"NP2TEST_RESULT=([^\s]+)", text)
        if result_match is not None:
            self._markers["result"].append(
                Marker(
                    self.lines_seen,
                    text,
                    {"value": result_match.group(1)},
                )
            )

        if (
            "NP2TEST_SD_MOUNT=FAIL" in text
            or "NP2TEST_RUNNER_BLOCKED" in text
            or "NP2TEST_FAIL" in text
            or "NP2TEST_RESULT=" in text
            and "NP2TEST_RESULT=PASS" not in text
            or "NP2TEST_VFS_FIXTURE_VERIFY result=FAIL" in text
        ):
            self.failure_lines.append(text)

    def _first(self, name: str) -> Marker | None:
        markers = self._markers[name]
        return markers[0] if markers else None

    def validate(self) -> ValidationReport:
        reasons: list[str] = []
        required_order = ("mount", "profile", "verify", "disk", "memory", "fdd", "pass", "result")
        first_markers = {name: self._first(name) for name in required_order}

        for failure in self.failure_lines:
            if failure not in reasons:
                reasons.append(f"failure marker: {failure}")

        previous_line = -1
        for name in required_order:
            marker = first_markers[name]
            if marker is None:
                reasons.append(f"missing marker: {name}")
                continue
            if marker.line_number <= previous_line:
                reasons.append(f"marker order invalid at: {name}")
            previous_line = marker.line_number

        def require_fields(name: str, expected: dict[str, str]) -> None:
            marker = first_markers[name]
            if marker is None:
                return
            for key, value in expected.items():
                if marker.fields.get(key) != value:
                    reasons.append(
                        f"{name} field {key}={marker.fields.get(key)!r}; expected {value!r}"
                    )

        require_fields(
            "mount",
            {"path": "/sdcard", "fixture": self.fixture_path},
        )
        require_fields(
            "profile",
            {"profile": "formal", "formal_extmem": "13", "effective_extmem": "13"},
        )
        require_fields(
            "verify",
            {
                "result": "PASS",
                "physical": self.fixture_path,
                "size": EXPECTED_FIXTURE_SIZE,
                "sha256": EXPECTED_FIXTURE_SHA256,
                "read_only": "1",
            },
        )
        require_fields(
            "disk",
            {"kind": "vfs", "physical": self.fixture_path},
        )
        require_fields(
            "memory",
            {
                "extmem_mb": "13",
                "actual_bytes": "13631488",
                "ptr_external": "1",
            },
        )
        require_fields("fdd", {"read_only": "1"})
        require_fields(
            "pass",
            {
                "completed": "13",
                "passed": "13",
                "failed": "0",
                "stored_crc": EXPECTED_CRC,
            },
        )

        result = self._first("result")
        if result is not None and result.fields.get("value") != "PASS":
            reasons.append(f"terminal result={result.fields.get('value')!r}; expected 'PASS'")

        return ValidationReport(
            ok=not reasons,
            reasons=tuple(reasons),
            lines_seen=self.lines_seen,
            terminal_result=self.terminal_result,
        )


def parse_log(text: str, fixture_path: str = EXPECTED_FIXTURE_PATH) -> ValidationReport:
    parser = NP2TestLogParser(fixture_path)
    parser.feed(text.encode("utf-8"))
    parser.finish()
    return parser.validate()


def _parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial device to observe")
    parser.add_argument("--baud", required=True, type=int, help="explicit UART baud rate")
    parser.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="wall-clock timeout in seconds (default: 120)",
    )
    parser.add_argument(
        "--fixture-path",
        default=EXPECTED_FIXTURE_PATH,
        help="expected physical A2 fixture path",
    )
    return parser.parse_args(argv)


def _open_serial(port_name: str, baud: int):
    try:
        import serial
    except ImportError as exc:  # pragma: no cover - host dependency
        raise RuntimeError("pyserial is required; use the ESP-IDF Python environment") from exc

    if baud <= 0:
        raise ValueError("--baud must be positive")
    port = serial.Serial()
    port.port = port_name
    port.baudrate = baud
    port.timeout = 0.05
    port.dtr = False
    port.rts = False
    port.open()
    return port


def run_serial(port_name: str, baud: int, timeout: float, fixture_path: str) -> int:
    if timeout <= 0:
        raise ValueError("--timeout must be positive")

    parser = NP2TestLogParser(fixture_path)
    serial_port = _open_serial(port_name, baud)
    deadline = time.monotonic() + timeout
    try:
        while not parser.terminal_seen and time.monotonic() < deadline:
            data = serial_port.read(4096)
            if data:
                parser.feed(data)
            else:
                time.sleep(0.005)
        parser.finish()
    finally:
        serial_port.close()

    report = parser.validate()
    if not parser.terminal_seen:
        report = ValidationReport(
            ok=False,
            reasons=report.reasons + ("host wall-clock timeout: terminal marker not observed",),
            lines_seen=report.lines_seen,
            terminal_result=report.terminal_result,
        )

    if report.ok:
        print(
            "NP2TEST_HARNESS_RESULT=PASS "
            f"lines={report.lines_seen} fixture={fixture_path}"
        )
        return 0

    print("NP2TEST_HARNESS_RESULT=FAIL", file=sys.stderr)
    for reason in report.reasons:
        print(f"reason={reason}", file=sys.stderr)
    return 1


def main(argv: Iterable[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        return run_serial(args.port, args.baud, args.timeout, args.fixture_path)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"NP2TEST_HARNESS_RESULT=FAIL reason={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
