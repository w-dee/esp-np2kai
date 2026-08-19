#!/usr/bin/env python3
"""Synchronize with and smoke-test a physical ESP-NP2 UART control stream.

The tool intentionally does not wait for the firmware READY diagnostic line and
does not flush the serial input. It scans received bytes for only complete,
matching JSON protocol responses, so boot logs and arbitrary UART garbage are
ignored.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - depends on the host environment
    raise SystemExit("pyserial is required; use the ESP-IDF Python environment") from exc


FRAME_PREFIX = b"@ESP-NP2 "
TRANSPORT_SYNC = b"\x00\x00\x00\x00"
DEFAULT_PORT = "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61041224-if00"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port",
        default=os.environ.get("ESP32_P4_NANO_PORT", DEFAULT_PORT),
        help="serial device (default: %(default)s)",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--trials", type=int, default=1,
                        help="successful sync/hello trials to perform")
    parser.add_argument("--retries", type=int, default=3,
                        help="sync+hello attempts per trial")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="response timeout in seconds")
    parser.add_argument("--garbage", action="store_true",
                        help="send bounded pre-sync garbage before each trial")
    return parser.parse_args()


def request(request_id: int, command: str) -> bytes:
    payload = json.dumps(
        {"v": 1, "id": request_id, "cmd": command},
        separators=(",", ":"),
    ).encode("ascii")
    return FRAME_PREFIX + payload + b"\n"


def bounded_garbage() -> bytes:
    # Separators keep the one-, two-, and three-zero runs distinct from the
    # four-zero synchronization token that follows.
    return b"\xffboot-junk\n@ESP-NP\x00X\x00\x00Y\x00\x00\x00Z"


def response_is_valid(response: object, request_id: int) -> bool:
    if not isinstance(response, dict):
        return False
    return (
        response.get("type") == "response"
        and response.get("v") == 1
        and response.get("id") == request_id
        and response.get("ok") is True
    )


class SerialProtocol:
    def __init__(self, port: str, baud: int, timeout: float) -> None:
        self.serial = serial.Serial(port, baudrate=baud, timeout=0.05,
                                    write_timeout=timeout)
        self.timeout = timeout
        self.rx_buffer = bytearray()
        self.tx_history = bytearray()
        self.rx_history = bytearray()

    def close(self) -> None:
        self.serial.close()

    def send(self, data: bytes) -> None:
        self.tx_history.extend(data)
        self.serial.write(data)

    def _read_lines(self) -> list[dict]:
        responses: list[dict] = []
        while True:
            line_end = self.rx_buffer.find(b"\n")
            if line_end < 0:
                if len(self.rx_buffer) > 8192:
                    del self.rx_buffer[:-4096]
                break
            line = bytes(self.rx_buffer[:line_end]).rstrip(b"\r")
            del self.rx_buffer[: line_end + 1]
            prefix_index = line.find(FRAME_PREFIX)
            if prefix_index < 0:
                continue
            try:
                decoded = json.loads(
                    line[prefix_index + len(FRAME_PREFIX):].decode("utf-8")
                )
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(decoded, dict):
                responses.append(decoded)
        return responses

    def wait_response(self, request_id: int) -> dict:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            chunk = self.serial.read(4096)
            if chunk:
                self.rx_history.extend(chunk)
                self.rx_buffer.extend(chunk)
                for response in self._read_lines():
                    if response_is_valid(response, request_id):
                        return response
            else:
                time.sleep(0.005)
        raise TimeoutError(f"timed out waiting for response id {request_id}")

    def dump_failure(self) -> None:
        print("TX bytes (hex):", bytes(self.tx_history).hex(), file=sys.stderr)
        print("RX bytes (hex):", bytes(self.rx_history).hex(), file=sys.stderr)
        print("RX bytes (escaped):", repr(bytes(self.rx_history)), file=sys.stderr)


def require_result(response: dict, request_id: int, command: str) -> dict:
    if not response_is_valid(response, request_id):
        raise AssertionError(f"{command} returned an invalid response: {response!r}")
    result = response.get("result")
    if not isinstance(result, dict):
        raise AssertionError(f"{command} response has no object result: {response!r}")
    return result


def main() -> int:
    args = parse_args()
    if args.trials < 1 or args.retries < 1 or args.timeout <= 0:
        raise AssertionError("trials, retries, and timeout must be positive")

    protocol: SerialProtocol | None = None
    try:
        protocol = SerialProtocol(args.port, args.baud, args.timeout)
        request_seed = int(time.time() * 1000) % 1_900_000_000
        first_attempt_successes = 0
        for trial in range(args.trials):
            hello_response: dict | None = None
            for retry in range(args.retries):
                request_id = (request_seed + trial * args.retries + retry) % 2_000_000_000
                if args.garbage:
                    protocol.send(bounded_garbage())
                protocol.send(TRANSPORT_SYNC)
                protocol.send(request(request_id, "protocol.hello"))
                try:
                    hello_response = protocol.wait_response(request_id)
                    if retry == 0:
                        first_attempt_successes += 1
                    break
                except TimeoutError as exc:
                    if retry + 1 == args.retries:
                        print(f"ERROR: {exc}", file=sys.stderr)
                        protocol.dump_failure()
                        return 1
            assert hello_response is not None
            hello = require_result(hello_response, request_id, "protocol.hello")
            if hello.get("protocol_version") != 1:
                raise AssertionError(f"unexpected protocol.hello result: {hello!r}")

            ping_id = (request_seed + 100_000 + trial) % 2_000_000_000
            protocol.send(request(ping_id, "system.ping"))
            ping = require_result(protocol.wait_response(ping_id), ping_id, "system.ping")
            if ping != {"pong": True}:
                raise AssertionError(f"unexpected system.ping result: {ping!r}")

            info_id = (request_seed + 200_000 + trial) % 2_000_000_000
            protocol.send(request(info_id, "system.info"))
            info = require_result(protocol.wait_response(info_id), info_id, "system.info")
            for key in ("project", "idf_version", "target"):
                if not isinstance(info.get(key), str) or not info[key]:
                    raise AssertionError(f"system.info lacks {key}: {info!r}")
            print(f"PASS: trial {trial + 1}/{args.trials} TRANSPORT_SYNC + hello + ping + info")
        print(f"SUMMARY: first-attempt sync/hello success = "
              f"{first_attempt_successes} / {args.trials}")
    except (AssertionError, OSError, serial.SerialException, TimeoutError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        if protocol is not None:
            protocol.dump_failure()
        return 1
    finally:
        if protocol is not None:
            protocol.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
