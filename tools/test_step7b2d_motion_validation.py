#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d automated live motion profile."""

from __future__ import annotations

import hashlib
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(text: str, fragment: str, description: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {description}: {fragment}")


def main() -> int:
    build = (ROOT / "tools/emu/build-production.sh").read_text(encoding="utf-8")
    main_cmake = (ROOT / "firmware/main/CMakeLists.txt").read_text(encoding="utf-8")
    live_cmake = (ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt").read_text(encoding="utf-8")
    runner_cmake = (ROOT / "firmware/components/np2video_runner/CMakeLists.txt").read_text(encoding="utf-8")
    runner = (ROOT / "firmware/components/np2video_runner/np2video_runner.c").read_text(encoding="utf-8")
    live = (ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp").read_text(encoding="utf-8")
    oracle_h = (ROOT / "host/compat/np2video_motion_oracle.h").read_text(encoding="utf-8")
    oracle_c = (ROOT / "host/compat/np2video_motion_oracle.c").read_text(encoding="utf-8")

    require(build, "--live-display-motion-validation", "profile selector")
    require(build, "requires --variant p4-v1x --board p4-nano", "P4-NANO restriction")
    require(build, "cannot be combined with --esp-emu-test", "esp-emu rejection")
    require(build, "NP2VIDEO_CONTINUOUS_PROFILE=", "continuous profile definition")
    require(build, "NP2VIDEO_BENCHMARK_PROFILE=", "benchmark profile definition")
    require(main_cmake, "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE_ACTIVE", "CMake profile")
    require(main_cmake, "motion validation and benchmark profiles are mutually exclusive", "mutual exclusion")
    require(main_cmake, "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE=1", "main definition")
    require(live_cmake, "np2video_motion_oracle.c", "shared oracle firmware source")
    require(live_cmake, "P4_NANO_LIVE_DISPLAY_MOTION_VALIDATION_PROFILE=1", "live definition")
    require(runner_cmake, "NP2VIDEO_CONTINUOUS_PROFILE=1", "runner continuous definition")
    require(runner, "#if defined(NP2VIDEO_CONTINUOUS_PROFILE)", "continuous runner boundary")
    require(runner, "np2_pccore_profiler_set_enabled(true)", "existing benchmark profiler")
    require(live, "kMotionMaximumAcquisitions = 64U", "bounded acquisition limit")
    require(live, "kMotionDistinctTarget = 16U", "distinct target")
    require(live, "PRESENTATION_SEQUENCE_FROZEN", "sequence failure")
    require(live, "IMMUTABLE_SOURCE_CHANGED", "same-frame lease failure")
    require(live, "MOTION_SAMPLE index=", "bounded accepted sample output")
    require(live, "MOTION_VALIDATION_SUMMARY", "machine-readable summary")
    require(live, "MOTION_VALIDATION_RESULT=", "machine-readable result")
    require(live, "profiler=OFF pause=OFF", "motion profile instrumentation policy")
    require(live, "np2_host_taskmng_cooperate();", "scheduler cooperation")
    if "kVisibleHoldUs" in live[live.index("esp_err_t run_motion_validation"):live.index("#endif", live.index("esp_err_t run_motion_validation"))]:
        raise AssertionError("motion profile must not use the 30-second visible hold")
    if "np2_pccore_profiler" in live[live.index("esp_err_t run_motion_validation"):live.index("#endif", live.index("esp_err_t run_motion_validation"))]:
        raise AssertionError("motion profile must not call the PCCORE profiler")
    require(oracle_h, "np2video_motion_guest_detect", "guest detector API")
    require(oracle_h, "np2video_motion_native_detect", "native detector API")
    if "malloc" in oracle_c or "free(" in oracle_c or "printf" in oracle_c or "UART" in oracle_c:
        raise AssertionError("motion oracle must remain pure, heap-free, and UART-free")

    fixture = ROOT / "tests/guest/np2video-live/np2video-live-fd1232.image"
    # The generated build fixture is intentionally not part of the source tree;
    # verify the immutable source inputs instead of requiring a build artifact.
    stage2 = (ROOT / "tests/guest/np2video-live/src/stage2.asm").read_bytes()
    if b"move_bar" not in stage2 or b"bar_pos" not in stage2:
        raise AssertionError("moving-bar fixture source is not present")
    golden = (ROOT / "tests/guest/np2video-live/golden.json").read_text(encoding="utf-8")
    require(golden, '"fixture_id": "np2video-7b2d-live-vram"', "fixture id")
    require(golden, '"minimum_distinct_frames": 16', "fixture distinct oracle")
    if fixture.exists():
        if fixture.stat().st_size != 1261568:
            raise AssertionError("fixture image size changed")
        if hashlib.sha256(fixture.read_bytes()).hexdigest() != "81975ad74c7b1769a5aa63977ee9c18b020d6381e858522cb4cb7c7861f85604":
            raise AssertionError("fixture image hash changed")

    print("Step 7B.2d motion-validation source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
