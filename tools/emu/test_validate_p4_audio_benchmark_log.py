#!/usr/bin/env python3
"""Focused unit tests for the A1 run-configuration validator."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio_benchmark_log.py"
SPEC = importlib.util.spec_from_file_location("validate_p4_audio_benchmark_log", VALIDATOR)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def metadata(environment: str, cpu_frequency_hz: int,
             audio_optimization: str = "o2") -> str:
    return (
        "P4_AUDIO_META environment=" + environment
        + " chip_target=esp32p4 chip_revision=1 idf=v5.5.4"
        + f" cpu_frequency_hz={cpu_frequency_hz} global_optimization=debug"
        + f" audio_optimization={audio_optimization}"
        + " capacity_housekeeping=enabled housekeeping_quantum_interval=64"
        + " housekeeping_delay_ticks=1 producer_full_wait=notification"
    )


def require_rejected(environment: str, cpu_frequency_hz: int) -> None:
    diagnostic = MODULE.validate_configuration(metadata(environment, cpu_frequency_hz))
    assert diagnostic is not None, (environment, cpu_frequency_hz)


def main() -> int:
    assert MODULE.validate_configuration(metadata("REAL_P4", 360_000_000)) is None
    assert MODULE.validate_configuration(metadata("ESP_EMU", 400_000_000)) is None
    require_rejected("REAL_P4", 360)
    require_rejected("ESP_EMU", 400)
    require_rejected("REAL_P4", 400_000_000)
    require_rejected("ESP_EMU", 360_000_000)
    assert MODULE.validate_configuration(metadata("ESP_EMU", 400_000_000, "debug")) is None
    assert MODULE.validate_configuration(metadata("ESP_EMU", 400_000_000).replace(
        "housekeeping_delay_ticks=1", "housekeeping_delay_ticks=2")) is not None
    assert MODULE.validate_configuration(metadata("ESP_EMU", 400_000_000).replace(
        "global_optimization=debug", "optimization=release-equivalent")) is not None
    print("P4_AUDIO_VALIDATOR_CONFIGURATION_TEST=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
