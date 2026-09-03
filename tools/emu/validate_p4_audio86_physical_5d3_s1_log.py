#!/usr/bin/env python3
"""Fail-closed validator for 86R.5D.3 S1 sustained physical UART evidence."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

try:
    from .p4_audio86_physical_capture_v2 import (
        UINT32_HALF_RANGE,
        decimal,
        require,
        uint32_delta,
        valid_source_git_sha,
        validate_capture_status_v2,
        validate_region_fatal_safety,
        validate_structural_tail,
    )
except ImportError:
    from p4_audio86_physical_capture_v2 import (
        UINT32_HALF_RANGE,
        decimal,
        require,
        uint32_delta,
        valid_source_git_sha,
        validate_capture_status_v2,
        validate_region_fatal_safety,
        validate_structural_tail,
    )


ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "host/probe/audio86_guest_sustained_2s_golden.json"
RECORD_FIELDS = {
    "5D3_S1_IDENTITY": (
        "schema", "evidence_class", "source_git_sha", "profile",
        "workload_id", "board", "backend", "display",
        "guest_program_bytes", "guest_program_crc32", "guest_program_sha256",
    ),
    "5D3_S1_START": (
        "schema", "evidence_class", "rate_hz", "channels", "sample_bits",
        "encoding", "i2s_format", "clock_source", "mclk_multiple", "mclk_hz",
        "q_frames", "bytes_per_frame", "physical_unit_bytes", "dma_desc",
        "dma_frames", "ring_capacity", "prefill", "semantic_duration_ms",
        "expected_units", "prepare_completed", "pa_initial_low",
        "codec_initialized_muted", "i2s_initialized",
        "muted_warmup_completed", "callbacks_registered", "stream_started",
        "codec_unmute_completed", "startup_durations_valid",
        "enable_stream_duration_us", "codec_unmute_duration_us",
    ),
    "5D3_S1_STREAM": (
        "schema", "evidence_class", "generated_frames", "generated_bytes",
        "generated_crc32", "generated_sha256", "accepted_frames",
        "accepted_bytes", "accepted_crc32", "accepted_sha256",
        "generated_units", "accepted_units", "next_generated_sequence",
        "next_accepted_sequence", "next_generated_frame_offset",
        "next_accepted_frame_offset", "generated_slot_fill_frames",
        "first_sequence", "first_offset", "first_valid_frames", "first_crc32",
        "final_sequence", "final_offset", "final_slot_valid_frames", "final_crc32",
        "pre_reset_frames", "pre_reset_bytes", "pre_reset_crc32",
        "pre_reset_sha256",
        "reset_frame", "reset_sequence", "reset_ordinal", "reset_opcode",
        "io_count", "io_crc32", "io_sha256", "event_count", "event_crc32",
        "event_sha256", "timer_count", "timer_crc32", "timer_sha256",
        "action_count", "action_crc32", "action_sha256", "final_state_count",
        "final_state_crc32", "final_state_sha256",
        "controller_accepted_frames", "controller_accepted_bytes",
        "sink_accepted_frames", "sink_accepted_bytes", "physical_units",
        "full_units", "final_partial_units", "final_valid_frames",
        "padding_frames", "padding_bytes", "submit_attempts", "retry_count",
        "retry_identity_failures", "retry_episode_units",
        "direct_running_accept_units", "running_q_ovf", "final_ring_occupancy",
        "final_ring_partial", "drops", "overwrite", "abandoned_published",
        "abandoned_partial", "abandoned_rendered",
    ),
    "5D3_S1_PROGRESS": (
        "schema", "evidence_class", "pcm_ring_max_occupancy",
        "pcm_producer_full_wait_count",
        "pcm_consumer_empty_after_release_before_done_count",
        "max_running_accept_gap_ms", "stream_started_ms", "drain_completed_ms",
        "stream_wall_ms", "preloaded_units", "running_accepted_units",
        "max_gap_initial", "max_gap_previous_sequence_valid",
        "max_gap_previous_sequence", "max_gap_next_sequence",
        "max_gap_previous_relative_ms", "max_gap_next_relative_ms",
        "max_downstream_submit_us", "max_downstream_submit_sequence",
        "max_post_accept_evidence_us", "max_post_accept_evidence_sequence",
        "timing_authority",
    ),
    "5D3_S1_FINISH": (
        "schema", "evidence_class", "controller_state", "sink_state",
        "final_copy_eof_epoch", "drain_completion_eof_epoch",
        "quiescent_eof_epoch", "drain_post_snapshot_eofs",
        "quiescent_post_snapshot_eofs", "drain_duration_ms",
        "finish_completed", "pending_frames", "drained_frames",
        "discarded_frames", "draining_q_ovf", "sticky_error",
        "first_active_qovf_latched", "first_qovf_state",
        "first_qovf_eof_epoch", "first_qovf_phase",
        "first_qovf_current_sequence", "first_qovf_published_sequence",
        "first_qovf_last_step_enter_us",
        "first_qovf_last_submit_return_us", "first_qovf_last_step_exit_us",
        "first_qovf_last_running_accepted_us", "first_qovf_wait_reason",
        "first_qovf_consumer_next_sequence",
        "first_qovf_next_published_sequence", "first_qovf_q399_rendered",
        "first_qovf_q399_published", "first_qovf_q399_available",
        "first_qovf_ring_occupancy", "first_qovf_production_done",
        "first_qovf_rendered_frames", "first_qovf_eof_notify_count",
        "first_qovf_hpwoken_true_count",
        "first_qovf_retry_wait_enter_count",
        "first_qovf_retry_wait_resume_count",
        "first_qovf_ring_wait_enter_count",
        "first_qovf_ring_wait_resume_count", "first_qovf_last_wait_enter_us",
        "first_qovf_last_wait_resume_us", "first_qovf_last_resume_reason",
        "first_qovf_last_resume_sequence", "notification_state_model",
        "cpu0_task_identity", "service_phase_semantics",
        "first_qovf_observed",
        "first_qovf_observed_us", "qovf_time_semantics",
        "registered_generation", "terminal_generation", "stale_callbacks",
        "callback_in_flight", "callbacks_active", "codec_final_muted",
        "pa_final_low", "i2s_enabled", "i2s_created", "first_error",
        "forced_abort", "sink_destroyed",
    ),
}
RECORD_ORDER = tuple(RECORD_FIELDS)
CRC32 = re.compile(r"[0-9a-f]{8}")
SHA256 = re.compile(r"[0-9a-f]{64}")
PRE_RESET_SHA256 = "5ea610e1e93f2119f9f2175be509a91657aa5de074650ee2b78fe792e782c8d8"
PASS_LINE = b"P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=COMPLETE"
FAIL_LINE = b"P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=FAILED"
REAL_GUEST_PASS_LINE = b"P4_AUDIO86_REAL_GUEST_RESULT=PASS"
REAL_GUEST_PREFIX = b"P4_AUDIO86_REAL_GUEST_RESULT="
AUTHORITATIVE_PREFIXES = (
    b"5D3_S1_", b"5D2_S1_", b"5D2_S2_",
    b"P4_AUDIO86_PHYSICAL_S1_TERMINAL=",
    b"P4_AUDIO86_PHYSICAL_S2_TERMINAL=",
)


def parse_records(raw: bytes, errors: list[str]) -> dict[str, dict[str, str]]:
    found: list[tuple[str, dict[str, str]]] = []
    for raw_line in raw.splitlines():
        stripped = raw_line.lstrip()
        if stripped.startswith((
                b"P4_AUDIO86_PHYSICAL_S1_TERMINAL=",
                b"P4_AUDIO86_PHYSICAL_S2_TERMINAL=")):
            errors.append("5D2 terminal marker is not 5D3 evidence")
            continue
        if stripped.startswith((b"5D2_S1_", b"5D2_S2_")):
            errors.append("5D2 authoritative record is not 5D3 evidence")
            continue
        if stripped.startswith(b"5D3_S1_") and not raw_line.startswith(b"5D3_S1_"):
            errors.append("authoritative record must start in column zero")
            continue
        if not raw_line.startswith(b"5D3_S1_"):
            continue
        try:
            line = raw_line.decode("ascii")
        except UnicodeDecodeError:
            errors.append("non-ASCII 5D3_S1 record")
            continue
        tokens = line.split()
        name = tokens[0]
        if name not in RECORD_FIELDS:
            errors.append(f"unknown record {name}")
            continue
        pairs: list[tuple[str, str]] = []
        malformed = False
        for token in tokens[1:]:
            if token.count("=") != 1:
                malformed = True
                break
            key, value = token.split("=", 1)
            if not key or not value:
                malformed = True
                break
            pairs.append((key, value))
        if malformed:
            errors.append(f"malformed record {name}")
            continue
        keys = tuple(key for key, _ in pairs)
        require(errors, keys == RECORD_FIELDS[name],
                f"{name}: field set/order mismatch")
        require(errors, len(set(keys)) == len(keys), f"{name}: duplicate field")
        found.append((name, dict(pairs)))
    require(errors, tuple(name for name, _ in found) == RECORD_ORDER,
            "authoritative record count/order mismatch")
    records: dict[str, dict[str, str]] = {}
    for name, fields in found:
        require(errors, name not in records, f"duplicate record {name}")
        records[name] = fields
    return records


def load_golden(errors: list[str]) -> dict[str, str] | None:
    try:
        values = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        errors.append(f"invalid sustained workload golden: {error}")
        return None
    if not isinstance(values, dict) or not all(
            isinstance(key, str) and isinstance(value, str)
            for key, value in values.items()):
        errors.append("sustained workload golden values malformed")
        return None
    return values


def validate(raw_path: Path, status_path: Path, expected_source_sha: str,
             validator_fixture: bool = False) -> list[str]:
    errors: list[str] = []
    evidence_class = "VALIDATOR_FIXTURE" if validator_fixture else "PHYSICAL_EXEC"
    require(errors, valid_source_git_sha(expected_source_sha),
            "expected source SHA must be 40 lowercase hex digits")
    try:
        regions = validate_capture_status_v2(
            raw_path, status_path, PASS_LINE, FAIL_LINE,
            AUTHORITATIVE_PREFIXES, "5D3 S1", errors)
    except OSError as error:
        return [f"capture read failed: {error}"]
    if regions is None:
        return errors
    validate_region_fatal_safety(regions.canonical, "canonical 5D3 S1 prefix", errors)
    validate_structural_tail(
        regions.tail, PASS_LINE, FAIL_LINE, AUTHORITATIVE_PREFIXES, errors)
    lines = regions.canonical.splitlines()
    real_guest = [line for line in lines if line.startswith(REAL_GUEST_PREFIX)]
    require(errors, real_guest == [REAL_GUEST_PASS_LINE],
            "canonical REAL_GUEST_RESULT must be exactly one PASS")
    require(errors, bool(lines) and lines[-1] == PASS_LINE,
            "canonical prefix does not end at COMPLETE terminal line")
    finish_indices = [i for i, line in enumerate(lines)
                      if line.startswith(b"5D3_S1_FINISH ")]
    result_indices = [i for i, line in enumerate(lines)
                      if line.startswith(REAL_GUEST_PREFIX)]
    require(errors, len(finish_indices) == 1 and len(result_indices) == 1 and
            finish_indices[0] < result_indices[0] < len(lines) - 1,
            "canonical finish/result/terminal order mismatch")
    records = parse_records(regions.canonical, errors)
    if set(records) != set(RECORD_ORDER):
        return errors
    identity, start, stream, progress, finish = (
        records[name] for name in RECORD_ORDER)
    for name, fields in records.items():
        require(errors, fields["schema"] == "3", f"{name}: schema mismatch")
        require(errors, fields["evidence_class"] == evidence_class,
                f"{name}: evidence class mismatch")

    golden = load_golden(errors)
    if golden is None:
        return errors
    expected_identity = {
        "schema": "3", "evidence_class": evidence_class,
        "source_git_sha": expected_source_sha,
        "profile": "AUDIO86_REAL_GUEST_SUSTAINED_2S_PHYSICAL_I2S",
        "workload_id": "FULL_REPLAY_PCM_SUSTAINED_2S_V1",
        "board": "P4_NANO_P4_V1X", "backend": "IDF_I2S0_ES8311",
        "display": "DISABLED",
        "guest_program_bytes": golden.get("SUSTAINED_GUEST_PROGRAM_SERIALIZED_BYTES", ""),
        "guest_program_crc32": golden.get("SUSTAINED_GUEST_PROGRAM_CRC32", ""),
        "guest_program_sha256": golden.get("SUSTAINED_GUEST_PROGRAM_SHA256", ""),
    }
    require(errors, identity == expected_identity, "physical/workload identity mismatch")
    require(errors, valid_source_git_sha(identity["source_git_sha"]),
            "source Git SHA format mismatch")

    expected_start = {
        "rate_hz": "48000", "channels": "2", "sample_bits": "16",
        "encoding": "S16LE", "i2s_format": "PHILIPS",
        "clock_source": "APLL", "mclk_multiple": "256",
        "mclk_hz": "12288000", "q_frames": "240",
        "bytes_per_frame": "4", "physical_unit_bytes": "960",
        "dma_desc": "4", "dma_frames": "240", "ring_capacity": "8",
        "prefill": "4", "semantic_duration_ms": "2000",
        "expected_units": "400", "prepare_completed": "1",
        "pa_initial_low": "1", "codec_initialized_muted": "1",
        "i2s_initialized": "1", "muted_warmup_completed": "1",
        "callbacks_registered": "1", "stream_started": "1",
        "codec_unmute_completed": "1", "startup_durations_valid": "1",
    }
    require(errors, all(start.get(key) == value
                        for key, value in expected_start.items()),
            "start geometry/history mismatch")

    enum_fields = {
        "schema", "evidence_class", "encoding", "i2s_format", "clock_source",
        "timing_authority", "controller_state", "sink_state",
        "first_qovf_state", "first_qovf_phase", "qovf_time_semantics",
        "first_qovf_wait_reason", "first_qovf_last_resume_reason",
        "notification_state_model", "cpu0_task_identity",
        "service_phase_semantics",
    }
    digest_fields = {key for key in stream if key.endswith("crc32") or
                     key.endswith("sha256")}
    numeric: dict[str, int] = {}
    for label, name, fields in (
        ("start", "5D3_S1_START", start),
        ("stream", "5D3_S1_STREAM", stream),
        ("progress", "5D3_S1_PROGRESS", progress),
        ("finish", "5D3_S1_FINISH", finish),
    ):
        for key in RECORD_FIELDS[name]:
            if key not in enum_fields and key not in digest_fields:
                numeric[f"{label}.{key}"] = decimal(fields, key, errors)
    for key in digest_fields:
        pattern = SHA256 if key.endswith("sha256") else CRC32
        require(errors, pattern.fullmatch(stream[key]) is not None,
                f"{key}: digest format mismatch")

    golden_pairs = {
        "generated_crc32": "FULL_REPLAY_PCM_CRC32",
        "generated_sha256": "FULL_REPLAY_PCM_SHA256",
        "accepted_crc32": "FULL_REPLAY_PCM_CRC32",
        "accepted_sha256": "FULL_REPLAY_PCM_SHA256",
        "io_crc32": "GUEST_IO_CRC32", "io_sha256": "GUEST_IO_SHA256",
        "event_crc32": "AUDIO_EVENTS_CRC32",
        "event_sha256": "AUDIO_EVENTS_SHA256",
        "timer_crc32": "TIMER_PIC_CRC32",
        "timer_sha256": "TIMER_PIC_SHA256",
        "action_crc32": "WORKER_APPLY_TRACE_CRC32",
        "action_sha256": "WORKER_APPLY_TRACE_SHA256",
        "final_state_crc32": "FINAL_G_STATE_CRC32",
        "final_state_sha256": "FINAL_G_STATE_SHA256",
    }
    require(errors, all(stream[field] == golden.get(golden_key)
                        for field, golden_key in golden_pairs.items()),
            "sustained PCM/trace golden mismatch")
    expected_counts = {
        "io_count": "GUEST_IO_SEMANTIC_COUNT",
        "event_count": "AUDIO_EVENTS_SEMANTIC_COUNT",
        "timer_count": "TIMER_PIC_SEMANTIC_COUNT",
        "action_count": "WORKER_APPLY_TRACE_SEMANTIC_COUNT",
        "final_state_count": "FINAL_G_STATE_SEMANTIC_COUNT",
    }
    require(errors, all(stream[field] == golden.get(golden_key)
                        for field, golden_key in expected_counts.items()),
            "sustained trace count mismatch")

    try:
        frames = int(golden.get("FULL_REPLAY_PCM_FRAMES", "-1"))
        byte_count = int(golden.get("FULL_REPLAY_PCM_BYTES", "-1"))
        units = int(golden.get("SUSTAINED_Q240_UNITS", "-1"))
    except ValueError:
        errors.append("sustained golden geometry is not decimal")
        return errors
    require(errors, frames == 96000 and byte_count == 384000 and units == 400,
            "frozen sustained golden geometry mismatch")
    require(errors,
            numeric["stream.generated_frames"] == frames and
            numeric["stream.generated_bytes"] == byte_count and
            numeric["stream.accepted_frames"] == frames and
            numeric["stream.accepted_bytes"] == byte_count and
            numeric["stream.controller_accepted_frames"] == frames and
            numeric["stream.controller_accepted_bytes"] == byte_count and
            numeric["stream.sink_accepted_frames"] == frames and
            numeric["stream.sink_accepted_bytes"] == byte_count,
            "multi-layer frame/byte ownership mismatch")
    require(errors,
            numeric["stream.generated_units"] == units and
            numeric["stream.accepted_units"] == units and
            numeric["stream.next_generated_sequence"] == units and
            numeric["stream.next_accepted_sequence"] == units and
            numeric["stream.next_generated_frame_offset"] == frames and
            numeric["stream.next_accepted_frame_offset"] == frames and
            numeric["stream.generated_slot_fill_frames"] == 0,
            "sequence/unit conservation mismatch")
    require(errors,
            numeric["stream.first_sequence"] == 0 and
            numeric["stream.first_offset"] == 0 and
            numeric["stream.first_valid_frames"] == 240 and
            stream["first_crc32"] == "b58ed112" and
            numeric["stream.final_sequence"] == 399 and
            numeric["stream.final_offset"] == 95760 and
            numeric["stream.final_slot_valid_frames"] == 240 and
            stream["final_crc32"] == "6d08c1ec",
            "bounded slot fingerprint mismatch")
    require(errors,
            numeric["stream.pre_reset_frames"] == 95761 and
            numeric["stream.pre_reset_bytes"] == 383044 and
            stream["pre_reset_crc32"] == "c65c7a5d" and
            stream["pre_reset_sha256"] == PRE_RESET_SHA256 and
            numeric["stream.reset_frame"] == 95761 and
            numeric["stream.reset_sequence"] == 18 and
            numeric["stream.reset_ordinal"] == 1 and
            numeric["stream.reset_opcode"] == 2147483648,
            "pre-reset/reset identity mismatch")

    physical_units = numeric["stream.physical_units"]
    retry_count = numeric["stream.retry_count"]
    require(errors,
            physical_units == units and numeric["stream.full_units"] == units and
            numeric["stream.final_partial_units"] == 0 and
            numeric["stream.final_valid_frames"] == 0 and
            numeric["stream.padding_frames"] == 0 and
            numeric["stream.padding_bytes"] == 0,
            "physical full-unit geometry mismatch")
    require(errors, retry_count >= 0 and
            numeric["stream.submit_attempts"] == physical_units + retry_count and
            numeric["stream.retry_identity_failures"] == 0,
            "retry arithmetic/identity mismatch")
    require(errors, numeric["stream.running_q_ovf"] == 0,
            "RUNNING queue overflow must be zero")
    require(errors, all(numeric[f"stream.{key}"] == 0 for key in (
        "final_ring_occupancy", "final_ring_partial", "drops", "overwrite",
        "abandoned_published", "abandoned_partial", "abandoned_rendered")),
        "ring/drop/abandonment mismatch")

    require(errors, 0 <= numeric["progress.pcm_ring_max_occupancy"] <= 8,
            "ring maximum occupancy outside structural bound")
    require(errors, progress["timing_authority"] == "HOST_ONLY",
            "realtime authority mismatch")
    started = numeric["progress.stream_started_ms"]
    completed = numeric["progress.drain_completed_ms"]
    wall = numeric["progress.stream_wall_ms"]
    require(errors, completed >= started and wall == completed - started,
            "stream wall arithmetic mismatch")
    require(errors, wall <= 2040,
            "stream wall exceeds 2000 ms + 40 ms project bound")
    require(errors, numeric["progress.max_running_accept_gap_ms"] <= 40,
            "RUNNING accepted-progress gap exceeds 8 q240 x 5 ms bound")
    preloaded = numeric["progress.preloaded_units"]
    running_units = numeric["progress.running_accepted_units"]
    require(errors, preloaded == numeric["start.prefill"] == 4 and
            running_units == physical_units - preloaded == 396,
            "preload/RUNNING accepted-unit arithmetic mismatch")
    require(errors,
            numeric["stream.retry_episode_units"] >= 0 and
            numeric["stream.retry_episode_units"] <= retry_count and
            numeric["stream.direct_running_accept_units"] >= 0 and
            numeric["stream.retry_episode_units"] +
                numeric["stream.direct_running_accept_units"] == running_units,
            "RUNNING retry distribution arithmetic mismatch")

    gap = numeric["progress.max_running_accept_gap_ms"]
    gap_initial = numeric["progress.max_gap_initial"]
    previous_valid = numeric["progress.max_gap_previous_sequence_valid"]
    previous_sequence = numeric["progress.max_gap_previous_sequence"]
    next_sequence = numeric["progress.max_gap_next_sequence"]
    previous_relative = numeric["progress.max_gap_previous_relative_ms"]
    next_relative = numeric["progress.max_gap_next_relative_ms"]
    require(errors, gap_initial in (0, 1) and previous_valid in (0, 1),
            "max-gap sentinel flags malformed")
    require(errors, next_relative >= previous_relative and
            next_relative - previous_relative == gap,
            "max-gap endpoint arithmetic mismatch")
    require(errors, preloaded <= next_sequence < units,
            "max-gap next sequence outside RUNNING range")
    if gap_initial:
        require(errors, previous_valid == 0 and previous_sequence == 0 and
                previous_relative == 0 and next_sequence == preloaded,
                "initial max-gap sentinel/sequence mismatch")
    else:
        require(errors, previous_valid == 1 and
                preloaded <= previous_sequence < units and
                next_sequence == previous_sequence + 1,
                "max-gap sequence mismatch")
    for duration_name, sequence_name in (
        ("max_downstream_submit_us", "max_downstream_submit_sequence"),
        ("max_post_accept_evidence_us", "max_post_accept_evidence_sequence"),
    ):
        require(errors,
                0 <= numeric[f"progress.{duration_name}"] <= 0xFFFFFFFF,
                f"{duration_name}: duration outside uint32 range")
        require(errors,
                preloaded <= numeric[f"progress.{sequence_name}"] < units,
                f"{sequence_name}: sequence outside RUNNING range")
    require(errors,
            numeric["start.startup_durations_valid"] == 1 and
            0 <= numeric["start.enable_stream_duration_us"] <= 0xFFFFFFFF and
            0 <= numeric["start.codec_unmute_duration_us"] <= 0xFFFFFFFF,
            "startup duration diagnostics malformed")

    latch = numeric["finish.first_active_qovf_latched"]
    observed = numeric["finish.first_qovf_observed"]
    require(errors, latch in (0, 1) and observed in (0, 1),
            "first q_ovf latch flags malformed")
    require(errors,
            finish["qovf_time_semantics"] ==
                "TASK_PUBLISHED_RELATIVE_US_NO_ISR_TIMER",
            "first q_ovf time semantics mismatch")
    require(errors,
            finish["first_qovf_state"] in {"NONE", "STARTING", "RUNNING"},
            "first q_ovf state invalid")
    require(errors,
            finish["first_qovf_phase"] in {
                "NONE", "START_ENABLE", "CODEC_UNMUTE",
                "DOWNSTREAM_SUBMIT", "POST_ACCEPT_EVIDENCE", "WAIT_EOF",
                "FINISH",
            }, "first q_ovf phase invalid")
    wait_reasons = {
        "RUNNABLE", "RETRY_EOF_WAIT", "PCM_RING_EMPTY_WAIT",
        "PCM_PREFILL_WAIT", "FINISH_OR_TERMINAL_WAIT",
    }
    require(errors, finish["first_qovf_wait_reason"] in wait_reasons,
            "first q_ovf wait reason invalid")
    require(errors, finish["first_qovf_last_resume_reason"] in wait_reasons,
            "first q_ovf last resume reason invalid")
    require(errors,
            finish["notification_state_model"] ==
                "PROJECT_WAIT_REASON_AND_COUNTERS" and
            finish["cpu0_task_identity"] == "UNAVAILABLE_SAFELY" and
            finish["service_phase_semantics"] ==
                "LAST_SERVICE_NOT_WAIT_REASON",
            "final-boundary diagnostic semantics mismatch")
    require(errors,
            0 <= numeric["finish.first_qovf_eof_epoch"] <= 0xFFFFFFFF and
            0 <= numeric["finish.first_qovf_current_sequence"] < units and
            0 <= numeric["finish.first_qovf_published_sequence"] <= units and
            numeric["finish.first_qovf_published_sequence"] <=
                numeric["finish.first_qovf_current_sequence"] + 1,
            "first q_ovf sequence/epoch range mismatch")
    qovf_times = tuple(numeric[f"finish.{name}"] for name in (
        "first_qovf_last_step_enter_us", "first_qovf_last_submit_return_us",
        "first_qovf_last_step_exit_us", "first_qovf_last_running_accepted_us",
    ))
    require(errors, all(0 <= value <= 0xFFFFFFFF for value in qovf_times) and
            0 <= numeric["finish.first_qovf_observed_us"] <= 0xFFFFFFFF,
            "first q_ovf progress timestamp outside uint32 range")
    consumer_sequence = numeric["finish.first_qovf_consumer_next_sequence"]
    published_sequence = numeric["finish.first_qovf_next_published_sequence"]
    ring_occupancy = numeric["finish.first_qovf_ring_occupancy"]
    production_done = numeric["finish.first_qovf_production_done"]
    rendered_frames = numeric["finish.first_qovf_rendered_frames"]
    q399_rendered = numeric["finish.first_qovf_q399_rendered"]
    q399_published = numeric["finish.first_qovf_q399_published"]
    q399_available = numeric["finish.first_qovf_q399_available"]
    eof_notify = numeric["finish.first_qovf_eof_notify_count"]
    hpwoken = numeric["finish.first_qovf_hpwoken_true_count"]
    retry_enter = numeric["finish.first_qovf_retry_wait_enter_count"]
    retry_resume = numeric["finish.first_qovf_retry_wait_resume_count"]
    ring_enter = numeric["finish.first_qovf_ring_wait_enter_count"]
    ring_resume = numeric["finish.first_qovf_ring_wait_resume_count"]
    last_wait_enter = numeric["finish.first_qovf_last_wait_enter_us"]
    last_wait_resume = numeric["finish.first_qovf_last_wait_resume_us"]
    last_resume_sequence = numeric["finish.first_qovf_last_resume_sequence"]
    require(errors,
            0 <= consumer_sequence <= units and
            0 <= published_sequence <= units and
            0 <= ring_occupancy <= numeric["start.ring_capacity"] and
            production_done in (0, 1) and
            0 <= rendered_frames <= frames and
            q399_rendered in (0, 1) and q399_published in (0, 1) and
            q399_available in (0, 1),
            "final-boundary ring state outside structural bounds")
    require(errors,
            q399_rendered == int(rendered_frames >= frames) and
            q399_published == int(published_sequence >= units) and
            q399_available == int(
                q399_published == 1 and consumer_sequence < units and
                ring_occupancy > 0),
            "q399 derived-state inconsistency")
    require(errors, production_done == 0 or published_sequence == units,
            "production-done publication inconsistency")
    require(errors, hpwoken <= eof_notify,
            "higher-priority-woken count exceeds EOF notifications")
    require(errors, retry_resume <= retry_enter and ring_resume <= ring_enter,
            "wait resume count exceeds wait entry count")
    require(errors, 0 <= last_wait_enter <= 0xFFFFFFFF and
            0 <= last_wait_resume <= 0xFFFFFFFF and
            0 <= last_resume_sequence <= units,
            "wait/resume diagnostic range mismatch")
    wait_reason = finish["first_qovf_wait_reason"]
    if wait_reason == "RUNNABLE":
        require(errors, retry_enter == retry_resume and ring_enter == ring_resume,
                "RUNNABLE snapshot has an unmatched wait entry")
        if retry_resume + ring_resume > 0:
            require(errors, last_wait_resume >= last_wait_enter,
                    "RUNNABLE snapshot resumes before latest wait entry")
    elif wait_reason == "RETRY_EOF_WAIT":
        require(errors, consumer_sequence < units and
                retry_enter == retry_resume + 1 and
                ring_enter == ring_resume and
                last_wait_enter >= last_wait_resume,
                "RETRY EOF wait tuple is impossible")
    elif wait_reason in {"PCM_RING_EMPTY_WAIT", "PCM_PREFILL_WAIT"}:
        occupancy_matches_reason = (ring_occupancy == 0
            if wait_reason == "PCM_RING_EMPTY_WAIT"
            else ring_occupancy < numeric["start.prefill"])
        require(errors, occupancy_matches_reason and production_done == 0 and
                ring_enter == ring_resume + 1 and
                retry_enter == retry_resume and
                last_wait_enter >= last_wait_resume,
                "PCM ring wait tuple is impossible")
    else:
        require(errors, retry_enter == retry_resume and ring_enter == ring_resume,
                "terminal snapshot has an unmatched wait entry")
    if retry_resume + ring_resume == 0:
        require(errors, last_wait_resume == 0 and
                finish["first_qovf_last_resume_reason"] == "RUNNABLE" and
                last_resume_sequence == 0,
                "zero-resume snapshot contains resume diagnostics")
    else:
        require(errors,
                finish["first_qovf_last_resume_reason"] in {
                    "RETRY_EOF_WAIT", "PCM_RING_EMPTY_WAIT", "PCM_PREFILL_WAIT",
                } and last_resume_sequence < units,
                "last resume tuple is impossible")
    if numeric["stream.running_q_ovf"] > 0:
        require(errors, latch == 1 and observed == 1 and
                finish["first_qovf_state"] in {"STARTING", "RUNNING"} and
                finish["first_qovf_phase"] != "NONE" and
                numeric["finish.first_qovf_observed_us"] >= max(qovf_times),
                "active q_ovf lacks a complete frozen service latch")
    else:
        require(errors, latch == 0 and observed == 0 and
                finish["first_qovf_state"] == "NONE" and
                finish["first_qovf_phase"] == "NONE" and
                numeric["finish.first_qovf_eof_epoch"] == 0 and
                numeric["finish.first_qovf_current_sequence"] == 0 and
                numeric["finish.first_qovf_published_sequence"] == 0 and
                all(value == 0 for value in qovf_times) and
                all(numeric[f"finish.{name}"] == 0 for name in (
                    "first_qovf_consumer_next_sequence",
                    "first_qovf_next_published_sequence",
                    "first_qovf_q399_rendered", "first_qovf_q399_published",
                    "first_qovf_q399_available", "first_qovf_ring_occupancy",
                    "first_qovf_production_done", "first_qovf_rendered_frames",
                    "first_qovf_eof_notify_count",
                    "first_qovf_hpwoken_true_count",
                    "first_qovf_retry_wait_enter_count",
                    "first_qovf_retry_wait_resume_count",
                    "first_qovf_ring_wait_enter_count",
                    "first_qovf_ring_wait_resume_count",
                    "first_qovf_last_wait_enter_us",
                    "first_qovf_last_wait_resume_us",
                    "first_qovf_last_resume_sequence",
                )) and
                finish["first_qovf_wait_reason"] == "RUNNABLE" and
                finish["first_qovf_last_resume_reason"] == "RUNNABLE" and
                numeric["finish.first_qovf_observed_us"] == 0,
                "zero-q_ovf run contains a spurious first-fault latch")

    final_epoch = numeric["finish.final_copy_eof_epoch"]
    drain_epoch = numeric["finish.drain_completion_eof_epoch"]
    quiescent_epoch = numeric["finish.quiescent_eof_epoch"]
    require(errors, all(0 <= value <= 0xFFFFFFFF
                        for value in (final_epoch, drain_epoch, quiescent_epoch)),
            "EOF epoch outside uint32 range")
    drain_delta = uint32_delta(drain_epoch, final_epoch)
    quiescent_delta = uint32_delta(quiescent_epoch, final_epoch)
    require(errors,
            numeric["finish.drain_post_snapshot_eofs"] == drain_delta and
            numeric["finish.quiescent_post_snapshot_eofs"] == quiescent_delta,
            "reported EOF interval delta mismatch")
    require(errors, drain_delta < UINT32_HALF_RANGE and
            quiescent_delta < UINT32_HALF_RANGE,
            "EOF interval has half-range ambiguity")
    require(errors, drain_delta >= numeric["start.dma_desc"],
            "physical drain EOF proof is insufficient")
    require(errors, quiescent_delta >= drain_delta,
            "quiescent observation precedes drain completion")
    require(errors, numeric["finish.draining_q_ovf"] <= quiescent_delta,
            "DRAINING q_ovf exceeds quiescent interval")
    require(errors, numeric["finish.drain_duration_ms"] < 40,
            "physical terminal drain timeout reached")
    require(errors, finish["controller_state"] == "FINISHED" and
            finish["sink_state"] == "QUIESCENT",
            "physical terminal state mismatch")
    fixed_finish = {
        "finish_completed": 1, "pending_frames": 0,
        "drained_frames": frames, "discarded_frames": 0, "sticky_error": 0,
        "stale_callbacks": 0, "callback_in_flight": 0, "callbacks_active": 0,
        "codec_final_muted": 1, "pa_final_low": 1, "i2s_enabled": 0,
        "i2s_created": 0, "first_error": 0, "forced_abort": 0,
        "sink_destroyed": 1,
    }
    require(errors, all(numeric[f"finish.{key}"] == value
                        for key, value in fixed_finish.items()),
            "finish/quiescence health mismatch")
    require(errors, numeric["finish.registered_generation"] > 0 and
            numeric["finish.terminal_generation"] ==
                numeric["finish.registered_generation"] + 1,
            "callback generation mismatch")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--expected-source-sha", required=True)
    parser.add_argument("--validator-fixture", action="store_true",
                        help="accept VALIDATOR_FIXTURE records in software tests")
    args = parser.parse_args()
    errors = validate(args.raw, args.status, args.expected_source_sha,
                      args.validator_fixture)
    if errors:
        for error in errors:
            print(f"5D3_S1_PHYSICAL_EVIDENCE_ERROR={error}", file=sys.stderr)
        return 1
    if args.validator_fixture:
        print("F3_VALIDATOR_FIXTURE_VALIDATION=PASS")
    else:
        print("F3_PHYSICAL_EVIDENCE_VALIDATION=PASS")
        print("F3_REALTIME_CLAIM=PROJECT_LEVEL_SEMANTIC_DELIVERY_PROGRESS_ONLY")
    print("F3_CAPTURE_STATUS_V2_REUSE=PASS")
    print("F3_HOST_GOLDEN_BINDING=PASS")
    print("F3_PHYSICAL_STREAM_GEOMETRY=PASS")
    print("F3_RETRY_ACCEPTANCE_POLICY=PASS")
    print("F3_EOF_MODULAR_ARITHMETIC=PASS")
    print("F3_OWNERSHIP_CONSERVATION=PASS")
    print("F3_PHYSICAL_REALTIME_ACCEPTANCE=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
