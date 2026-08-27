#!/usr/bin/env python3
"""Validate the non-performance correctness contract of the A1 esp-emu log."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXPECTED_CPU_FREQUENCY_HZ = {
    "REAL_P4": 360_000_000,
    "ESP_EMU": 400_000_000,
}

META_PATTERN = re.compile(
    r"^P4_AUDIO_META environment=(?P<environment>ESP_EMU|REAL_P4) "
    r"[^\n]*? cpu_frequency_hz=(?P<cpu_frequency_hz>[0-9]+) "
    r"global_optimization=(?P<global_optimization>debug) "
    r"audio_optimization=(?P<audio_optimization>debug|o2) "
    r"capacity_housekeeping=enabled "
    r"housekeeping_quantum_interval=(?P<housekeeping_quantum_interval>[0-9]+) "
    r"housekeeping_delay_ticks=(?P<housekeeping_delay_ticks>[0-9]+) "
    r"producer_full_wait=notification",
    re.MULTILINE,
)


def validate_configuration(text: str) -> str | None:
    """Return a diagnostic when the run metadata has an invalid CPU unit/value."""

    matches = list(META_PATTERN.finditer(text))
    if len(matches) != 1:
        return f"metadata_count={len(matches)}"
    match = matches[0]
    environment = match.group("environment")
    cpu_frequency_hz = int(match.group("cpu_frequency_hz"))
    expected = EXPECTED_CPU_FREQUENCY_HZ[environment]
    if cpu_frequency_hz != expected:
        return (
            f"environment={environment} cpu_frequency_hz={cpu_frequency_hz}"
            f" expected={expected}"
        )
    if match.group("housekeeping_quantum_interval") != "64" or \
       match.group("housekeeping_delay_ticks") != "1":
        return (
            "housekeeping_quantum_interval=" +
            match.group("housekeeping_quantum_interval") +
            " housekeeping_delay_ticks=" +
            match.group("housekeeping_delay_ticks") +
            " expected=64,1"
        )
    return None


def validate_text(text: str) -> tuple[list[str], str | None]:
    """Return missing contract checks and an optional configuration diagnostic."""

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
    return failures, validate_configuration(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    failures, configuration_failure = validate_text(text)
    if configuration_failure is not None:
        failures.append("configuration")
    if failures:
        detail = ""
        if configuration_failure is not None:
            detail = " configuration_detail=" + configuration_failure.replace(" ", "_")
        print("P4_AUDIO_EMU_VALIDATION=FAIL missing=" + ",".join(failures) + detail)
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
