#!/usr/bin/env python3
"""Fail-closed validator for the native 86H.3 asynchronous proof."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CRC32_RE = re.compile(r"^[0-9a-f]{8}$")
MODES = {
    "PRODUCER_FAST_WORKER_YIELD",
    "PRODUCER_YIELD_WORKER_FAST",
    "DETERMINISTIC_ALTERNATING_YIELDS",
    "BYTE_TRANSPORT_PRESSURE",
}


def fail(message: str) -> None:
    raise SystemExit(f"AUDIO86_ASYNC_VALIDATION=FAIL {message}")


def one_line(lines: list[str], prefix: str) -> str:
    matches = [line for line in lines if line.startswith(prefix)]
    if len(matches) != 1:
        fail(f"{prefix}: expected one marker, found {len(matches)}")
    return matches[0]


def fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            fail(f"malformed token {token!r}")
        key, value = token.split("=", 1)
        if key in result:
            fail(f"duplicate field {key!r}")
        result[key] = value
    return result


def require(values: dict[str, str], key: str, expected: str, name: str) -> None:
    actual = values.get(key)
    if actual != expected:
        fail(f"{name}.{key}: {actual!r} != {expected!r}")


def require_uint(values: dict[str, str], key: str, name: str) -> int:
    try:
        value = int(values[key], 10)
    except (KeyError, ValueError) as exc:
        fail(f"{name}.{key}: invalid unsigned integer")
        raise AssertionError from exc
    if value < 0:
        fail(f"{name}.{key}: negative value")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--golden", required=True, type=Path)
    args = parser.parse_args()

    try:
        golden = json.loads(args.golden.read_text(encoding="utf-8"))
        lines = args.log.read_text(encoding="utf-8").splitlines()
    except (OSError, json.JSONDecodeError) as exc:
        fail(str(exc))

    required = {
        "AUDIO86_ASYNC_RING_TEST",
        "AUDIO86_ASYNC_CONFIG",
        "AUDIO86_ASYNC_ORACLE",
        "AUDIO86_ASYNC_TRANSPORT",
        "AUDIO86_ASYNC_PCM86",
        "AUDIO86_ASYNC_CROSS_MODE_EXACTNESS=PASS",
        "AUDIO86_ASYNC_RESULT=PASS",
    }
    for marker in required:
        if marker.endswith("=PASS"):
            if lines.count(marker) != 1:
                fail(f"required marker {marker!r} count={lines.count(marker)}")
        elif not any(line.startswith(marker + " ") for line in lines):
            fail(f"missing marker {marker!r}")
    if any("=FAIL" in line for line in lines):
        fail("explicit FAIL marker")

    ring = fields(one_line(lines, "AUDIO86_ASYNC_RING_TEST "))
    require(ring, "event_ring", "PASS", "ring")
    require(ring, "byte_ring", "PASS", "ring")

    config = fields(one_line(lines, "AUDIO86_ASYNC_CONFIG "))
    for key, expected in {
        "modes": "4",
        "lifecycles_per_mode": "2",
        "event_capacity": "128",
        "byte_capacity": "65536",
        "max_data_run": "32768",
    }.items():
        require(config, key, expected, "config")

    fixture = golden["fixture"]
    oracle = fields(one_line(lines, "AUDIO86_ASYNC_ORACLE "))
    require(oracle, "pcm_crc32", golden["pcm"]["crc32"][2:], "oracle")
    require(oracle, "pcm_sha256", golden["pcm"]["sha256"], "oracle")
    for key in ("pcm_sha256",):
        if not SHA256_RE.fullmatch(oracle[key]):
            fail(f"oracle.{key}: invalid SHA-256 syntax")
    if not CRC32_RE.fullmatch(oracle["pcm_crc32"]):
        fail("oracle.pcm_crc32: invalid CRC32 syntax")

    transport = fields(one_line(lines, "AUDIO86_ASYNC_TRANSPORT "))
    require(transport, "events", "333", "transport")
    if not CRC32_RE.fullmatch(transport.get("crc32", "")):
        fail("transport.crc32: invalid CRC32 syntax")
    if not SHA256_RE.fullmatch(transport.get("sha256", "")):
        fail("transport.sha256: invalid SHA-256 syntax")

    feed = golden["pcm86_feed"]
    pcm86 = fields(one_line(lines, "AUDIO86_ASYNC_PCM86 "))
    for key, expected in {
        "data_runs": "323",
        "supplied": str(feed["bytes_supplied"]),
        "consumed": str(feed["bytes_consumed"]),
        "fifo_min": str(feed["fifo_min"]),
        "fifo_max": str(feed["fifo_max"]),
        "underrun": str(feed["underrun"]),
    }.items():
        require(pcm86, key, expected, "pcm86")

    mode_lines = [line for line in lines if line.startswith("AUDIO86_ASYNC_MODE ")]
    if len(mode_lines) != 8:
        fail(f"mode marker count={len(mode_lines)}")
    seen: set[tuple[str, str]] = set()
    for line in mode_lines:
        values = fields(line)
        mode = values.get("mode", "")
        lifecycle = values.get("lifecycle", "")
        if mode not in MODES or lifecycle not in {"A", "B"}:
            fail(f"invalid mode/lifecycle {mode!r}/{lifecycle!r}")
        key = (mode, lifecycle)
        if key in seen:
            fail(f"duplicate mode/lifecycle {key!r}")
        seen.add(key)
        require(values, "pass", "1", f"mode.{mode}.{lifecycle}")
        require(values, "first_error", "NONE", f"mode.{mode}.{lifecycle}")
        require_uint(values, "event_full_wait", f"mode.{mode}.{lifecycle}")
        byte_wait = require_uint(values, "byte_full_wait", f"mode.{mode}.{lifecycle}")
        require_uint(values, "event_high_water", f"mode.{mode}.{lifecycle}")
        require_uint(values, "byte_high_water", f"mode.{mode}.{lifecycle}")
        if mode == "BYTE_TRANSPORT_PRESSURE" and byte_wait == 0:
            fail("BYTE_TRANSPORT_PRESSURE must exercise byte-ring backpressure")
    if seen != {(mode, lifecycle) for mode in MODES for lifecycle in {"A", "B"}}:
        fail("incomplete mode/lifecycle matrix")

    # Keep geometry tied to the frozen synchronous oracle rather than a new
    # async golden.  These are mechanical comparisons against the JSON.
    if fixture["rate_hz"] != 48000 or fixture["quantum_frames"] != 240:
        fail("unexpected frozen geometry")

    print("AUDIO86_ASYNC_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
