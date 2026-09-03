#!/usr/bin/env python3
"""Static contract for 86R.5D.2 S2A runtime evidence support."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BINDING = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/"
    "p4_nano_audio86_guest_binding.cpp"
)
TERMINAL = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/include/"
    "p4_nano_audio86_guest_binding/p4_nano_audio86_terminal_predicate.hpp"
)
BUILD = ROOT / "tools/emu/build-production.sh"
CMAKE = ROOT / "firmware/components/p4_nano_audio86_guest_binding/CMakeLists.txt"
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def ordered(source: str, *tokens: str) -> bool:
    positions = [source.find(token) for token in tokens]
    return all(position >= 0 for position in positions) and positions == sorted(positions)


def between(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first:source.index(end, first)]


def main() -> int:
    binding = BINDING.read_text(encoding="utf-8")
    terminal = TERMINAL.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]

    require(ordered(
        binding,
        "defined(P4_NANO_AUDIO86_PHYSICAL_I2S_PROFILE)",
        "!defined(P4_NANO_AUDIO86_PHYSICAL_SHORT_PROFILE)",
        "!defined(P4_NANO_AUDIO86_PHYSICAL_LIFECYCLE_TEST_PROFILE)",
        "#define P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE 1"),
        "S2 derived profile gate changed")
    require(golden["FULL_REPLAY_PCM_FRAMES"] == "2400" and
            golden["FULL_REPLAY_PCM_BYTES"] == "9600" and
            golden["FULL_REPLAY_PCM_CRC32"] == "b518c3c9" and
            golden["FULL_REPLAY_PCM_SHA256"] ==
            "176ea419f153382039e143163ff8476c5461abfddd055cd801003ef89c04a18a" and
            golden["FULL_REPLAY_PCM_PEAK"] == "4148",
            "frozen full replay golden changed")

    evidence = between(binding, "void emit_physical_s2_evidence(",
                       "bool physical_s2_snapshot_healthy(")
    record_names = (
        "5D2_S2_IDENTITY schema=1",
        "5D2_S2_START schema=1",
        "5D2_S2_STREAM schema=1",
        "5D2_S2_FINISH schema=1",
    )
    require(ordered(evidence, *record_names) and
            all(evidence.count(name) == 1 for name in record_names),
            "S2 record count/order changed")
    require(ordered(
        evidence,
        "semantic_frames=", "semantic_bytes=", "semantic_crc32=",
        "semantic_sha256=", "produced_frames=", "produced_bytes=",
        "produced_slots=", "controller_accepted_frames=",
        "controller_accepted_bytes=", "sink_accepted_frames=",
        "sink_accepted_bytes=", "physical_units=", "full_units=",
        "final_partial_units=", "final_valid_frames=", "padding_frames=",
        "padding_bytes=", "preloaded_units=", "running_units=",
        "submit_attempts=", "retry_count=", "running_q_ovf=",
        "final_ring_occupancy=", "final_ring_partial=", "drops=",
        "overwrite=", "abandoned_published=", "abandoned_partial=",
        "abandoned_rendered=", "semantic_duration_ms="),
        "S2 STREAM field order changed")
    require(ordered(
        evidence,
        "final_copy_eof_epoch=", "drain_completion_eof_epoch=",
        "quiescent_eof_epoch=", "drain_post_snapshot_eofs=",
        "quiescent_post_snapshot_eofs=", "drain_duration_ms=",
        "finish_completed=", "pending_frames=", "drained_frames=",
        "discarded_frames=", "draining_q_ovf=", "sticky_error=",
        "registered_generation=", "terminal_generation=", "stale_callbacks=",
        "callback_in_flight=", "callbacks_active=", "codec_final_muted=",
        "pa_final_low=", "i2s_enabled=", "i2s_created=", "first_error=",
        "forced_abort=", "sink_destroyed="),
        "S2 FINISH field order changed")
    require("p4_nano_audio86_physical_sink_get_telemetry" not in evidence and
            evidence.count("runtime->physical") == 1,
            "S2 evidence is not emitted from the owned snapshot")

    predicate = between(terminal, "bool physical_s2_snapshot_healthy(",
                        "constexpr bool normal_terminal_healthy(")
    for token in (
        "physical_snapshot_healthy(snapshot)",
        "snapshot.semantic_frames == expected_frames",
        "sink.physical_units_copied == expected_units",
        "sink.full_units == expected_units",
        "sink.final_partial_units == 0U",
        "sink.physical_padding_frames == 0U",
        "sink.preloaded_units == expected_preloaded_units",
        "sink.submit_attempts - sink.physical_units_copied ==",
        "sink.retry_count",
        "P4_NANO_AUDIO86_PHYSICAL_DRAIN_TIMEOUT_MS",
    ):
        require(token in predicate, f"S2 predicate lost {token}")

    require(binding.count("P4_AUDIO86_PHYSICAL_S2_TERMINAL=%s") == 1 and
            "#elif defined(P4_NANO_AUDIO86_PHYSICAL_S2_PROFILE)" in binding,
            "S2 terminal marker gate changed")
    require(build.splitlines().count(
                "if (( audio86_physical_i2s )); then") == 2 and
            build.count('"${SCRIPT_DIR}/resolve-clean-source-git-sha.sh"') == 2 and
            "physical Audio 86 source HEAD changed during build" in build and
            "physical Audio 86 evidence requires P4_AUDIO86_GIT_SHA" in cmake,
            "full physical provenance is not fail-closed")

    print("S2_EXISTING_FULL_PROFILE_REUSED=PASS")
    print("S2_REUSES_EXISTING_FULL_REPLAY_GOLDEN=PASS")
    print("S2_NEW_PCM_GOLDEN=NO")
    print("S2_WORKLOAD_PREDICATE=PASS")
    print("S2_RETRY_CONTRACT_PRESERVED=PASS")
    print("S2_PHYSICAL_OWNERSHIP_CONTRACT=PASS")
    print("S2_EOF_INTERVAL_CONTRACT=PASS")
    print("S2_STRUCTURED_RECORD_SOURCE_SCHEMA=PASS")
    print("S2_FULL_PHYSICAL_TERMINAL_PREDICATE_TRUTHFUL=PASS")
    print("S2_FULL_PHYSICAL_PROVENANCE_FAIL_CLOSED=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
