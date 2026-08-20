#!/usr/bin/env python3
"""Human-controlled cold-boot monitor for the P4-NANO SDMMC diagnostic."""

from __future__ import annotations

import argparse
import sys
import tempfile
import time
from pathlib import Path

import serial
from serial.serialutil import SerialException


BOOT_MARKER = b"ESP-ROM:esp32p4"
REQUIRED_MARKERS = (
    b"rst:0x1 (POWERON)",
    b"chip revision: v1.3",
    b"P4-NANO SD POWER CONFIG: PASS",
    b"P4-NANO SD HOST INIT: PASS",
    b"P4-NANO SD CARD INIT: PASS",
    b"P4-NANO SD CARD INFO: PASS",
    b"SD card: type=SD capacity=8068792320 bytes sector=512 bus_width=4 real_freq=20000 kHz",
    b"P4-NANO FAT MOUNT: PASS",
    b"P4-NANO FAT ROOT LIST: PASS",
    b"P4-NANO README READ: PASS",
    b"P4-NANO SD READ-ONLY RESULT: PASS",
    b"P4-NANO SD WRITE COMMAND READY",
    b"P4-NANO SAFE-OFF LED ENABLED",
    b"P4-NANO SAFE TO POWER OFF: YES",
)


class BootCapture:
    """Capture one boot and reject duplicate or incomplete boot evidence."""

    def __init__(self) -> None:
        self.rx = bytearray()
        self.result: str | None = None
        self.reason: str | None = None

    def feed(self, chunk: bytes) -> None:
        if self.result is not None:
            return
        self.rx.extend(chunk)
        if self.rx.count(BOOT_MARKER) > 1:
            self.result = "ABORTED BY DUPLICATE BOOT"
            self.reason = "more than one boot marker in one power-cycle capture"
            return
        if all(marker in self.rx for marker in REQUIRED_MARKERS):
            if b"P4SDTEST.BIN" in self.rx:
                self.result = "FAIL"
                self.reason = "P4SDTEST.BIN appeared in the FAT root listing"
            else:
                self.result = "PASS"


def open_serial_without_flow_control(port: str) -> serial.Serial:
    connection = serial.Serial(
        port=None,
        baudrate=115200,
        timeout=0.1,
        dsrdtr=False,
        rtscts=False,
    )
    connection.port = port
    # Request inactive modem-control states before opening. This helper never
    # intentionally toggles DTR/RTS and does not send any UART bytes.
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def wait_for_path(path: Path, present: bool, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() == present:
            return True
        time.sleep(0.1)
    return False


def wait_for_power_off(connection: serial.Serial, path: Path, timeout: float) -> bool:
    """Drain the current boot without counting it, then wait for USB loss."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not path.exists():
            return True
        try:
            connection.read(256)
        except SerialException:
            # pyserial can report a transient transport error while the
            # by-id symlink still exists.  Keep waiting for actual device
            # disappearance so reconnect handling remains deterministic.
            time.sleep(0.1)
    return False


def capture_boot(connection: serial.Serial, path: Path, timeout: float) -> BootCapture:
    capture = BootCapture()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and capture.result is None:
        if not path.exists():
            capture.result = "USB DISCONNECTED DURING BOOT"
            capture.reason = "serial device disappeared before required markers"
            break
        try:
            chunk = connection.read(256)
        except SerialException:
            capture.result = "USB DISCONNECTED DURING BOOT"
            capture.reason = "serial device disappeared before required markers"
            break
        if chunk:
            capture.feed(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    if capture.result is None:
        capture.result = "TIMEOUT"
        capture.reason = "complete boot markers were not observed"
    return capture


def run_self_test() -> None:
    full = BOOT_MARKER + b"\n" + b"\n".join(REQUIRED_MARKERS)

    capture = BootCapture()
    for offset in range(0, len(full), 11):
        capture.feed(full[offset:offset + 11])
    assert capture.result == "PASS", "partial/chunked complete boot was rejected"

    capture = BootCapture()
    capture.feed(full[: len(full) // 2])
    assert capture.result is None, "partial boot was counted"

    capture = BootCapture()
    capture.feed(full + BOOT_MARKER)
    assert capture.result == "ABORTED BY DUPLICATE BOOT", "duplicate boot was counted"

    with tempfile.TemporaryDirectory() as temporary:
        fake_path = Path(temporary) / "serial"
        assert not wait_for_path(fake_path, True, 0.05), "missing device appeared"
        fake_path.touch()
        assert wait_for_path(fake_path, True, 0.1), "device reappearance was not detected"
        fake_path.unlink()
        assert wait_for_path(fake_path, False, 0.1), "device disappearance was not detected"

    print("cold_boot_monitor.py self-test: PASS")


def run_monitor(port: str, boots: int, completed: int) -> int:
    path = Path(port)
    if not wait_for_path(path, True, 30.0):
        print("HOST CAPTURE FAILED: serial path did not appear", flush=True)
        return 1

    try:
        current = open_serial_without_flow_control(port)
    except SerialException as error:
        print(f"HOST CAPTURE FAILED: initial serial open: {error}", flush=True)
        return 1

    print("COLD-BOOT MONITOR READY", flush=True)
    print("Current boot is not counted. Power OFF the P4-NANO now.", flush=True)
    if not wait_for_power_off(current, path, 300.0):
        current.close()
        print("HOST CAPTURE FAILED: initial power-off was not observed", flush=True)
        return 1
    current.close()

    confirmed = completed
    while confirmed < boots:
        print(f"Waiting for serial device for cold boot #{confirmed + 1}...", flush=True)
        if not wait_for_path(path, True, 300.0):
            print("HOST CAPTURE FAILED: serial path did not reappear", flush=True)
            return 1
        try:
            connection = open_serial_without_flow_control(port)
        except SerialException as error:
            print(f"HOST CAPTURE FAILED: serial open: {error}", flush=True)
            return 1

        capture = capture_boot(connection, path, 30.0)
        if capture.result != "PASS":
            connection.close()
            print(f"COLD BOOT #{confirmed + 1}: {capture.result}", flush=True)
            print(f"HOST REASON: {capture.reason}", flush=True)
            return 1

        confirmed += 1
        print(f"COLD BOOT #{confirmed} CONFIRMED", flush=True)
        print("SAFE TO POWER OFF NOW", flush=True)
        if not wait_for_power_off(connection, path, 300.0):
            connection.close()
            print("HOST CAPTURE FAILED: expected USB disappearance was not observed", flush=True)
            return 1
        connection.close()

    print(f"cold-boot read-only = {confirmed} / {boots} PASS", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--port")
    parser.add_argument(
        "--completed",
        type=int,
        default=0,
        help="number of cold boots already confirmed in an earlier monitor run",
    )
    args = parser.parse_args()
    if args.self_test:
        run_self_test()
        return 0
    if not args.port:
        parser.error("--port is required for hardware mode")
    if not 0 <= args.completed <= 5:
        parser.error("--completed must be between 0 and 5")
    return run_monitor(args.port, boots=5, completed=args.completed)


if __name__ == "__main__":
    raise SystemExit(main())
