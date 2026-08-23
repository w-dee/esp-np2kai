#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d coarse pccore phase profiler."""

from __future__ import annotations

import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE_CMAKE = ROOT / "firmware/components/np2core/CMakeLists.txt"
PROFILER_HEADER = ROOT / "firmware/components/np2core/include/np2_pccore_profiler.h"
PROFILER_SOURCE = ROOT / "firmware/components/np2core/np2_pccore_profiler.c"
RUNNER = ROOT / "firmware/components/np2video_runner/np2video_runner.c"
RUNNER_HEADER = ROOT / "firmware/components/np2video_runner/include/np2video_runner/np2video_runner.h"
LIVE_DISPLAY = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
PCCORE = ROOT / "third_party/np2kai/src/pccore.c"
SCRNDRAW = ROOT / "third_party/np2kai/src/vram/scrndraw.c"
PATCH_SET = ROOT / "host/patches/np2kai/step4/patch-set.json"
PCCORE_PATCH = ROOT / "host/patches/np2kai/step4/0001-pccore-non-fmgen-vol-midi.patch"
SCRNDRAW_PATCH = ROOT / "host/patches/np2kai/step4/0008-pccore-phase-profiler-scrndraw.patch"


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def require(text: str, fragment: str, description: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {description}: {fragment}")


def main() -> int:
    cmake = read(CORE_CMAKE)
    header = read(PROFILER_HEADER)
    source = read(PROFILER_SOURCE)
    runner = read(RUNNER)
    runner_header = read(RUNNER_HEADER)
    live = read(LIVE_DISPLAY)
    pccore = read(PCCORE)
    scrndraw = read(SCRNDRAW)
    pccore_patch = read(PCCORE_PATCH)
    scrndraw_patch = read(SCRNDRAW_PATCH)
    patch_set = json.loads(read(PATCH_SET))

    require(cmake, "NP2_PCCORE_PHASE_PROFILER_ACTIVE", "profile selector")
    require(cmake, "NP2VIDEO_BENCHMARK_PROFILE", "benchmark-only selector input")
    require(cmake, "NP2_PCCORE_PHASE_PROFILER=1", "enabled profiler definition")
    if "NP2_PCCORE_PHASE_PROFILER=1" in cmake.split("if(NP2_PCCORE_PHASE_PROFILER_ACTIVE)", 1)[0]:
        raise AssertionError("profiler definition must stay inside benchmark gate")

    for phase in (
        "NP2_PCCORE_PHASE_LOOP_INCLUSIVE",
        "NP2_PCCORE_PHASE_CALLBACKS",
        "NP2_PCCORE_PHASE_SOUND",
        "NP2_PCCORE_PHASE_DRAW_NESTED",
    ):
        require(header, phase, f"phase enum {phase}")
    for field in ("count", "total_us", "max_single_us", "min_single_us"):
        require(header, field, f"phase statistic {field}")
    require(header, "static inline uint64_t np2_pccore_profiler_phase_begin",
            "disabled begin no-op")
    require(header, "static inline void np2_pccore_profiler_phase_end",
            "disabled end no-op")
    require(source, "esp_timer_get_time()", "ESP wall-clock timing")
    for forbidden in ("printf", "ESP_LOG", "ets_printf", "fwrite",
                      "vTaskDelay", "taskYIELD", "np2_host_taskmng_cooperate"):
        if forbidden in source or forbidden in pccore_patch or forbidden in scrndraw_patch:
            raise AssertionError(f"profiler hot path contains forbidden operation: {forbidden}")

    require(pccore_patch, "while (pcstat.screendispflag)", "LOOP boundary")
    require(pccore_patch, "NP2_PCCORE_PHASE_LOOP_INCLUSIVE", "LOOP hook")
    require(pccore_patch, "NP2_PCCORE_PHASE_CALLBACKS", "CALLBACKS hook")
    require(pccore_patch, "NP2_PCCORE_PHASE_SOUND", "SOUND hook")
    require(scrndraw_patch, "NP2_PCCORE_PHASE_DRAW_NESTED", "nested DRAW hook")
    require(pccore, "nevent_progress();", "event-loop call path")
    require(pccore, "drawscreen();", "screen event draw path")
    for semantic in ("CPU_EXEC();", "CPU_EXECV30();", "nevent_progress();",
                     "artic_callback();", "S98_sync();", "sound_sync();"):
        require(pccore, semantic, f"unchanged pccore semantic {semantic}")
    require(scrndraw, "UINT8 scrndraw_draw(UINT8 redraw)", "draw boundary")
    require(scrndraw, "scrnmng_surflock()", "unchanged draw surface lock")

    require(runner, "np2_pccore_profiler_reset();", "benchmark profiler reset")
    require(runner, "np2_pccore_profiler_set_enabled(true);",
            "benchmark profiler enable")
    require(runner, "np2_pccore_profiler_set_enabled(false);",
            "profiler disable before cleanup")
    require(runner, "np2_pccore_profiler_snapshot(&result.pccore_profile);",
            "profile snapshot")
    require(runner_header, "np2_pccore_profile pccore_profile;",
            "runner profile result")
    require(live, "P4_NANO_PCCORE_PHASE", "post-interval phase summaries")
    require(live, '"loop_inclusive",', "loop output")
    require(live, '"draw_nested",', "draw output")
    require(live, "nested_draw_excluded_from_top_level=1",
            "nested draw reconciliation metadata")
    require(live, "top_level_profiled_us =\n        pccore_profile.phases[NP2_PCCORE_PHASE_LOOP_INCLUSIVE].total_us +\n        pccore_profile.phases[NP2_PCCORE_PHASE_CALLBACKS].total_us +\n        pccore_profile.phases[NP2_PCCORE_PHASE_SOUND].total_us",
            "top-level reconciliation excludes nested draw")
    if "NP2_PCCORE_PHASE_DRAW_NESTED].total_us +" in live:
        raise AssertionError("nested DRAW must not be added to top-level total")

    entries = patch_set["entries"]
    patched_sources = {entry["logical_source"] for entry in entries}
    if "vram/scrndraw.c" not in patched_sources:
        raise AssertionError("scrndraw vendor hook is not in the established patch set")
    if "pccore.c" not in patched_sources:
        raise AssertionError("pccore vendor hook is not in the established patch set")

    print("Step 7B.2d pccore phase profiler source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
