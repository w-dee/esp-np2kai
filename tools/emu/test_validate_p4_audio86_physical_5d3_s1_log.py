#!/usr/bin/env python3
"""Change-sensitivity tests for the 86R.5D.3 S1 physical validator."""

from __future__ import annotations

import hashlib
import json
import re
import tempfile
from pathlib import Path

from p4_audio86_physical_capture_v2 import raw_lines
from validate_p4_audio86_physical_5d3_s1_log import RECORD_FIELDS, validate


ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "host/probe/audio86_guest_sustained_2s_golden.json"
BINDING = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/"
    "p4_nano_audio86_guest_binding.cpp"
)
EXPECTED_SHA = "0123456789abcdef0123456789abcdef01234567"
PASS_MARKER = "P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=COMPLETE"
FAIL_MARKER = "P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=FAILED"
BOOT = (
    "I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader",
    "I (1558) main_task: Calling app_main()",
)


def require_source_schema_sync() -> None:
    source = BINDING.read_text(encoding="utf-8")
    begin = source.index("void emit_physical_5d3_s1_evidence(")
    end = source.index("sustained_physical_local_health(", begin)
    evidence = source[begin:end]
    positions = [evidence.index(f'"{name} ') for name in RECORD_FIELDS]
    for index, name in enumerate(RECORD_FIELDS):
        stop = positions[index + 1] if index + 1 < len(positions) else len(evidence)
        block = evidence[positions[index]:stop]
        literal = "".join(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', block))
        fields = tuple(re.findall(r"(?:^| )([a-z0-9_]+)=", literal))
        if fields != RECORD_FIELDS[name]:
            raise AssertionError(f"validator/source schema mismatch for {name}: {fields!r}")


def canonical_lines(*, wall_ms: int = 2040, gap_ms: int = 40,
                    evidence_class: str = "VALIDATOR_FIXTURE") -> list[str]:
    g = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]
    return [
        "5D3_S1_IDENTITY schema=1 "
        f"evidence_class={evidence_class} source_git_sha={EXPECTED_SHA} "
        "profile=AUDIO86_REAL_GUEST_SUSTAINED_2S_PHYSICAL_I2S "
        "workload_id=FULL_REPLAY_PCM_SUSTAINED_2S_V1 "
        "board=P4_NANO_P4_V1X backend=IDF_I2S0_ES8311 display=DISABLED "
        f"guest_program_bytes={g['SUSTAINED_GUEST_PROGRAM_SERIALIZED_BYTES']} "
        f"guest_program_crc32={g['SUSTAINED_GUEST_PROGRAM_CRC32']} "
        f"guest_program_sha256={g['SUSTAINED_GUEST_PROGRAM_SHA256']}",
        "5D3_S1_START schema=1 "
        f"evidence_class={evidence_class} rate_hz=48000 channels=2 "
        "sample_bits=16 encoding=S16LE i2s_format=PHILIPS clock_source=APLL "
        "mclk_multiple=256 mclk_hz=12288000 q_frames=240 bytes_per_frame=4 "
        "physical_unit_bytes=960 dma_desc=4 dma_frames=240 ring_capacity=8 "
        "prefill=4 semantic_duration_ms=2000 expected_units=400 "
        "prepare_completed=1 pa_initial_low=1 codec_initialized_muted=1 "
        "i2s_initialized=1 muted_warmup_completed=1 callbacks_registered=1 "
        "stream_started=1 codec_unmute_completed=1",
        "5D3_S1_STREAM schema=1 "
        f"evidence_class={evidence_class} generated_frames=96000 "
        f"generated_bytes=384000 generated_crc32={g['FULL_REPLAY_PCM_CRC32']} "
        f"generated_sha256={g['FULL_REPLAY_PCM_SHA256']} accepted_frames=96000 "
        f"accepted_bytes=384000 accepted_crc32={g['FULL_REPLAY_PCM_CRC32']} "
        f"accepted_sha256={g['FULL_REPLAY_PCM_SHA256']} generated_units=400 "
        "accepted_units=400 next_generated_sequence=400 next_accepted_sequence=400 "
        "next_generated_frame_offset=96000 next_accepted_frame_offset=96000 "
        "generated_slot_fill_frames=0 first_sequence=0 first_offset=0 "
        "first_valid_frames=240 first_crc32=b58ed112 final_sequence=399 "
        "final_offset=95760 final_slot_valid_frames=240 final_crc32=6d08c1ec "
        "pre_reset_frames=95761 pre_reset_bytes=383044 pre_reset_crc32=c65c7a5d "
        "reset_frame=95761 reset_sequence=18 reset_ordinal=1 reset_opcode=2147483648 "
        f"io_count={g['GUEST_IO_SEMANTIC_COUNT']} io_crc32={g['GUEST_IO_CRC32']} "
        f"io_sha256={g['GUEST_IO_SHA256']} "
        f"event_count={g['AUDIO_EVENTS_SEMANTIC_COUNT']} "
        f"event_crc32={g['AUDIO_EVENTS_CRC32']} "
        f"event_sha256={g['AUDIO_EVENTS_SHA256']} "
        f"timer_count={g['TIMER_PIC_SEMANTIC_COUNT']} "
        f"timer_crc32={g['TIMER_PIC_CRC32']} timer_sha256={g['TIMER_PIC_SHA256']} "
        f"action_count={g['WORKER_APPLY_TRACE_SEMANTIC_COUNT']} "
        f"action_crc32={g['WORKER_APPLY_TRACE_CRC32']} "
        f"action_sha256={g['WORKER_APPLY_TRACE_SHA256']} "
        f"final_state_count={g['FINAL_G_STATE_SEMANTIC_COUNT']} "
        f"final_state_crc32={g['FINAL_G_STATE_CRC32']} "
        f"final_state_sha256={g['FINAL_G_STATE_SHA256']} "
        "controller_accepted_frames=96000 controller_accepted_bytes=384000 "
        "sink_accepted_frames=96000 sink_accepted_bytes=384000 physical_units=400 "
        "full_units=400 final_partial_units=0 final_valid_frames=0 "
        "padding_frames=0 padding_bytes=0 submit_attempts=401 retry_count=1 "
        "retry_identity_failures=0 running_q_ovf=0 final_ring_occupancy=0 "
        "final_ring_partial=0 drops=0 overwrite=0 abandoned_published=0 "
        "abandoned_partial=0 abandoned_rendered=0",
        "5D3_S1_PROGRESS schema=1 "
        f"evidence_class={evidence_class} pcm_ring_max_occupancy=8 "
        "pcm_producer_full_wait_count=7 "
        "pcm_consumer_empty_after_release_before_done_count=2 "
        f"max_running_accept_gap_ms={gap_ms} stream_started_ms=1000 "
        f"drain_completed_ms={1000 + wall_ms} stream_wall_ms={wall_ms} "
        "preloaded_units=4 running_accepted_units=396 timing_authority=HOST_ONLY",
        "5D3_S1_FINISH schema=1 "
        f"evidence_class={evidence_class} controller_state=FINISHED "
        "sink_state=QUIESCENT final_copy_eof_epoch=4294967294 "
        "drain_completion_eof_epoch=2 quiescent_eof_epoch=4 "
        "drain_post_snapshot_eofs=4 quiescent_post_snapshot_eofs=6 "
        "drain_duration_ms=4 finish_completed=1 pending_frames=0 "
        "drained_frames=96000 discarded_frames=0 draining_q_ovf=2 "
        "sticky_error=0 registered_generation=1 terminal_generation=2 "
        "stale_callbacks=0 callback_in_flight=0 callbacks_active=0 "
        "codec_final_muted=1 pa_final_low=1 i2s_enabled=0 i2s_created=0 "
        "first_error=0 forced_abort=0 sink_destroyed=1",
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS",
        PASS_MARKER,
    ]


