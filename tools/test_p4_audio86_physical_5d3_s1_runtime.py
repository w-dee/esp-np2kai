#!/usr/bin/env python3
"""Static/runtime-source contract for 86R.5D.3 sustained physical evidence."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "e4c05476a7f26a5c00f79979eca46f684f049447"
BINDING = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/"
    "p4_nano_audio86_guest_binding.cpp"
)
PREDICATE = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/include/"
    "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_predicate.hpp"
)
BUILD = ROOT / "tools/emu/build-production.sh"
CMAKE = ROOT / "firmware/components/p4_nano_audio86_guest_binding/CMakeLists.txt"
GOLDEN = ROOT / "host/probe/audio86_guest_sustained_2s_golden.json"
MANIFEST = ROOT / "tools/emu/p4_audio86_physical_sink_acceptance_manifest.tsv"
UNCHANGED = (
    "tools/emu/p4_audio86_physical_capture_v2.py",
    "host/probe/audio86_guest_sustained_2s_golden.json",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def unchanged_from_baseline(path: str) -> bool:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "diff", "--quiet", BASELINE, "--", path],
        check=False,
    )
    return result.returncode == 0


def main() -> int:
    binding = BINDING.read_text(encoding="utf-8")
    predicate = PREDICATE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    manifest = MANIFEST.read_text(encoding="utf-8")
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]

    new_case = section(
        build, "--audio86-real-guest-sustained-2s-physical-i2s)",
        "--audio86-real-guest-physical-i2s)")
    for token in (
        "audio86_real_guest=1", "audio86_pcm_output=1",
        "audio86_sustained=1", "audio86_sustained_physical=1",
        "audio86_physical_i2s=1", "audio86_async=1",
    ):
        require(token in new_case, f"new selector lost {token}")
    require("P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE=${audio86_sustained_physical}" in build,
            "sustained physical CMake selector binding missing")
    require("if (( audio86_physical_i2s )); then" in build and
            build.count('"${SCRIPT_DIR}/resolve-clean-source-git-sha.sh"') == 2,
            "new physical selector is not covered by fail-closed provenance")
    require("P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE=1" in cmake,
            "component sustained physical definition missing")

    old_sustained = section(build, "--audio86-real-guest-sustained-2s)",
                            "--audio86-real-guest-sustained-2s-physical-i2s)")
    old_s2 = section(build, "--audio86-real-guest-physical-i2s)",
                     "--audio86-real-guest-physical-i2s-short)")
    old_s1 = section(build, "--audio86-real-guest-physical-i2s-short)",
                     "--audio86-physical-lifecycle-test)")
    require("audio86_sustained=1" in old_sustained and
            "audio86_physical_i2s=1" not in old_sustained,
            "nonphysical sustained selector meaning changed")
    require("audio86_physical_selector=1" in old_s2 and
            "audio86_sustained=1" not in old_s2,
            "S2 selector meaning changed")
    require("audio86_physical_short=1" in old_s1 and
            "audio86_sustained=1" not in old_s1,
            "S1 selector meaning changed")

    evidence = section(binding, "void emit_physical_5d3_s1_evidence(",
                       "sustained_physical_local_health(")
    names = (
        "5D3_S1_IDENTITY schema=2", "5D3_S1_START schema=2",
        "5D3_S1_STREAM schema=2", "5D3_S1_PROGRESS schema=2",
        "5D3_S1_FINISH schema=2",
    )
    positions = [evidence.find(name) for name in names]
    require(all(position >= 0 for position in positions) and
            positions == sorted(positions) and
            all(evidence.count(name) == 1 for name in names),
            "5D3 record source count/order mismatch")
    require(evidence.count("evidence_class=PHYSICAL_EXEC") == 5,
            "physical record classification mismatch")
    tail = section(binding, "emit_physical_5d3_s1_evidence(runtime);",
                   "} // namespace")
    require(tail.index("P4_AUDIO86_REAL_GUEST_RESULT=%s") <
            tail.index("P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=%s"),
            "5D3 result/terminal source order mismatch")
    require(binding.count("P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=%s") == 1 and
            binding.count("emit_physical_5d3_s1_evidence(runtime);") == 1,
            "5D3 terminal/emitter gate is not unique")

    local_predicate = section(predicate, "bool sustained_physical_snapshot_healthy(",
                              "constexpr bool normal_terminal_healthy(")
    for token in (
        "physical_s2_snapshot_healthy(", "generated_digest_expected",
        "accepted_digest_matches_generated", "reset_identity_expected",
        "trace_shape_expected", "retry_identity_failures == 0U",
    ):
        require(token in local_predicate, f"F3 local predicate lost {token}")
    require("stream_wall" not in local_predicate and
            "max_running_accept_gap" not in local_predicate,
            "host-only realtime leaked into firmware predicate")

    require(golden["FULL_REPLAY_PCM_FRAMES"] == "96000" and
            golden["FULL_REPLAY_PCM_BYTES"] == "384000" and
            golden["FULL_REPLAY_PCM_CRC32"] == "5bb15277" and
            golden["FULL_REPLAY_PCM_SHA256"] ==
                "b315a9476e4fc30cbb7aea0c7a1bfa9cd4aa31a033c9223eb22500604fff62a0",
            "frozen F1 sustained golden identity changed")
    sha_block = section(
        binding, "constexpr uint8_t kSustainedExpectedPcmSha256",
        "constexpr uint8_t kSustainedExpectedPreResetPcmSha256")
    embedded_sha = "".join(
        match.group(1) for match in
        re.finditer(r"0x([0-9a-f]{2})U", sha_block)
    )
    require(embedded_sha == golden["FULL_REPLAY_PCM_SHA256"] and
            "kSustainedExpectedPcmCrc32 = 0x" +
            golden["FULL_REPLAY_PCM_CRC32"] + "U" in binding,
            "firmware local PCM identity differs from authoritative golden")
    pre_reset_sha_block = section(
        binding, "constexpr uint8_t kSustainedExpectedPreResetPcmSha256",
        "#endif")
    embedded_pre_reset_sha = "".join(
        match.group(1) for match in
        re.finditer(r"0x([0-9a-f]{2})U", pre_reset_sha_block)
    )
    require(embedded_pre_reset_sha ==
            "5ea610e1e93f2119f9f2175be509a91657aa5de074650ee2b78fe792e782c8d8" and
            "pre_reset_sha256=" in evidence,
            "frozen pre-reset SHA-256 is not bound into 5D3 evidence")
    require(all(unchanged_from_baseline(path) for path in UNCHANGED),
            "closed physical sink/capture/golden source changed")
    require("PHYSICAL_EXEC" not in "\n".join(
        line for line in manifest.splitlines() if "5d3" in line.lower()),
        "manifest claims pre-hardware 5D3 PHYSICAL_EXEC PASS")
    for token in (
        "np2audio86_sustained_cooperative_checkpoint(&cooperative)",
        "sustained_guest_delay_one_tick", "vTaskDelay(1U)",
        "NP2_AUDIO86_SUSTAINED_COOPERATIVE_SLICE_US",
        "P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT",
        "P4_NANO_AUDIO86_CONSUMER_PHASE_POST_ACCEPT_EVIDENCE",
        "TASK_PUBLISHED_RELATIVE_US_NO_ISR_TIMER",
        "retry_episode_units", "direct_running_accept_units",
        "max_downstream_submit_us", "max_post_accept_evidence_us",
    ):
        require(token in binding or token in (
                    ROOT / "firmware/components/np2audio86_fixture/include/"
                    "np2audio86_sustained_evidence.h").read_text(encoding="utf-8"),
                f"R2 source contract lost {token}")

    print("R2_PHYSICAL_SINK_ARCHITECTURE_SEMANTICS_UNCHANGED=PASS")
    print("F3_SUSTAINED_PHYSICAL_PROFILE_FIRST_CLASS=PASS")
    print("S1_SELECTOR_SEMANTICS_UNCHANGED=PASS")
    print("S2_SELECTOR_SEMANTICS_UNCHANGED=PASS")
    print("F2_SUSTAINED_NONPHYSICAL_SELECTOR_UNCHANGED=PASS")
    print("F3_SUSTAINED_PHYSICAL_PROVENANCE_FAIL_CLOSED=PASS")
    print("F3_EVIDENCE_NAMESPACE_ISOLATED=PASS")
    print("F3_PHYSICAL_EVIDENCE_CLASSIFICATION_TRUTHFUL=PASS")
    print("F3_FIRMWARE_RECORD_EMISSION_ORDER=PASS")
    print("F3_TERMINAL_MARKER_GATING=PASS")
    print("F3_FIRMWARE_TERMINAL_PREDICATE_TRUTHFUL=PASS")
    print("F3_FIRMWARE_REALTIME_FAILURE_GATE=ABSENT")
    print("F3_HOST_GOLDEN_REMAINS_AUTHORITATIVE=PASS")
    print("F3_PHYSICAL_EXEC_PASS=NOT_CLAIMED")
    print("SUSTAINED_GUEST_COOPERATIVE_SCHEDULING=PASS")
    print("WDT_FEED_ONLY_FIX_REJECTED=YES")
    print("PHYSICAL_ACCEPTANCE_THRESHOLDS_UNCHANGED=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
