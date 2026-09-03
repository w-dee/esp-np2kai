#!/usr/bin/env python3
"""Change-sensitivity tests for the 86R.5D.2 S2 physical validator."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from p4_audio86_physical_capture_v2 import raw_lines
from validate_p4_audio86_physical_s2_log import RECORD_FIELDS


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_physical_s2_log.py"
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
BINDING = ROOT / (
    "firmware/components/p4_nano_audio86_guest_binding/"
    "p4_nano_audio86_guest_binding.cpp"
)
EXPECTED_SHA = "0123456789abcdef0123456789abcdef01234567"
PASS_MARKER = "P4_AUDIO86_PHYSICAL_S2_TERMINAL=COMPLETE"
FAIL_MARKER = "P4_AUDIO86_PHYSICAL_S2_TERMINAL=FAILED"
BOOT_LINES = (
    "I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader",
    "I (1558) main_task: Calling app_main()",
)


def require_source_schema_sync() -> None:
    source = BINDING.read_text(encoding="utf-8")
    begin = source.index("void emit_physical_s2_evidence(")
    end = source.index("bool physical_s2_snapshot_healthy(", begin)
    evidence = source[begin:end]
    names = tuple(RECORD_FIELDS)
    positions = [evidence.index(f'"{name} ') for name in names]
    for index, name in enumerate(names):
        block_end = positions[index + 1] if index + 1 < len(names) else len(evidence)
        block = evidence[positions[index]:block_end]
        literal = "".join(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', block))
        fields = tuple(re.findall(r"(?:^| )([a-z0-9_]+)=", literal))
        if fields != RECORD_FIELDS[name]:
            raise AssertionError(
                f"validator/source schema mismatch for {name}: {fields!r}"
            )


def canonical_lines(*, retry_count: int = 1) -> list[str]:
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]
    return [
        "5D2_S2_IDENTITY schema=1 evidence_class=PHYSICAL_EXEC "
        f"source_git_sha={EXPECTED_SHA} "
        "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S board=P4_NANO_P4_V1X "
        "backend=IDF_I2S0_ES8311 stimulus=FULL_REPLAY_PCM display=DISABLED",
        "5D2_S2_START schema=1 evidence_class=PHYSICAL_EXEC rate_hz=48000 "
        "channels=2 sample_bits=16 encoding=S16LE i2s_format=PHILIPS "
        "clock_source=APLL mclk_multiple=256 mclk_hz=12288000 q_frames=240 "
        "bytes_per_frame=4 physical_unit_bytes=960 dma_desc=4 dma_frames=240 "
        "ring_capacity=8 prefill=4 prepare_completed=1 pa_initial_low=1 "
        "codec_initialized_muted=1 i2s_initialized=1 "
        "muted_warmup_completed=1 callbacks_registered=1 stream_started=1 "
        "codec_unmute_completed=1",
        "5D2_S2_STREAM schema=1 evidence_class=PHYSICAL_EXEC "
        f"semantic_frames={golden['FULL_REPLAY_PCM_FRAMES']} "
        f"semantic_bytes={golden['FULL_REPLAY_PCM_BYTES']} "
        f"semantic_crc32={golden['FULL_REPLAY_PCM_CRC32']} "
        f"semantic_sha256={golden['FULL_REPLAY_PCM_SHA256']} "
        "produced_frames=2400 produced_bytes=9600 produced_slots=10 "
        "controller_accepted_frames=2400 controller_accepted_bytes=9600 "
        "sink_accepted_frames=2400 sink_accepted_bytes=9600 "
        "physical_units=10 full_units=10 final_partial_units=0 "
        "final_valid_frames=0 padding_frames=0 padding_bytes=0 "
        f"preloaded_units=4 running_units=6 submit_attempts={10 + retry_count} "
        f"retry_count={retry_count} running_q_ovf=0 final_ring_occupancy=0 "
        "final_ring_partial=0 drops=0 overwrite=0 abandoned_published=0 "
        "abandoned_partial=0 abandoned_rendered=0 semantic_duration_ms=50",
        "5D2_S2_FINISH schema=1 evidence_class=PHYSICAL_EXEC "
        "controller_state=FINISHED sink_state=QUIESCENT "
        "final_copy_eof_epoch=4294967294 drain_completion_eof_epoch=2 "
        "quiescent_eof_epoch=4 drain_post_snapshot_eofs=4 "
        "quiescent_post_snapshot_eofs=6 drain_duration_ms=4 "
        "finish_completed=1 pending_frames=0 drained_frames=2400 "
        "discarded_frames=0 draining_q_ovf=2 sticky_error=0 "
        "registered_generation=1 terminal_generation=2 stale_callbacks=0 "
        "callback_in_flight=0 callbacks_active=0 codec_final_muted=1 "
        "pa_final_low=1 i2s_enabled=0 i2s_created=0 first_error=0 "
        "forced_abort=0 sink_destroyed=1",
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS",
        PASS_MARKER,
    ]


def replace_once(lines: list[str], old: str, new: str) -> list[str]:
    text = "\n".join(lines)
    if text.count(old) != 1:
        raise AssertionError(f"mutation target is not unique: {old!r}")
    return text.replace(old, new, 1).split("\n")


def capture_status_v2(raw: bytes, raw_path: Path,
                      reset_offset: int) -> dict[str, object]:
    pass_bytes = PASS_MARKER.encode("ascii")
    fail_bytes = FAIL_MARKER.encode("ascii")
    occurrences: list[tuple[int, int, str]] = []
    for line in raw_lines(raw, reset_offset):
        if line.complete and line.content in {pass_bytes, fail_bytes}:
            occurrences.append((
                line.start, line.end,
                "PASS" if line.content == pass_bytes else "FAIL",
            ))
    if occurrences:
        terminal_start, terminal_end, terminal_kind = occurrences[0]
        terminal_complete = True
        terminal_status = terminal_kind
        terminal_marker = terminal_kind
        exit_reason = f"TERMINAL_{terminal_kind}"
        state = exit_reason
    else:
        terminal_start = None
        terminal_end = None
        terminal_complete = False
        terminal_status = "NOT_OBSERVED"
        terminal_marker = None
        exit_reason = "HARD_TIMEOUT"
        state = "HARD_TIMEOUT"
    return {
        "schema_version": 2,
        "raw_path": str(raw_path),
        "raw_bytes": len(raw),
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "reset_byte_offset": reset_offset,
        "reset_count": 1,
        "terminal_status": terminal_status,
        "terminal_marker": terminal_marker,
        "terminal_line_start_offset": terminal_start,
        "terminal_line_end_offset": terminal_end,
        "terminal_line_complete": terminal_complete,
        "terminal_pass_marker_config": PASS_MARKER,
        "terminal_fail_marker_config": FAIL_MARKER,
        "exit_reason": exit_reason,
        "state": state,
        "post_terminal_drain_seconds": 0.5,
        "idle_timeout_enabled": False,
        "serial_error": None,
        "raw_final_line_complete": not raw or raw.endswith(b"\n"),
    }


def run_raw(
    directory: Path,
    raw: bytes,
    *,
    expected_sha: str = EXPECTED_SHA,
    reset_offset: int = len(b"setup boot\r\n"),
    status_overrides: dict[str, object] | None = None,
) -> subprocess.CompletedProcess[str]:
    raw_path = directory / "capture.raw"
    status_path = directory / "capture.status.json"
    raw_path.write_bytes(raw)
    status = capture_status_v2(raw, raw_path, reset_offset)
    if status_overrides:
        status.update(status_overrides)
    status_path.write_text(json.dumps(status), encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(VALIDATOR), str(raw_path), "--status",
         str(status_path), "--expected-source-sha", expected_sha],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def run_lines(
    directory: Path,
    lines: list[str],
    *,
    boot_lines: tuple[str, ...] = BOOT_LINES,
    tail: bytes = b"",
    terminal_delimiter: bytes = b"\r\n",
    status_overrides: dict[str, object] | None = None,
) -> subprocess.CompletedProcess[str]:
    prefix = b"setup boot\r\n"
    before_terminal = [*boot_lines, *lines[:-1]]
    raw = prefix + ("\r\n".join(before_terminal) + "\r\n").encode("ascii")
    raw += lines[-1].encode("ascii") + terminal_delimiter + tail
    return run_raw(directory, raw, status_overrides=status_overrides)


def require_accepted(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"valid S2 evidence rejected: {label}: {result.stderr.strip()}"
        )


def require_rejected(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode == 0:
        raise AssertionError(f"S2 mutation accepted: {label}")


def main() -> int:
    require_source_schema_sync()
    canonical = canonical_lines()
    retry_zero = canonical_lines(retry_count=0)
    mutation_families: dict[str, list[tuple[str, list[str]]]] = {
        "grammar": [
            ("missing_record", canonical[:1] + canonical[2:]),
            ("duplicate_record", canonical[:2] + [canonical[1]] + canonical[2:]),
            ("unknown_record", canonical[:1] +
             ["5D2_S2_UNKNOWN schema=1 evidence_class=PHYSICAL_EXEC"] +
             canonical[1:]),
            ("record_reorder", [canonical[1], canonical[0], *canonical[2:]]),
            ("unknown_field", replace_once(
                canonical, " codec_unmute_completed=1",
                " codec_unmute_completed=1 unknown=0")),
            ("missing_field", replace_once(
                canonical, " codec_unmute_completed=1", "")),
            ("duplicate_field", replace_once(
                canonical, " prefill=4", " prefill=4 prefill=4")),
            ("leading_space_record", [" " + canonical[0], *canonical[1:]]),
            ("s1_record_not_evidence", canonical[:4] +
             ["5D2_S1_IDENTITY schema=2 evidence_class=PHYSICAL_EXEC"] +
             canonical[4:]),
        ],
        "identity": [
            ("source_sha", replace_once(
                canonical, f"source_git_sha={EXPECTED_SHA}",
                "source_git_sha=1123456789abcdef0123456789abcdef01234567")),
            ("profile", replace_once(
                canonical, "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S",
                "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S_SHORT")),
            ("backend", replace_once(
                canonical, "backend=IDF_I2S0_ES8311", "backend=OTHER")),
            ("stimulus", replace_once(
                canonical, "stimulus=FULL_REPLAY_PCM", "stimulus=PRE_RESET_PCM")),
            ("board", replace_once(
                canonical, "board=P4_NANO_P4_V1X", "board=GENERIC")),
            ("display", replace_once(
                canonical, "display=DISABLED", "display=ENABLED")),
        ],
        "start": [
            ("rate", replace_once(canonical, "rate_hz=48000",
                                  "rate_hz=44100")),
            ("format", replace_once(canonical, "i2s_format=PHILIPS",
                                    "i2s_format=MSB")),
            ("unit_bytes", replace_once(canonical, "physical_unit_bytes=960",
                                        "physical_unit_bytes=959")),
            ("dma_desc", replace_once(canonical, "dma_desc=4", "dma_desc=3")),
            ("ring_capacity", replace_once(canonical, "ring_capacity=8",
                                           "ring_capacity=7")),
            ("prefill", replace_once(canonical, "prefill=4", "prefill=3")),
            ("startup_latch", replace_once(canonical, "prepare_completed=1",
                                           "prepare_completed=0")),
        ],
        "pcm": [
            ("frames", replace_once(canonical, "semantic_frames=2400",
                                    "semantic_frames=2399")),
            ("bytes", replace_once(canonical, "semantic_bytes=9600",
                                   "semantic_bytes=9596")),
            ("crc", replace_once(canonical, "semantic_crc32=b518c3c9",
                                 "semantic_crc32=00000000")),
            ("sha", replace_once(
                canonical,
                "semantic_sha256=176ea419f153382039e143163ff8476c5461abfddd055cd801003ef89c04a18a",
                "semantic_sha256=076ea419f153382039e143163ff8476c5461abfddd055cd801"
                "003ef89c04a18a")),
            ("malformed_crc", replace_once(canonical, "semantic_crc32=b518c3c9",
                                           "semantic_crc32=b518c3c")),
            ("malformed_sha", replace_once(
                canonical,
                "semantic_sha256=176ea419f153382039e143163ff8476c5461abfddd055cd801003ef89c04a18a",
                "semantic_sha256=176ea419")),
        ],
        "stream_geometry": [
            ("produced_slots", replace_once(canonical, "produced_slots=10",
                                            "produced_slots=9")),
            ("physical_units", replace_once(canonical, "physical_units=10",
                                            "physical_units=9")),
            ("full_units", replace_once(canonical, "full_units=10",
                                        "full_units=9")),
            ("preloaded_units", replace_once(canonical, "preloaded_units=4",
                                             "preloaded_units=3")),
            ("running_units", replace_once(canonical, "running_units=6",
                                           "running_units=5")),
            ("final_partial", replace_once(canonical, "final_partial_units=0",
                                           "final_partial_units=1")),
            ("final_valid", replace_once(canonical, "final_valid_frames=0",
                                         "final_valid_frames=1")),
            ("padding_frames", replace_once(canonical, "padding_frames=0",
                                            "padding_frames=1")),
            ("padding_bytes", replace_once(canonical, "padding_bytes=0",
                                           "padding_bytes=4")),
        ],
        "retry": [
            ("submit_attempts", replace_once(canonical, "submit_attempts=11",
                                             "submit_attempts=10")),
            ("invalid_retry", replace_once(canonical, "retry_count=1",
                                           "retry_count=-1")),
            ("accepted_count", replace_once(
                canonical, "sink_accepted_frames=2400",
                "sink_accepted_frames=2160")),
        ],
        "q_ovf": [
            ("running_nonzero", replace_once(canonical, "running_q_ovf=0",
                                             "running_q_ovf=1")),
            ("draining_exceeds_interval", replace_once(
                canonical, "draining_q_ovf=2", "draining_q_ovf=7")),
        ],
        "eof": [
            ("insufficient_drain", replace_once(
                replace_once(canonical, "drain_completion_eof_epoch=2",
                             "drain_completion_eof_epoch=1"),
                "drain_post_snapshot_eofs=4", "drain_post_snapshot_eofs=3")),
            ("quiescent_before_drain", replace_once(
                replace_once(
                    replace_once(
                        replace_once(canonical, "drain_completion_eof_epoch=2",
                                     "drain_completion_eof_epoch=3"),
                        "drain_post_snapshot_eofs=4", "drain_post_snapshot_eofs=5"),
                    "quiescent_eof_epoch=4", "quiescent_eof_epoch=2"),
                "quiescent_post_snapshot_eofs=6", "quiescent_post_snapshot_eofs=4")),
            ("reported_delta", replace_once(canonical,
                                            "drain_post_snapshot_eofs=4",
                                            "drain_post_snapshot_eofs=5")),
            ("half_range", replace_once(
                replace_once(
                    replace_once(
                        replace_once(canonical,
                                     "final_copy_eof_epoch=4294967294",
                                     "final_copy_eof_epoch=0"),
                        "drain_completion_eof_epoch=2",
                        "drain_completion_eof_epoch=2147483648"),
                    "quiescent_eof_epoch=4",
                    "quiescent_eof_epoch=2147483648"),
                "drain_post_snapshot_eofs=4 quiescent_post_snapshot_eofs=6",
                "drain_post_snapshot_eofs=2147483648 "
                "quiescent_post_snapshot_eofs=2147483648")),
            ("drain_timeout", replace_once(canonical, "drain_duration_ms=4",
                                           "drain_duration_ms=40")),
        ],
        "ownership": [
            ("pending", replace_once(canonical, "pending_frames=0",
                                     "pending_frames=1")),
            ("drained", replace_once(canonical, "drained_frames=2400",
                                     "drained_frames=2399")),
            ("discarded", replace_once(canonical, "discarded_frames=0",
                                       "discarded_frames=1")),
        ],
        "ring_errors": [
            ("ring_occupancy", replace_once(canonical,
                                            "final_ring_occupancy=0",
                                            "final_ring_occupancy=1")),
            ("ring_partial", replace_once(canonical, "final_ring_partial=0",
                                          "final_ring_partial=1")),
            ("drop", replace_once(canonical, "drops=0", "drops=1")),
            ("overwrite", replace_once(canonical, "overwrite=0",
                                       "overwrite=1")),
            ("abandonment", replace_once(canonical, "abandoned_published=0",
                                         "abandoned_published=1")),
            ("partial_abandonment", replace_once(canonical,
                                                 "abandoned_partial=0",
                                                 "abandoned_partial=1")),
            ("rendered_abandonment", replace_once(canonical,
                                                  "abandoned_rendered=0",
                                                  "abandoned_rendered=1")),
            ("sticky", replace_once(canonical, "sticky_error=0",
                                    "sticky_error=1")),
            ("first_error", replace_once(canonical, "first_error=0",
                                         "first_error=2")),
            ("forced_abort", replace_once(canonical, "forced_abort=0",
                                          "forced_abort=1")),
        ],
        "callback_final": [
            ("callback_in_flight", replace_once(canonical,
                                                "callback_in_flight=0",
                                                "callback_in_flight=1")),
            ("callbacks_active", replace_once(canonical, "callbacks_active=0",
                                              "callbacks_active=1")),
            ("codec_unmuted", replace_once(canonical, "codec_final_muted=1",
                                           "codec_final_muted=0")),
            ("pa_not_low", replace_once(canonical, "pa_final_low=1",
                                        "pa_final_low=0")),
            ("i2s_enabled", replace_once(canonical, "i2s_enabled=0",
                                         "i2s_enabled=1")),
            ("i2s_created", replace_once(canonical, "i2s_created=0",
                                         "i2s_created=1")),
            ("sink_not_destroyed", replace_once(canonical, "sink_destroyed=1",
                                                "sink_destroyed=0")),
            ("finish_incomplete", replace_once(canonical, "finish_completed=1",
                                               "finish_completed=0")),
            ("stale_callback", replace_once(canonical, "stale_callbacks=0",
                                            "stale_callbacks=1")),
            ("generation", replace_once(canonical, "terminal_generation=2",
                                        "terminal_generation=3")),
            ("controller_state", replace_once(canonical,
                                              "controller_state=FINISHED",
                                              "controller_state=FAILED")),
            ("sink_state", replace_once(canonical, "sink_state=QUIESCENT",
                                        "sink_state=FAILED")),
        ],
    }

    with tempfile.TemporaryDirectory(prefix="p4-audio86-s2-validator-") as temp:
        directory = Path(temp)
        valid = run_lines(directory, canonical)
        require_accepted(valid, "natural_retry")
        if "S2_PHYSICAL_EVIDENCE_VALIDATION=PASS" not in valid.stdout:
            raise AssertionError("validator success marker missing")
        require_accepted(run_lines(directory, retry_zero), "zero_retry")
        require_accepted(
            run_lines(directory, canonical, tail=b"I cleanup: complete\r\n"),
            "benign_tail",
        )
        require_accepted(
            run_lines(directory, canonical, tail=b"partial diagnostic"),
            "benign_tail_fragment",
        )

        for family, mutations in mutation_families.items():
            for name, mutated in mutations:
                require_rejected(run_lines(directory, mutated), f"{family}.{name}")
            print(f"S2_MUTATION_FAMILY_{family.upper()}=PASS")

        boundary_mutations = (
            ("failed", [*canonical[:-1], FAIL_MARKER], b"", None),
            ("missing_complete", canonical[:-1], b"", None),
            ("duplicate_terminal", canonical,
             PASS_MARKER.encode("ascii") + b"\r\n", None),
            ("panic_tail", canonical, b"Guru Meditation Error\r\n", None),
            ("second_boot_tail", canonical,
             b"I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader\r\n", None),
            ("extra_s2_tail", canonical,
             b"5D2_S2_UNKNOWN schema=1 evidence_class=PHYSICAL_EXEC\r\n", None),
            ("s1_terminal_tail", canonical,
             b"P4_AUDIO86_PHYSICAL_S1_TERMINAL=COMPLETE\r\n", None),
            ("extra_result_tail", canonical,
             b"P4_AUDIO86_REAL_GUEST_RESULT=PASS\r\n", None),
            ("status_v1", canonical, b"", {"schema_version": 1}),
            ("forged_offset", canonical, b"",
             {"terminal_line_start_offset": 0}),
            ("raw_length", canonical, b"", {"raw_bytes": 0}),
            ("raw_hash", canonical, b"", {"raw_sha256": "0" * 64}),
        )
        for name, lines, tail, overrides in boundary_mutations:
            require_rejected(
                run_lines(directory, lines, tail=tail,
                          status_overrides=overrides),
                f"capture_terminal.{name}",
            )
        require_rejected(
            run_lines(directory, canonical, terminal_delimiter=b""),
            "capture_terminal.incomplete_terminal",
        )
        require_rejected(
            run_lines(directory, canonical,
                      boot_lines=(*BOOT_LINES, BOOT_LINES[0])),
            "capture_terminal.second_boot_inside_canonical",
        )

    print("S2_VALIDATOR_SEPARATE_WORKLOAD_CONTRACT=PASS")
    print("S2_CAPTURE_STATUS_V2_REUSE=PASS")
    print("S2_CAPTURE_BOUNDARY_SEMANTICS_UNCHANGED=PASS")
    print("S2_RECORD_GRAMMAR_FAIL_CLOSED=PASS")
    print("S2_RECORD_SCHEMA_SOURCE_SYNC=PASS")
    print("S2B_REUSES_EXISTING_FULL_REPLAY_GOLDEN=PASS")
    print("S2B_NEW_PCM_GOLDEN=NO")
    print("S2_STREAM_GEOMETRY_RECOMPUTED=PASS")
    print("S2_CANONICAL_PCM_IDENTITY=PASS")
    print("S2_RETRY_RUNTIME_RULE=PASS")
    print("S2_RETRY_BYTE_IDENTITY_PROOF_CLASS=HOST_SOURCE_EXEC")
    print("S2_RUNNING_Q_OVF_ACCEPTANCE=ZERO_REQUIRED")
    print("S2_DRAIN_Q_OVF_INTERVAL_RULE=PASS")
    print("S2_EOF_MODULAR_ARITHMETIC=PASS")
    print("S2_OWNERSHIP_CONSERVATION=PASS")
    print("S2_DRAIN_DURATION_VALIDATION=PASS")
    print("S2_TERMINAL_POLICY_STRICT=PASS")
    print("S2_VALIDATOR_CHANGE_SENSITIVITY=PASS")
    print("S2_HOST_VALIDATOR_RECOMPUTES_ACCEPTANCE=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
