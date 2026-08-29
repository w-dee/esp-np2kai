#!/usr/bin/env python3
"""Validate the compact native 86H.2 synchronous fixture identity."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CRC32_RE = re.compile(r"^0x[0-9a-f]{8}$")


def fail(message: str) -> None:
    raise SystemExit(f"AUDIO86_GOLDEN_VALIDATION=FAIL {message}")


def parse_kv(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            fail(f"malformed token {token!r}")
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def require_fields(actual: dict[str, str], expected: dict[str, str], name: str) -> None:
    for key, value in expected.items():
        if actual.get(key) != value:
            fail(f"{name}.{key}: {actual.get(key)!r} != {value!r}")


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

    markers: dict[str, str] = {}
    for line in lines:
        if line.startswith("AUDIO86_SYNC_"):
            token = line.split()[0]
            markers[token.split("=", 1)[0]] = line
    required = {
        "AUDIO86_SYNC_CONFIG",
        "AUDIO86_SYNC_LOAD",
        "AUDIO86_SYNC_CONTROL_IDENTITY",
        "AUDIO86_SYNC_PCM86_SOURCE_IDENTITY",
        "AUDIO86_SYNC_PCM86_FEED",
        "AUDIO86_SYNC_ACTIVITY",
        "AUDIO86_SYNC_PCM_IDENTITY",
        "AUDIO86_SYNC_AMPLITUDE",
        "AUDIO86_SYNC_SAFETY",
        "AUDIO86_SYNC_SAME_PROCESS_RESET",
        "AUDIO86_SYNC_GOLDEN",
    }
    if required - markers.keys():
        fail(f"missing markers {sorted(required - markers.keys())}")

    fixture = golden["fixture"]
    require_fields(
        parse_kv(markers["AUDIO86_SYNC_CONFIG"]),
        {"rate": str(fixture["rate_hz"]),
         "quantum_frames": str(fixture["quantum_frames"]),
         "duration_frames": str(fixture["duration_frames"]),
         "quanta": str(fixture["quanta"])},
        "config",
    )
    load = golden["load"]
    require_fields(
        parse_kv(markers["AUDIO86_SYNC_LOAD"]),
        {
            "fm_channels": str(load["fm_channels"]),
            "fm_operators": str(load["fm_operators"]),
            "psg_channels": str(load["psg_channels"]),
            "psg_noise": "1" if load["psg_noise"] else "0",
            "psg_envelope": "1" if load["psg_envelope"] else "0",
            "rhythm_tracks": str(load["rhythm_tracks"]),
            "pcm86_source_rate": str(load["pcm86_source_rate_hz"]),
            "pcm86_channels": str(load["pcm86_channels"]),
            "pcm86_bits": str(load["pcm86_bits"]),
            "pcm86_dactrl": load["pcm86_dactrl"],
        },
        "load",
    )
    control = parse_kv(markers["AUDIO86_SYNC_CONTROL_IDENTITY"])
    require_fields(control, {"events": str(golden["control"]["events"]),
                             "mid_quantum_events": str(golden["control"]["mid_quantum_events"]),
                             "crc32": golden["control"]["crc32"],
                             "sha256": golden["control"]["sha256"]}, "control")
    source = parse_kv(markers["AUDIO86_SYNC_PCM86_SOURCE_IDENTITY"])
    require_fields(source, {"period_frames": str(golden["pcm86_source"]["period_frames"]),
                            "bytes": str(golden["pcm86_source"]["period_bytes"]),
                            "crc32": golden["pcm86_source"]["crc32"],
                            "sha256": golden["pcm86_source"]["sha256"]}, "source")
    feed = parse_kv(markers["AUDIO86_SYNC_PCM86_FEED"])
    feed_golden = golden["pcm86_feed"]
    require_fields(feed, {"supplied": str(feed_golden["bytes_supplied"]),
                          "consumed": str(feed_golden["bytes_consumed"]),
                          "refills": str(feed_golden["refills"]),
                          "fifo_min": str(feed_golden["fifo_min"]),
                          "fifo_max": str(feed_golden["fifo_max"])}, "feed")
    activity = parse_kv(markers["AUDIO86_SYNC_ACTIVITY"])
    require_fields(activity, golden["activity"], "activity")
    pcm = parse_kv(markers["AUDIO86_SYNC_PCM_IDENTITY"])
    require_fields(pcm, {key: str(value) for key, value in golden["pcm"].items()}, "pcm")
    amplitude = parse_kv(markers["AUDIO86_SYNC_AMPLITUDE"])
    require_fields(amplitude, {key: str(value) for key, value in golden["amplitude"].items()}, "amplitude")
    safety = parse_kv(markers["AUDIO86_SYNC_SAFETY"])
    require_fields(safety, {"pcm86_fifo_underrun": str(golden["pcm86_feed"]["underrun"]),
                            "sequence_error": str(golden["safety"]["sequence_error"]),
                            "arithmetic_error": str(golden["safety"]["arithmetic_error"])},
                   "safety")
    if markers["AUDIO86_SYNC_SAME_PROCESS_RESET"] != "AUDIO86_SYNC_SAME_PROCESS_RESET=PASS":
        fail("same-process reset")
    if markers["AUDIO86_SYNC_GOLDEN"] != "AUDIO86_SYNC_GOLDEN=PASS":
        fail("frozen golden self-check")

    for name, fields in (("control", control), ("source", source), ("pcm", pcm)):
        if not CRC32_RE.fullmatch(fields["crc32"]):
            fail(f"{name} CRC32 syntax")
        if not SHA256_RE.fullmatch(fields["sha256"]):
            fail(f"{name} SHA-256 syntax")
    print("AUDIO86_GOLDEN_VALIDATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