def replace_once(lines: list[str], old: str, new: str) -> list[str]:
    text = "\n".join(lines)
    if text.count(old) != 1:
        raise AssertionError(f"non-unique mutation target: {old!r}")
    return text.replace(old, new, 1).split("\n")


def status_v2(raw: bytes, path: Path, reset_offset: int) -> dict[str, object]:
    occurrences = []
    for line in raw_lines(raw, reset_offset):
        if line.complete and line.content in {
                PASS_MARKER.encode(), FAIL_MARKER.encode()}:
            occurrences.append((line.start, line.end,
                                "PASS" if line.content == PASS_MARKER.encode()
                                else "FAIL"))
    if occurrences:
        start, end, kind = occurrences[0]
    else:
        start, end, kind = None, None, "NOT_OBSERVED"
    return {
        "schema_version": 2, "raw_path": str(path), "raw_bytes": len(raw),
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "reset_byte_offset": reset_offset, "reset_count": 1,
        "terminal_status": kind, "terminal_marker": kind if occurrences else None,
        "terminal_line_start_offset": start, "terminal_line_end_offset": end,
        "terminal_line_complete": bool(occurrences),
        "terminal_pass_marker_config": PASS_MARKER,
        "terminal_fail_marker_config": FAIL_MARKER,
        "exit_reason": f"TERMINAL_{kind}" if occurrences else "HARD_TIMEOUT",
        "state": f"TERMINAL_{kind}" if occurrences else "HARD_TIMEOUT",
        "post_terminal_drain_seconds": 0.5, "idle_timeout_enabled": False,
        "serial_error": None, "raw_final_line_complete": raw.endswith(b"\n"),
    }


