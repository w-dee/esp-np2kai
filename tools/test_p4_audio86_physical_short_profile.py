#!/usr/bin/env python3
"""Static and rejection contracts for the 86R.5D.2 S1 short profile."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "tools/emu/build-production.sh"
BINDING_CMAKE = (ROOT / "firmware/components/p4_nano_audio86_guest_binding/"
                 "CMakeLists.txt")
BINDING_SOURCE = (ROOT / "firmware/components/p4_nano_audio86_guest_binding/"
                  "p4_nano_audio86_guest_binding.cpp")
MATRIX = ROOT / "tools/emu/test-p4-audio86-physical-build-matrix.sh"
MANIFEST = ROOT / "tools/emu/p4_audio86_physical_sink_acceptance_manifest.tsv"
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
PROVENANCE = ROOT / "tools/emu/resolve-clean-source-git-sha.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def case_arm(source: str, selector: str) -> str:
    match = re.search(
        rf"        {re.escape(selector)}\)\n(.*?)            ;;",
        source,
        re.DOTALL,
    )
    require(match is not None, f"selector arm is missing: {selector}")
    return match.group(1)


def run_rejected(*arguments: str, expected_fragment: str) -> None:
    completed = subprocess.run(
        [str(BUILD), *arguments],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    require(
        completed.returncode == 2,
        f"expected exit 2 for {arguments}, got {completed.returncode}: "
        f"{completed.stdout}",
    )
    require(
        expected_fragment in completed.stdout,
        f"missing rejection {expected_fragment!r}: {completed.stdout}",
    )


def main() -> int:
    build = BUILD.read_text(encoding="utf-8")
    binding_cmake = BINDING_CMAKE.read_text(encoding="utf-8")
    binding_source = BINDING_SOURCE.read_text(encoding="utf-8")
    matrix = MATRIX.read_text(encoding="utf-8")
    manifest = MANIFEST.read_text(encoding="utf-8")
    provenance = PROVENANCE.read_text(encoding="utf-8")
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]

    for assignment in (
        "audio86_real_guest=0",
        "audio86_pcm_output=0",
        "audio86_physical_i2s=0",
        "audio86_physical_short=0",
        "audio86_pcm_partial_eos=0",
        "audio86_async=0",
    ):
        require(re.search(rf"^{assignment}$", build, re.MULTILINE) is not None,
                f"default profile assignment changed: {assignment}")

    expected_existing_arms = {
        "--audio86-async-inactive": {"audio86_async=1"},
        "--audio86-real-guest": {
            "audio86_real_guest=1", "audio86_async=1"},
        "--audio86-real-guest-pcm-output": {
            "audio86_real_guest=1", "audio86_pcm_output=1",
            "audio86_async=1"},
        "--audio86-real-guest-physical-i2s": {
            "audio86_real_guest=1", "audio86_pcm_output=1",
            "audio86_physical_i2s=1", "audio86_physical_selector=1",
            "audio86_async=1"},
        "--audio86-real-guest-pcm-output-partial": {
            "audio86_real_guest=1", "audio86_pcm_output=1",
            "audio86_pcm_partial_eos=1", "audio86_partial_selector=1",
            "audio86_async=1"},
    }
    for selector, expected in expected_existing_arms.items():
        arm = case_arm(build, selector)
        assignments = {line.strip() for line in arm.splitlines() if "=" in line}
        require(assignments == expected,
                f"existing selector composition changed: {selector}")

    short = case_arm(build, "--audio86-real-guest-physical-i2s-short")
    expected_short_assignments = {
        "audio86_real_guest=1",
        "audio86_pcm_output=1",
        "audio86_physical_i2s=1",
        "audio86_physical_short=1",
        "audio86_pcm_partial_eos=1",
        "audio86_async=1",
    }
    require(
        {line.strip() for line in short.splitlines() if "=" in line}
        == expected_short_assignments,
        "short selector does not have the frozen first-class composition",
    )

    full = case_arm(build, "--audio86-real-guest-physical-i2s")
    partial = case_arm(build, "--audio86-real-guest-pcm-output-partial")
    require("audio86_pcm_partial_eos=1" not in full,
            "full physical selector acquired partial-EOS semantics")
    require("audio86_physical_short=1" not in full,
            "full physical selector acquired short-profile identity")
    require("audio86_physical_i2s=1" not in partial,
            "virtual partial selector acquired the physical backend")
    require("audio86_physical_short=1" not in partial,
            "virtual partial selector acquired short-profile identity")

    for token in (
        'P4_NANO_AUDIO86_REAL_GUEST_PROFILE=${audio86_real_guest}',
        'P4_NANO_AUDIO86_PCM_OUTPUT_PROFILE=${audio86_pcm_output}',
        'P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE=${audio86_physical_i2s}',
        'P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE=${audio86_physical_short}',
        'P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE=${audio86_pcm_partial_eos}',
        'NP2_ASYNC_AUDIO86=${audio86_async}',
        'build-${board}-${variant}-audio86-physical-i2s-short',
    ):
        require(token in build, f"missing short profile routing token: {token}")
    require(
        "if(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)" in binding_cmake and
        "P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE=1" in binding_cmake,
        "binding component does not receive the short-profile identity",
    )
    require(
        build.count('"${SCRIPT_DIR}/resolve-clean-source-git-sha.sh"') == 2 and
        'post_build_git_sha' in build and
        '"${post_build_git_sha}" != "${REPOSITORY_GIT_SHA}"' in build,
        "physical-short build does not enforce clean provenance before and "
        "after the build",
    )
    for command in (
        "ls-files -v -z",
        "diff-files --quiet --ignore-submodules=none",
        "diff-index --cached --quiet",
        "ls-files --others --exclude-standard -z",
    ):
        require(command in provenance,
                f"clean-source provenance check missing: {command}")

    physical_start = binding_source.index("np2_pcm_sink selected_sink = kPcmSink;")
    physical_branch = binding_source[
        physical_start:
        binding_source.index("if (np2_pcm_output_controller_init", physical_start)
    ]
    require("P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE" in physical_branch and
            "create_idf(" in physical_branch and
            "p4_nano_audio86_physical_sink_interface(" in physical_branch,
            "physical profiles do not select the production IDF sink")
    require("create_lifecycle_test(" in physical_branch,
            "fake lifecycle backend guard unexpectedly disappeared")

    partial_fixture = binding_source[
        binding_source.index("#if defined(P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE)"):
        binding_source.index("#elif P4_NANO_AUDIO86_PCM_LIFECYCLE_SCENARIO",)
    ]
    require("kRenderFrames = 13U" in partial_fixture and
            "kExpectedPcmSlots = 1U" in partial_fixture and
            "kExpectedPartialSlots = 1U" in partial_fixture,
            "existing 13-frame partial-EOS fixture semantics changed")
    render_profiles = binding_source[
        binding_source.index("#if defined(P4_NANO_AUDIO86_PCM_PARTIAL_EOS_PROFILE)"):
        binding_source.index("constexpr size_t kApplyRecordBytes")
    ]
    require(
        re.search(
            r"#else\s+constexpr size_t kRenderFrames = 2400U;\s+"
            r"constexpr uint32_t kExpectedPcmSlots = 10U;\s+"
            r"constexpr uint32_t kExpectedPartialSlots = 0U;\s+#endif",
            render_profiles,
        ) is not None,
        "full profile is not the unchanged 2400-frame/10-q240 fixture",
    )
    require(golden["PRE_RESET_PCM_FRAMES"] == "13" and
            golden["PRE_RESET_PCM_BYTES"] == "52" and
            golden["PRE_RESET_PCM_CRC32"] == "f1b8c4c5" and
            golden["PRE_RESET_PCM_SHA256"] ==
            "d51e85a3e8d63ecd763988f02521ef38e754f914ad84fc728375c8d84b8bf9a7",
            "frozen PRE_RESET_PCM identity changed")

    require(
        "audio86_physical_selector + audio86_partial_selector +" in build and
        "audio86_physical_short > 1" in build and
        "--audio86-real-guest-physical-i2s-short are mutually exclusive" in build,
        "manual physical-plus-partial composition is not rejected",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio86-real-guest-physical-i2s",
        "--audio86-real-guest-pcm-output-partial",
        expected_fragment="are mutually exclusive",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio86-real-guest-pcm-output-partial",
        "--audio86-real-guest-physical-i2s",
        expected_fragment="are mutually exclusive",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio86-real-guest-physical-i2s-short",
        "--audio86-real-guest-physical-i2s",
        expected_fragment="are mutually exclusive",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio86-real-guest-physical-i2s-short",
        "--audio86-real-guest-pcm-output-partial",
        expected_fragment="are mutually exclusive",
    )
    run_rejected(
        "--variant", "p4-v3x", "--board", "generic",
        "--audio86-real-guest-physical-i2s-short", "--esp-emu-test",
        expected_fragment="requires physical p4-v1x/p4-nano",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "generic",
        "--audio86-real-guest-physical-i2s-short",
        expected_fragment="--audio86-real-guest requires",
    )
    run_rejected(
        "--variant", "p4-v1x", "--board", "p4-nano",
        "--audio86-real-guest-physical-i2s-short", "--esp-emu-test",
        expected_fragment="--esp-emu-test requires the generic p4-v3x",
    )

    explicit_builds = len(re.findall(r"^build ", matrix, re.MULTILINE))
    stages = re.search(r"^stages=\(([^)]*)\)$", matrix, re.MULTILINE)
    require(stages is not None, "lifecycle stage matrix is missing")
    stage_count = len(stages.group(1).split())
    require(explicit_builds + stage_count == 12,
            "physical build matrix is not exactly 12 profiles")
    require(matrix.count("--audio86-real-guest-physical-i2s-short") == 1,
            "short physical matrix entry must occur exactly once")
    require(matrix.count("--audio86-real-guest-physical-i2s\n") == 1,
            "full physical matrix entry must remain exactly once")
    require("5D2_S1A_BUILD_MATRIX=12/12_PASS" in matrix,
            "matrix result marker is not truthful")
    require("build_matrix\tTwelve profiles build\tBUILD" in manifest,
            "physical acceptance manifest does not describe 12 builds")

    print("S1_PROFILE_IS_FIRST_CLASS=YES")
    print("S1_PROFILE_ARBITRARY_SELECTOR_COMPOSITION=NO")
    print("S1_SHORT_REUSES_EXISTING_13_FRAME_FIXTURE=PASS")
    print("S1_SELECTOR_ISOLATION=PASS")
    print("FULL_PHYSICAL_PROFILE_SEMANTICS_UNCHANGED=PASS")
    print("VIRTUAL_PARTIAL_PROFILE_SEMANTICS_UNCHANGED=PASS")
    print("5D2_S1A_BUILD_MATRIX_EXPECTED=12")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
