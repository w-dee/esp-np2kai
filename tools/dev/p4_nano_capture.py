#!/usr/bin/env python3
"""Durable, byte-exact P4-NANO UART capture helper.

The formal capture path owns the serial port and raw evidence file directly.
ESP-IDF Monitor is intentionally not used here: its decoded console output and
normal-exit logging are not a durable raw-byte evidence boundary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import time
from enum import Enum
from pathlib import Path
from typing import Callable, Optional


DEFAULT_BAUD = 1_500_000
DEFAULT_HARD_TIMEOUT_SECONDS = 360.0
DEFAULT_READ_TIMEOUT_SECONDS = 0.25
DEFAULT_POST_TERMINAL_DRAIN_SECONDS = 0.5
RESET_LOW_SECONDS = 0.005

PASS_MARKER = b"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS"
FAIL_MARKER = b"P4_AUDIO_ONLY_BENCHMARK_RESULT=FAIL"
TERMINAL_MARKERS = {PASS_MARKER: "PASS", FAIL_MARKER: "FAIL"}


class CaptureState(str, Enum):
    PREPARED = "PREPARED"
    CAPTURING_PRE_RESET = "CAPTURING_PRE_RESET"
    RESET_ISSUED = "RESET_ISSUED"
    BENCHMARK_RUNNING = "BENCHMARK_RUNNING"
    TERMINAL_PASS = "TERMINAL_PASS"
    TERMINAL_FAIL = "TERMINAL_FAIL"
    HARD_TIMEOUT = "HARD_TIMEOUT"
    SERIAL_ERROR = "SERIAL_ERROR"
    CONTROL_ERROR = "CONTROL_ERROR"
    INTERNAL_ERROR = "INTERNAL_ERROR"


class ExitReason(str, Enum):
    TERMINAL_PASS = "TERMINAL_PASS"
    TERMINAL_FAIL = "TERMINAL_FAIL"
    HARD_TIMEOUT = "HARD_TIMEOUT"
    SERIAL_ERROR = "SERIAL_ERROR"
    CONTROL_ERROR = "CONTROL_ERROR"
    INTERNAL_ERROR = "INTERNAL_ERROR"


class CaptureError(RuntimeError):
    """Base error for fail-closed capture state transitions."""


class CaptureStateError(CaptureError):
    """Raised when a caller requests an invalid state transition."""


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError("raw capture write returned no progress")
        view = view[written:]


class RawSink:
    """Exclusive, unbuffered raw sink with per-chunk fdatasync durability."""

    def __init__(self, path: Path) -> None:
        self.path = path
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_CLOEXEC", 0)
        self.fd = os.open(path, flags, 0o644)
        self.bytes_written = 0
        self.digest = hashlib.sha256()
        self.closed = False

    def write(self, data: bytes) -> None:
        if self.closed:
            raise CaptureError("raw sink is closed")
        if not data:
            return
        _write_all(self.fd, data)
        self.bytes_written += len(data)
        self.digest.update(data)
        os.fdatasync(self.fd)

    def sync(self) -> None:
        if not self.closed:
            os.fdatasync(self.fd)

    def close(self) -> None:
        if not self.closed:
            os.close(self.fd)
            self.closed = True

    @property
    def sha256(self) -> str:
        return self.digest.hexdigest()


class TerminalLineDetector:
    """Incrementally detect only complete LF/CRLF terminal lines."""

    def __init__(self) -> None:
        self._line = bytearray()
        self.terminal_marker: Optional[str] = None

    @property
    def final_line_complete(self) -> bool:
        return not self._line

    def reset_epoch(self) -> None:
        """Discard parser-only state at the canonical reset boundary.

        The bytes remain in the raw artifact; setup output must not be able to
        complete or terminate a post-reset benchmark line.
        """
        self._line.clear()
        self.terminal_marker = None

    def feed(self, data: bytes) -> Optional[str]:
        if self.terminal_marker is not None:
            return self.terminal_marker
        self._line.extend(data)
        while True:
            try:
                newline = self._line.index(0x0A)
            except ValueError:
                return None
            line = bytes(self._line[:newline])
            del self._line[: newline + 1]
            if line.endswith(b"\r"):
                line = line[:-1]
            marker = TERMINAL_MARKERS.get(line)
            if marker is not None:
                self.terminal_marker = marker
                return marker


def _sanitize_error(error: BaseException, serial_label: Optional[str] = None) -> str:
    message = f"{type(error).__name__}: {error}"
    if serial_label:
        message = message.replace(serial_label, "<serial>")
    return message


class CaptureSession:
    """State machine shared by the real serial path and host-only tests."""

    def __init__(
        self,
        raw_path: Path,
        status_path: Path,
        *,
        hard_timeout_seconds: float = DEFAULT_HARD_TIMEOUT_SECONDS,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if hard_timeout_seconds <= 0:
            raise ValueError("hard timeout must be positive")
        self.raw_path = raw_path
        self.status_path = status_path
        self.hard_timeout_seconds = hard_timeout_seconds
        self.clock = clock
        self.state = CaptureState.PREPARED
        self.raw: Optional[RawSink] = None
        self.detector = TerminalLineDetector()
        self.start_monotonic: Optional[float] = None
        self.reset_monotonic: Optional[float] = None
        self.end_monotonic: Optional[float] = None
        self.deadline_monotonic: Optional[float] = None
        self.reset_byte_offset: Optional[int] = None
        self.reset_count = 0
        self.terminal_status = "NOT_OBSERVED"
        self.terminal_marker: Optional[str] = None
        self.exit_reason: Optional[ExitReason] = None
        self.serial_error: Optional[str] = None
        self._status_written = False

    def prepare(self) -> None:
        if self.state != CaptureState.PREPARED or self.raw is not None:
            raise CaptureStateError("capture has already been prepared")
        if self.status_path.exists():
            raise FileExistsError(f"status output already exists: {self.status_path}")
        self.raw = RawSink(self.raw_path)
        self.start_monotonic = self.clock()
        self.state = CaptureState.CAPTURING_PRE_RESET

    def feed(self, data: bytes) -> Optional[str]:
        if self.raw is None:
            raise CaptureStateError("capture is not prepared")
        if self.state in {
            CaptureState.HARD_TIMEOUT,
            CaptureState.SERIAL_ERROR,
            CaptureState.CONTROL_ERROR,
            CaptureState.INTERNAL_ERROR,
        }:
            return None
        if not data:
            return self.detector.terminal_marker
        # Persist before parsing.  A parser exception can therefore never
        # erase the bytes which caused it.
        self.raw.write(data)
        marker = self.detector.feed(data)
        if (
            marker is not None
            and self.state == CaptureState.BENCHMARK_RUNNING
            and self.terminal_marker is None
        ):
            self.terminal_marker = marker
            self.terminal_status = marker
            self.state = (
                CaptureState.TERMINAL_PASS
                if marker == "PASS"
                else CaptureState.TERMINAL_FAIL
            )
        return marker

    def issue_reset(self, reset_action: Callable[[], None]) -> None:
        if self.raw is None or self.state != CaptureState.CAPTURING_PRE_RESET:
            raise CaptureStateError("reset is allowed only once after prepare")
        if self.reset_count != 0:
            raise CaptureStateError("a second reset is forbidden")
        self.reset_byte_offset = self.raw.bytes_written
        self.reset_monotonic = self.clock()
        self.reset_count = 1
        self.state = CaptureState.RESET_ISSUED
        self.detector.reset_epoch()
        try:
            reset_action()
        except BaseException as error:
            self.serial_error = _sanitize_error(error)
            self.state = CaptureState.CONTROL_ERROR
            self.exit_reason = ExitReason.CONTROL_ERROR
            raise
        self.deadline_monotonic = self.reset_monotonic + self.hard_timeout_seconds
        self.state = CaptureState.BENCHMARK_RUNNING

    def check_hard_timeout(self) -> bool:
        if self.state not in {CaptureState.BENCHMARK_RUNNING}:
            return self.state == CaptureState.HARD_TIMEOUT
        if self.deadline_monotonic is not None and self.clock() >= self.deadline_monotonic:
            self.state = CaptureState.HARD_TIMEOUT
            self.exit_reason = ExitReason.HARD_TIMEOUT
            self.terminal_status = "NOT_OBSERVED"
            return True
        return False

    def finish(
        self,
        reason: Optional[ExitReason] = None,
        *,
        serial_error: Optional[str] = None,
    ) -> dict[str, object]:
        if self._status_written:
            return self.status()
        if self.raw is None:
            raise CaptureStateError("capture was never prepared")
        if serial_error is not None:
            self.serial_error = serial_error
        if reason is not None:
            self.exit_reason = reason
        if self.exit_reason is None:
            if self.terminal_marker == "PASS":
                self.exit_reason = ExitReason.TERMINAL_PASS
            elif self.terminal_marker == "FAIL":
                self.exit_reason = ExitReason.TERMINAL_FAIL
            else:
                self.exit_reason = ExitReason.INTERNAL_ERROR
        if self.exit_reason == ExitReason.TERMINAL_PASS:
            self.state = CaptureState.TERMINAL_PASS
            self.terminal_status = "PASS"
        elif self.exit_reason == ExitReason.TERMINAL_FAIL:
            self.state = CaptureState.TERMINAL_FAIL
            self.terminal_status = "FAIL"
        elif self.exit_reason == ExitReason.HARD_TIMEOUT:
            self.state = CaptureState.HARD_TIMEOUT
        elif self.exit_reason == ExitReason.SERIAL_ERROR:
            self.state = CaptureState.SERIAL_ERROR
        elif self.exit_reason == ExitReason.CONTROL_ERROR:
            self.state = CaptureState.CONTROL_ERROR
        else:
            self.state = CaptureState.INTERNAL_ERROR
        self.raw.sync()
        self.raw.close()
        self.end_monotonic = self.clock()
        document = self.status()
        encoded = (json.dumps(document, sort_keys=True, indent=2) + "\n").encode("utf-8")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_CLOEXEC", 0)
        fd = os.open(self.status_path, flags, 0o644)
        try:
            _write_all(fd, encoded)
            os.fdatasync(fd)
        finally:
            os.close(fd)
        self._status_written = True
        return document

    def status(self) -> dict[str, object]:
        raw_bytes = self.raw.bytes_written if self.raw is not None else 0
        raw_sha = self.raw.sha256 if self.raw is not None else hashlib.sha256(b"").hexdigest()
        return {
            "schema_version": 1,
            "raw_path": str(self.raw_path),
            "raw_bytes": raw_bytes,
            "raw_sha256": raw_sha,
            "start_monotonic": self.start_monotonic,
            "reset_monotonic": self.reset_monotonic,
            "end_monotonic": self.end_monotonic,
            "reset_byte_offset": self.reset_byte_offset,
            "reset_count": self.reset_count,
            "terminal_status": self.terminal_status,
            "terminal_marker": self.terminal_marker,
            "exit_reason": self.exit_reason.value if self.exit_reason else None,
            "state": self.state.value,
            "hard_timeout_seconds": self.hard_timeout_seconds,
            "idle_timeout_enabled": False,
            "serial_error": self.serial_error,
            "final_line_complete": self.detector.final_line_complete,
        }


def hard_reset(serial_port: object) -> None:
    """Use the existing IDF Monitor hard-reset RTS pulse (5 ms)."""
    serial_port.setRTS(True)  # type: ignore[attr-defined]
    try:
        time.sleep(RESET_LOW_SECONDS)
    finally:
        serial_port.setRTS(False)  # type: ignore[attr-defined]
        if hasattr(serial_port, "setDTR") and hasattr(serial_port, "dtr"):
            serial_port.setDTR(serial_port.dtr)  # type: ignore[attr-defined]


def _open_serial(port: str, baud: int, read_timeout: float) -> object:
    try:
        import serial  # type: ignore
    except ImportError as error:  # pragma: no cover - depends on host setup
        raise RuntimeError("pyserial is required for physical capture") from error
    serial_port = serial.serial_for_url(
        port,
        baudrate=baud,
        timeout=read_timeout,
        do_not_open=True,
        exclusive=True,
    )
    serial_port.write_timeout = 0.3
    serial_port.open()
    return serial_port


def run_serial_capture(
    *,
    port: str,
    raw_path: Path,
    status_path: Path,
    baud: int = DEFAULT_BAUD,
    hard_timeout_seconds: float = DEFAULT_HARD_TIMEOUT_SECONDS,
    read_timeout_seconds: float = DEFAULT_READ_TIMEOUT_SECONDS,
    post_terminal_drain_seconds: float = DEFAULT_POST_TERMINAL_DRAIN_SECONDS,
) -> int:
    """Run one physical capture; return zero for either observed terminal."""
    if post_terminal_drain_seconds < 0:
        raise ValueError("post-terminal drain must not be negative")
    session = CaptureSession(
        raw_path,
        status_path,
        hard_timeout_seconds=hard_timeout_seconds,
    )
    session.prepare()
    serial_port: Optional[object] = None
    signal_number: list[int] = []

    def request_stop(signum: int, _frame: object) -> None:
        signal_number.append(signum)

    old_handlers = {
        signal.SIGTERM: signal.getsignal(signal.SIGTERM),
        signal.SIGINT: signal.getsignal(signal.SIGINT),
    }
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    try:
        try:
            serial_port = _open_serial(port, baud, read_timeout_seconds)
            session.issue_reset(lambda: hard_reset(serial_port))
        except BaseException as error:
            if session.exit_reason is None:
                session.exit_reason = ExitReason.CONTROL_ERROR
                session.state = CaptureState.CONTROL_ERROR
            session.finish(
                session.exit_reason,
                serial_error=_sanitize_error(error, port),
            )
            return 2

        while session.terminal_marker is None:
            if signal_number:
                session.finish(
                    ExitReason.CONTROL_ERROR,
                    serial_error=f"interrupted by signal {signal_number[-1]}",
                )
                return 2
            if session.check_hard_timeout():
                session.finish(ExitReason.HARD_TIMEOUT)
                return 2
            try:
                data = serial_port.read(4096)  # type: ignore[attr-defined]
            except BaseException as error:
                session.finish(
                    ExitReason.SERIAL_ERROR,
                    serial_error=_sanitize_error(error, port),
                )
                return 2
            # A read may wake just after the deadline.  Do not accept bytes as
            # benchmark completion once the reset-relative safety window has
            # expired.
            if session.check_hard_timeout():
                session.finish(ExitReason.HARD_TIMEOUT)
                return 2
            if data:
                try:
                    session.feed(data)
                except BaseException as error:
                    session.finish(
                        ExitReason.INTERNAL_ERROR,
                        serial_error=_sanitize_error(error, port),
                    )
                    return 2

        drain_deadline = time.monotonic() + post_terminal_drain_seconds
        while time.monotonic() < drain_deadline:
            if signal_number:
                session.finish(
                    ExitReason.CONTROL_ERROR,
                    serial_error=f"interrupted by signal {signal_number[-1]}",
                )
                return 2
            remaining = max(0.0, drain_deadline - time.monotonic())
            if hasattr(serial_port, "timeout"):
                serial_port.timeout = min(read_timeout_seconds, remaining)  # type: ignore[attr-defined]
            try:
                data = serial_port.read(4096)  # type: ignore[attr-defined]
            except BaseException as error:
                session.finish(
                    ExitReason.SERIAL_ERROR,
                    serial_error=_sanitize_error(error, port),
                )
                return 2
            if data:
                try:
                    session.feed(data)
                except BaseException as error:
                    session.finish(
                        ExitReason.INTERNAL_ERROR,
                        serial_error=_sanitize_error(error, port),
                    )
                    return 2
        session.finish()
        return 0
    finally:
        if serial_port is not None:
            try:
                serial_port.close()  # type: ignore[attr-defined]
            except Exception:
                pass
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="machine-local serial port")
    parser.add_argument("--raw", required=True, type=Path, help="exclusive raw UART output")
    parser.add_argument("--status", required=True, type=Path, help="exclusive JSON status output")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--hard-timeout",
        type=float,
        default=DEFAULT_HARD_TIMEOUT_SECONDS,
        help="seconds from canonical reset (default: 360)",
    )
    parser.add_argument(
        "--read-timeout",
        type=float,
        default=DEFAULT_READ_TIMEOUT_SECONDS,
        help="serial read wake interval; not an idle timeout",
    )
    parser.add_argument(
        "--post-terminal-drain",
        type=float,
        default=DEFAULT_POST_TERMINAL_DRAIN_SECONDS,
        help="bounded drain after terminal line (default: 0.5)",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    if args.hard_timeout <= 0 or args.read_timeout <= 0:
        raise SystemExit("timeouts must be positive")
    return run_serial_capture(
        port=args.port,
        raw_path=args.raw,
        status_path=args.status,
        baud=args.baud,
        hard_timeout_seconds=args.hard_timeout,
        read_timeout_seconds=args.read_timeout,
        post_terminal_drain_seconds=args.post_terminal_drain,
    )


if __name__ == "__main__":
    raise SystemExit(main())