def run_case(directory: Path, lines: list[str], *, tail: bytes = b"",
             status_changes: dict[str, object] | None = None,
             fixture: bool = True) -> list[str]:
    reset_prefix = b"setup boot\r\n"
    raw = reset_prefix + ("\r\n".join((*BOOT, *lines)) + "\r\n").encode() + tail
    raw_path = directory / "capture.raw"
    status_path = directory / "capture.status.json"
    raw_path.write_bytes(raw)
    status = status_v2(raw, raw_path, len(reset_prefix))
    if status_changes:
        status.update(status_changes)
    status_path.write_text(json.dumps(status), encoding="utf-8")
    return validate(raw_path, status_path, EXPECTED_SHA, fixture)


def main() -> int:
    require_source_schema_sync()
    base = canonical_lines()
    mutations: dict[str, list[str]] = {
        "A_missing_record": base[:2] + base[3:],
        "A_duplicate_record": base[:2] + [base[1]] + base[2:],
        "A_reorder": [base[1], base[0], *base[2:]],
        "A_unknown_record": [base[0], "5D3_S1_UNKNOWN schema=1", *base[1:]],
        "A_cross_namespace_terminal": [
            *base[:-2], "P4_AUDIO86_PHYSICAL_S2_TERMINAL=COMPLETE", *base[-2:]],
        "A_unknown_field": replace_once(base, " codec_unmute_completed=1",
                                         " codec_unmute_completed=1 unknown=1"),
        "A_duplicate_field": replace_once(base, " prefill=4", " prefill=4 prefill=4"),
        "A_missing_field": replace_once(base, " codec_unmute_completed=1", ""),
        "A_malformed": replace_once(base, "rate_hz=48000", "rate_hz=x"),
        "B_source": replace_once(base, EXPECTED_SHA, "1" * 40),
        "B_workload": replace_once(base, "FULL_REPLAY_PCM_SUSTAINED_2S_V1", "OTHER"),
        "B_guest": replace_once(base, "guest_program_crc32=e577580a", "guest_program_crc32=00000000"),
        "B_profile": replace_once(base, "profile=AUDIO86_REAL_GUEST_SUSTAINED_2S_PHYSICAL_I2S", "profile=OTHER"),
        "B_board": replace_once(base, "board=P4_NANO_P4_V1X", "board=GENERIC"),
        "B_backend": replace_once(base, "backend=IDF_I2S0_ES8311", "backend=FAKE"),
        "B_class": replace_once(
            base, "5D3_S1_IDENTITY schema=1 evidence_class=VALIDATOR_FIXTURE",
            "5D3_S1_IDENTITY schema=1 evidence_class=HOST_EXEC"),
        "C_generated_frames": replace_once(base, "generated_frames=96000", "generated_frames=95999"),
        "C_generated_crc": replace_once(base, "generated_crc32=5bb15277", "generated_crc32=0bb15277"),
        "C_accepted_sha": replace_once(base, "accepted_sha256=b315", "accepted_sha256=a315"),
        "C_accepted_count": replace_once(
            base, "accepted_frames=96000 accepted_bytes=384000",
            "accepted_frames=95999 accepted_bytes=384000"),
        "D_units": replace_once(base, "generated_units=400", "generated_units=399"),
        "D_sequence": replace_once(base, "next_accepted_sequence=400", "next_accepted_sequence=399"),
        "D_first": replace_once(base, "first_crc32=b58ed112", "first_crc32=058ed112"),
        "D_final": replace_once(base, "final_offset=95760", "final_offset=95759"),
        "E_io": replace_once(base, "io_count=246", "io_count=245"),
        "E_event": replace_once(base, "event_crc32=0388eae3", "event_crc32=1388eae3"),
        "E_timer": replace_once(base, "timer_crc32=540799b1", "timer_crc32=040799b1"),
        "E_action": replace_once(base, "action_count=19", "action_count=18"),
        "E_final": replace_once(base, "final_state_crc32=848e5e85", "final_state_crc32=048e5e85"),
        "F_reset": replace_once(base, "reset_ordinal=1", "reset_ordinal=2"),
        "F_pre_reset": replace_once(base, "pre_reset_crc32=c65c7a5d", "pre_reset_crc32=065c7a5d"),
        "G_submit": replace_once(base, "submit_attempts=401", "submit_attempts=400"),
        "G_acceptance": replace_once(base, "sink_accepted_frames=96000", "sink_accepted_frames=95760"),
        "H_running_qovf": replace_once(base, "running_q_ovf=0", "running_q_ovf=1"),
        "H_drain": replace_once(base, "drain_post_snapshot_eofs=4", "drain_post_snapshot_eofs=3"),
        "H_quiescent": replace_once(base, "quiescent_post_snapshot_eofs=6", "quiescent_post_snapshot_eofs=3"),
        "H_draining_qovf": replace_once(base, "draining_q_ovf=2", "draining_q_ovf=7"),
        "H_half_range": replace_once(
            replace_once(base, "drain_completion_eof_epoch=2", "drain_completion_eof_epoch=2147483646"),
            "drain_post_snapshot_eofs=4", "drain_post_snapshot_eofs=2147483648"),
        "I_wall": canonical_lines(wall_ms=2041),
        "I_gap": canonical_lines(gap_ms=41),
        "J_pending": replace_once(base, "pending_frames=0", "pending_frames=1"),
        "J_discarded": replace_once(base, "discarded_frames=0", "discarded_frames=1"),
        "J_drop": replace_once(base, "drops=0", "drops=1"),
        "J_overwrite": replace_once(base, "overwrite=0", "overwrite=1"),
        "J_abandon": replace_once(base, "abandoned_published=0", "abandoned_published=1"),
        "J_occupancy": replace_once(base, "final_ring_occupancy=0", "final_ring_occupancy=1"),
        "K_callback": replace_once(base, "callbacks_active=0", "callbacks_active=1"),
        "K_sticky": replace_once(base, "sticky_error=0", "sticky_error=1"),
        "K_codec": replace_once(base, "codec_final_muted=1", "codec_final_muted=0"),
        "K_pa": replace_once(base, "pa_final_low=1", "pa_final_low=0"),
        "K_i2s": replace_once(base, "i2s_enabled=0", "i2s_enabled=1"),
        "K_destroy": replace_once(base, "sink_destroyed=1", "sink_destroyed=0"),
        "K_abort": replace_once(base, "forced_abort=0", "forced_abort=1"),
        "K_error": replace_once(base, "first_error=0", "first_error=1"),
        "L_failed": [*base[:-1], FAIL_MARKER],
        "L_missing_terminal": base[:-1],
        "L_duplicate_terminal": [*base, PASS_MARKER],
    }
    with tempfile.TemporaryDirectory(prefix="f3-validator-") as tmp:
        directory = Path(tmp)
        if run_case(directory, base):
            raise AssertionError("valid 40/2040 fixture rejected")
        if run_case(directory, canonical_lines(wall_ms=2039, gap_ms=39)):
            raise AssertionError("valid below-boundary fixture rejected")
        if not run_case(directory, base, fixture=False):
            raise AssertionError("validator fixture masqueraded as PHYSICAL_EXEC")
        for label, lines in mutations.items():
            if not run_case(directory, lines):
                raise AssertionError(f"mutation accepted: {label}")
        if not run_case(directory, base, status_changes={"schema_version": 1}):
            raise AssertionError("status v1 accepted")
        if not run_case(directory, base, status_changes={"terminal_line_start_offset": 0}):
            raise AssertionError("forged terminal offset accepted")
        if not run_case(directory, base, status_changes={"raw_sha256": "0" * 64}):
            raise AssertionError("raw/status mismatch accepted")
        for label, tail in (
            ("panic", b"Guru Meditation Error: Core 0 panic'ed\r\n"),
            ("reset", b"ESP-ROM:esp32p4\r\n"),
            ("authority", b"5D3_S1_PROGRESS schema=1\r\n"),
        ):
            if not run_case(directory, base, tail=tail):
                raise AssertionError(f"unsafe tail accepted: {label}")
        if run_case(directory, base, tail=b"I (9999) cleanup: complete\r\n"):
            raise AssertionError("benign cleanup tail rejected")
    print(f"F3_VALIDATOR_MUTATIONS={len(mutations) + 9}/PASS")
    print("F3_VALIDATOR_CHANGE_SENSITIVITY=PASS")
    print("F3_RECORD_GRAMMAR_SOURCE_SYNC=PASS")
    print("F3_REALTIME_BOUNDARIES_2040_40=PASS")
    print("F3_VALIDATOR_FIXTURE_CLASSIFICATION=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
