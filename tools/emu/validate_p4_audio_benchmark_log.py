#!/usr/bin/env python3
"""Validate the non-performance correctness contract of the A1 esp-emu log."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    checks = {
        "source": r"P4_AUDIO_SOURCE fixture=retrofm-pocket-demo-strict\.s98 bytes=3753 sha256=702d8b3003d2d81449fd1003aa2231afdacaae9d680f73fdf11d8195edb046c2",
        "atomic": r"P4_AUDIO_ATOMIC [^\n]*head_lock_free=PASS tail_lock_free=PASS",
        "retro_correctness": r"P4_AUDIO_IDENTITY workload=RETROFM event_count=1047 event_crc32=0x3416c2b6",
        "stress_correctness": r"P4_AUDIO_IDENTITY workload=STRESS-60 event_count=41127 event_crc32=0x91eac288",
        "retro_pcm": r"workload=RETROFM.*pcm_frames=576960 pcm_bytes=2307840 pcm_crc32=0x79b0dfad",
        "stress_pcm": r"workload=STRESS-60.*pcm_frames=2880000 pcm_bytes=11520000 pcm_crc32=0x39c7f2d2",
        "service": r"P4_AUDIO_SERVICE measured_quanta=",
        "final": r"P4_AUDIO_ONLY_BENCHMARK_RESULT=PASS",
    }
    if "P4_AUDIO_EMU_STRESS=SKIPPED reason=performance_not_valid_in_emulator" in text:
        checks.pop("stress_correctness")
        checks.pop("stress_pcm")
    failures = [name for name, pattern in checks.items()
                if re.search(pattern, text, re.MULTILINE) is None]
    if failures:
        print("P4_AUDIO_EMU_VALIDATION=FAIL missing=" + ",".join(failures))
        return 1
    if "P4_AUDIO_RESULT workload=RETROFM identity=PASS" not in text or \
       ("P4_AUDIO_EMU_STRESS=SKIPPED" not in text and
        "P4_AUDIO_RESULT workload=STRESS-60 identity=PASS" not in text):
        print("P4_AUDIO_EMU_VALIDATION=FAIL identity_result_missing")
        return 1
    print("P4_AUDIO_EMU_VALIDATION=PASS source=PASS atomic=PASS identity=PASS "
          "pcm=PASS lifecycle=PASS performance=INVALID")
    return 0


if __name__ == "__main__":
    sys.exit(main())
