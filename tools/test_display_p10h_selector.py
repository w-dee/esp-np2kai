#!/usr/bin/env python3
"""Host/static contract for the P10H P10F display-refresh selector."""

from __future__ import annotations

import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "tools/emu/build-production.sh"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
DISPLAY = ROOT / "firmware/components/p4_nano_display/p4_nano_display.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
TIMING = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_timing_profiles.hpp"
RUNBOOK = ROOT / "docs/development/codex-runbook.md"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_cli_failure(*args: str) -> None:
    result = subprocess.run(
        ["bash", str(BUILD), *args], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    require(result.returncode == 2,
            f"expected CLI rejection for {' '.join(args)}: {result.stdout}")


def main() -> int:
    build = BUILD.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    display = DISPLAY.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    timing = TIMING.read_text(encoding="utf-8")
    runbook = RUNBOOK.read_text(encoding="utf-8")

    for fragment in (
        "--benchmark-display-refresh baseline|lower2",
        "benchmark_display_refresh_requested",
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE=${benchmark_display_refresh_profile}",
        "build-${board}-${variant}-exact2x-internal-source-refresh-${benchmark_display_refresh_profile}",
        "requires --exact2x-internal-source-benchmark",
    ):
        require(fragment in build, f"missing selector build contract: {fragment}")

    for fragment in (
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE_ACTIVE",
        "must be baseline or lower2",
        "requires the P10F internal-source benchmark",
        "P4-NANO refresh visual profile is mutually exclusive with other profiles",
    ):
        require(fragment in main_cmake,
                f"missing selector CMake isolation: {fragment}")

    for fragment in (
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE_SELECTED",
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE=1",
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_BASELINE_PROFILE=1",
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_LOWER2_PROFILE=1",
    ):
        require(fragment in display_cmake,
                f"missing display compile selector: {fragment}")

    for fragment in (
        '"baseline", "PLL_F240M"', '80.0F', '3U', '68.662455F', '1500.0F',
        '"lower1", "PLL_F240M"', '48.0F', '5U', '41.197473F', '700.0F',
        '"lower2", "PLL_F240M"', '240.0F / 7.0F', '7U', '29.426767F',
        '500.0F', 'htotal == 880U', 'vtotal == 1324U',
    ):
        require(fragment in timing, f"missing shared timing profile value: {fragment}")

    for fragment in (
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_BASELINE_PROFILE",
        "P4_NANO_BENCHMARK_DISPLAY_REFRESH_LOWER2_PROFILE",
        "P4_NANO_BENCHMARK_DISPLAY_CONFIG profile=legacy-default",
        "P4_NANO_BENCHMARK_DISPLAY_CONFIG profile=%s",
        "print_benchmark_display_config",
    ):
        require(fragment in display,
                f"missing display configuration evidence: {fragment}")
    require("print_benchmark_display_config();" in live,
            "P10F benchmark does not emit configuration before measurement")

    require("P4_NANO_REFRESH_VISUAL_PROFILE" in display and
            "P4_NANO_BENCHMARK_DISPLAY_REFRESH_PROFILE" in display,
            "display timing priority branches are missing")

    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--benchmark-display-refresh", "baseline")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--exact2x-internal-source-benchmark",
                       "--benchmark-display-refresh", "lower1")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--exact2x-internal-source-benchmark",
                       "--benchmark-display-refresh", "bogus")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--exact2x-internal-source-benchmark",
                       "--benchmark-display-refresh", "")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--real-runtime", "--benchmark-display-refresh", "lower2")
    expect_cli_failure("--variant", "p4-v1x", "--board", "p4-nano",
                       "--display-refresh-visual", "lower2",
                       "--exact2x-internal-source-benchmark",
                       "--benchmark-display-refresh", "lower2")

    runbook_flat = " ".join(runbook.replace("\\\n", " ").split())
    for profile in ("baseline", "lower2"):
        require(
            f"--exact2x-internal-source-benchmark --benchmark-display-refresh {profile}"
            in runbook_flat,
            f"runbook is missing P10H {profile} command")

    print("Display Performance P10H P10F refresh selector host/static contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
