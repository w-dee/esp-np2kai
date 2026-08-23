#!/usr/bin/env python3
"""Contract checks for the Step 7B.2d coarse pccore phase profiler."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


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
VENDOR_ROOT = ROOT / "third_party/np2kai"
IMPORT_MANIFEST = VENDOR_ROOT / "import-manifest.json"
STEP4_PREPARER = ROOT / "host/tools/prepare_step4_sources.py"


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def require(text: str, fragment: str, description: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {description}: {fragment}")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def eol_counts(value: bytes) -> tuple[int, int]:
    crlf = value.count(b"\r\n")
    bare_lf = value.count(b"\n") - crlf
    return crlf, bare_lf


def patch_payload_lines(value: bytes) -> list[bytes]:
    """Return raw unified-diff hunk payload lines, excluding file headers."""

    payload: list[bytes] = []
    for line in value.splitlines(keepends=True):
        if line.startswith((b"---", b"+++", b"\\ No newline")):
            continue
        if line[:1] in (b" ", b"+", b"-"):
            payload.append(line)
    return payload


def verify_step4_patch_eol(entries: list[dict[str, object]]) -> None:
    """Check EOL-sensitive patch bytes independently of the local Git version."""

    styles: dict[str, bytes] = {}
    for entry in entries:
        logical_source = entry["logical_source"]
        pristine_path = VENDOR_ROOT / "src" / pathlib.Path(str(entry["pristine_path"]))
        patch_path = PATCH_SET.parent / pathlib.Path(str(entry["patch_path"]))
        pristine = pristine_path.read_bytes()
        patch = patch_path.read_bytes()
        crlf, bare_lf = eol_counts(pristine)
        if crlf and bare_lf:
            continue
        if not crlf and not bare_lf:
            continue
        expected_eol = b"\r\n" if crlf else b"\n"
        styles[str(logical_source)] = expected_eol
        payload = patch_payload_lines(patch)
        if not payload:
            raise AssertionError(f"missing patch payload for {logical_source}")
        for index, line in enumerate(payload, start=1):
            if expected_eol == b"\n":
                compatible = line.endswith(b"\n") and not line.endswith(b"\r\n")
            else:
                compatible = line.endswith(b"\r\n")
            if not compatible:
                raise AssertionError(
                    f"EOL mismatch in {logical_source} patch payload line {index}"
                )

    with tempfile.TemporaryDirectory(prefix="step7b2d-patch-eol-") as temporary:
        output_root = pathlib.Path(temporary) / "step4-prepared"
        completed = subprocess.run(
            [
                sys.executable,
                str(STEP4_PREPARER),
                "--vendor-root",
                str(VENDOR_ROOT),
                "--import-manifest",
                str(IMPORT_MANIFEST),
                "--patch-set",
                str(PATCH_SET),
                "--output-root",
                str(output_root),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if completed.returncode:
            raise AssertionError(
                "fresh Step 4 preparation failed during EOL contract: "
                f"{completed.stdout.strip()}"
            )
        for entry in entries:
            logical_source = str(entry["logical_source"])
            expected_eol = styles.get(logical_source)
            if expected_eol is None:
                continue
            prepared = output_root / "patched-src" / pathlib.Path(logical_source)
            prepared_bytes = prepared.read_bytes()
            crlf, bare_lf = eol_counts(prepared_bytes)
            if expected_eol == b"\r\n" and (not crlf or bare_lf):
                raise AssertionError(
                    f"prepared {logical_source} does not preserve CRLF bytes"
                )
            if expected_eol == b"\n" and crlf:
                raise AssertionError(
                    f"prepared {logical_source} unexpectedly contains CRLF bytes"
                )
            expected_hash = str(entry["patched_sha256"])
            if sha256_bytes(prepared_bytes) != expected_hash:
                raise AssertionError(
                    f"prepared SHA-256 mismatch for {logical_source}"
                )


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
    require(header, "void np2_pccore_profiler_phase_begin",
            "tokenless begin API")
    require(header, "void np2_pccore_profiler_phase_end(np2_pccore_phase phase)",
            "tokenless end API")
    if "uint64_t np2_pccore_profiler_phase_begin" in header:
        raise AssertionError("begin API must not return a timing token")
    if "start_us" in header:
        raise AssertionError("timing start storage must stay project-owned")
    require(header, "static inline void np2_pccore_profiler_phase_begin",
            "disabled begin no-op")
    require(header, "static inline void np2_pccore_profiler_phase_end",
            "disabled end no-op")
    require(source, "esp_timer_get_time()", "ESP wall-clock timing")
    require(source, "start_us[NP2_PCCORE_PHASE_COUNT]",
            "project-owned per-phase start storage")
    require(source, "active[NP2_PCCORE_PHASE_COUNT]",
            "project-owned per-phase active storage")
    require(source, "if (!np2_pccore_profile_state.active[phase])",
            "safe unmatched phase end")
    for forbidden in ("printf", "ESP_LOG", "ets_printf", "fwrite",
                      "vTaskDelay", "taskYIELD", "np2_host_taskmng_cooperate"):
        if forbidden in source or forbidden in pccore_patch or forbidden in scrndraw_patch:
            raise AssertionError(f"profiler hot path contains forbidden operation: {forbidden}")

    require(pccore_patch, "while (pcstat.screendispflag)", "LOOP boundary")
    require(pccore_patch, "NP2_PCCORE_PHASE_LOOP_INCLUSIVE", "LOOP hook")
    require(pccore_patch, "NP2_PCCORE_PHASE_CALLBACKS", "CALLBACKS hook")
    require(pccore_patch, "NP2_PCCORE_PHASE_SOUND", "SOUND hook")
    require(scrndraw_patch, "NP2_PCCORE_PHASE_DRAW_NESTED", "nested DRAW hook")
    if "start_us" in pccore_patch or "start_us" in scrndraw_patch:
        raise AssertionError("vendor hooks must not retain automatic start_us")
    if pccore_patch.count(
        "np2_pccore_profiler_phase_begin(NP2_PCCORE_PHASE_LOOP_INCLUSIVE);") != 1:
        raise AssertionError("LOOP must use one tokenless begin hook")
    early_return = scrndraw_patch.split("ret = 1;", 1)[1]
    require(
        early_return,
        "+\t\t\tnp2_pccore_profiler_phase_end(NP2_PCCORE_PHASE_DRAW_NESTED);\n+#endif\n \t\t\treturn(ret);",
        "DRAW WAB early return closes phase",
    )
    if scrndraw_patch.count(
        "np2_pccore_profiler_phase_end(NP2_PCCORE_PHASE_DRAW_NESTED);") != 2:
        raise AssertionError("DRAW must close both early and common exits")
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

    verify_step4_patch_eol(entries)

    print("Step 7B.2d pccore phase profiler source contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
