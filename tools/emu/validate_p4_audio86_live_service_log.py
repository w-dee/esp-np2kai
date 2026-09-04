#!/usr/bin/env python3
"""Validate the focused Audio86 live-service ESP-EMU lifecycle evidence."""

from __future__ import annotations

import argparse
from pathlib import Path


def validate(text: str) -> None:
    required = (
        "P4_AUDIO86_LIVE_SERVICE owner_core=1 worker_core=0 "
        "final_horizon=13 accepted_frames=13 slots=1 partial=1",
        "P4_AUDIO86_LIVE_SERVICE_RESIDUAL guest=0 sink=0 ownership=0",
        "P4_AUDIO86_LIVE_SERVICE_RESULT=PASS",
        "P4_NANO_AUDIO86_LIVE_SERVICE_STATUS=PASS",
    )
    for marker in required:
        if marker not in text:
            raise ValueError(f"missing live-service marker: {marker}")
    forbidden = ("assert failed", "Guru Meditation", "Task watchdog", "=FAIL")
    for marker in forbidden:
        if marker in text:
            raise ValueError(f"forbidden live-service marker: {marker}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    validate(args.log.read_text(encoding="utf-8", errors="replace"))
    print("P4_AUDIO86_LIVE_SERVICE_ESP_EMU=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
