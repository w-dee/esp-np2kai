#!/usr/bin/env python3
"""Change-sensitivity tests for the 86R.5D.2 S1 physical validator."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from validate_p4_audio86_physical_s1_log import raw_lines, validate


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.dev import p4_nano_capture as CAPTURE  # noqa: E402


VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_physical_s1_log.py"
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
EXPECTED_SHA = "0123456789abcdef0123456789abcdef01234567"
PASS_MARKER = "P4_AUDIO86_PHYSICAL_S1_TERMINAL=COMPLETE"
FAIL_MARKER = "P4_AUDIO86_PHYSICAL_S1_TERMINAL=FAILED"
BOOT_LINES = (
    "I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader",
    "I (1558) main_task: Calling app_main()",
)


def canonical_lines() -> list[str]:
    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))["values"]
    return [
        "5D2_S1_IDENTITY schema=2 evidence_class=PHYSICAL_EXEC "
        f"source_git_sha={EXPECTED_SHA} "
        "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S_SHORT "
        "board=P4_NANO_P4_V1X backend=IDF_I2S0_ES8311 "
        "stimulus=PRE_RESET_PCM display=DISABLED",
        "5D2_S1_START schema=2 evidence_class=PHYSICAL_EXEC rate_hz=48000 "
        "channels=2 sample_bits=16 encoding=S16LE i2s_format=PHILIPS "
        "clock_source=APLL mclk_multiple=256 mclk_hz=12288000 q_frames=240 "
        "bytes_per_frame=4 physical_unit_bytes=960 dma_desc=4 dma_frames=240 "
        "prepare_completed=1 pa_initial_low=1 codec_initialized_muted=1 "
        "i2s_initialized=1 muted_warmup_completed=1 callbacks_registered=1 "
        "stream_started=1 codec_unmute_completed=1",
        "5D2_S1_PCM schema=2 evidence_class=PHYSICAL_EXEC "
        f"semantic_frames={golden['PRE_RESET_PCM_FRAMES']} "
        f"semantic_bytes={golden['PRE_RESET_PCM_BYTES']} "
        f"semantic_crc32={golden['PRE_RESET_PCM_CRC32']} "
        f"semantic_sha256={golden['PRE_RESET_PCM_SHA256']} "
        "controller_accepted_frames=13 controller_accepted_bytes=52 "
        "sink_accepted_frames=13 sink_accepted_bytes=52 full_units=0 "
        "final_partial_units=1 final_valid_frames=13 physical_units=1 "
        "physical_bytes=960 padding_frames=227 padding_bytes=908 "
        "submit_attempts=1 retry_count=0",
        "5D2_S1_FINISH schema=2 evidence_class=PHYSICAL_EXEC "
        "controller_state=FINISHED sink_state=QUIESCENT "
        "final_copy_eof_epoch=4294967294 drain_completion_eof_epoch=2 "
        "quiescent_eof_epoch=2 drain_post_snapshot_eofs=4 "
        "quiescent_post_snapshot_eofs=4 drain_duration_ms=4 "
        "finish_completed=1 "
        "pending_frames=0 drained_frames=13 discarded_frames=0 "
        "running_q_ovf=0 draining_q_ovf=4 sticky_error=0 "
        "registered_generation=1 terminal_generation=2 stale_callbacks=0 "
        "callback_in_flight=0 callbacks_active=0 codec_final_muted=1 "
        "pa_final_low=1 i2s_enabled=0 i2s_created=0 first_error=0 "
        "forced_abort=0 sink_destroyed=1",
        "P4_AUDIO86_REAL_GUEST_RESULT=PASS",
        PASS_MARKER,
    ]


def replace_once(lines: list[str], old: str, new: str) -> list[str]:
    text = "\n".join(lines)
    changed = text.replace(old, new, 1)
    if changed == text:
        raise AssertionError(f"mutation did not apply: {old!r}")
    return changed.split("\n")


def capture_status_v2(raw: bytes, raw_path: Path,
                      reset_offset: int) -> dict[str, object]:
    pass_bytes = PASS_MARKER.encode("ascii")
    fail_bytes = FAIL_MARKER.encode("ascii")
    occurrences: list[tuple[int, int, str]] = []
    for line in raw_lines(raw, reset_offset):
        if line.complete and line.content in {pass_bytes, fail_bytes}:
            occurrences.append((
                line.start,
                line.end,
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


def validate_raw(
    directory: Path,
    raw: bytes,
    *,
    expected_sha: str = EXPECTED_SHA,
    reset_offset: int = len(b"setup boot\r\n"),
    status_overrides: dict[str, object] | None = None,
) -> bool:
    raw_path = directory / "capture.raw"
    status_path = directory / "capture.status.json"
    raw_path.write_bytes(raw)
    status = capture_status_v2(raw, raw_path, reset_offset)
    if status_overrides:
        status.update(status_overrides)
    status_path.write_text(json.dumps(status), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), str(raw_path), "--status",
         str(status_path), "--expected-source-sha", expected_sha],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def accepted(
    directory: Path,
    lines: list[str],
    *,
    boot_lines: tuple[str, ...] = BOOT_LINES,
    tail: bytes = b"",
    terminal_delimiter: bytes = b"\r\n",
    status_overrides: dict[str, object] | None = None,
) -> bool:
    prefix = b"setup boot\r\n"
    before_terminal = [*boot_lines, *lines[:-1]]
    raw = prefix + ("\r\n".join(before_terminal) + "\r\n").encode("ascii")
    raw += lines[-1].encode("ascii") + terminal_delimiter + tail
    return validate_raw(
        directory, raw, status_overrides=status_overrides
    )


def replay_historical_bundle(directory: Path, bundle: Path) -> list[str]:
    """Reconstruct status v2 in a temp dir; never alter retained evidence."""
    raw = (bundle / "s1-canonical.raw").read_bytes()
    old_status = json.loads(
        (bundle / "s1-canonical.status.json").read_text(encoding="utf-8")
    )
    source_shas = set(re.findall(rb"source_git_sha=([0-9a-f]{40})", raw))
    if len(source_shas) != 1:
        raise AssertionError(f"historical source SHA is ambiguous: {bundle}")
    expected_sha = next(iter(source_shas)).decode("ascii")
    raw_path = directory / "historical.raw"
    status_path = directory / "historical.status.json"
    raw_path.write_bytes(raw)
    status = capture_status_v2(
        raw, raw_path, int(old_status["reset_byte_offset"])
    )
    status_path.write_text(json.dumps(status), encoding="utf-8")
    return validate(raw_path, status_path, expected_sha)


def capture_chunking_matrix(directory: Path, lines: list[str]) -> bool:
    pre_reset = b"setup boot\r\n"
    body = ("\r\n".join((*BOOT_LINES, *lines)) + "\r\n").encode("ascii")
    tail = b"ARBITRARY_DIAGNOSTIC key=value\r\n"
    stream = body + tail
    marker = PASS_MARKER.encode("ascii")
    marker_start = stream.index(marker)
    terminal_end = marker_start + len(marker) + 2
    variants = (
        (stream,),
        (stream[:marker_start], stream[marker_start:]),
        (stream[:terminal_end - 2], stream[terminal_end - 2:]),
        (stream[:terminal_end - 1], stream[terminal_end - 1:]),
        (stream[:terminal_end], stream[terminal_end:]),
    )
    observed: set[tuple[object, ...]] = set()
    for index, chunks in enumerate(variants):
        root = directory / f"capture-chunks-{index}"
        root.mkdir()
        raw_path = root / "capture.raw"
        status_path = root / "capture.status.json"
        session = CAPTURE.CaptureSession(
            raw_path,
            status_path,
            terminal_pass_marker=PASS_MARKER,
            terminal_fail_marker=FAIL_MARKER,
        )
        session.prepare()
        session.feed(pre_reset)
        session.issue_reset(lambda: None)
        for chunk in chunks:
            session.feed(chunk)
        status = session.finish()
        errors = validate(raw_path, status_path, EXPECTED_SHA)
        if errors:
            print(f"chunking variant rejected: {index}: {errors!r}",
                  file=sys.stderr)
            return False
        observed.add((
            status["terminal_marker"],
            status["terminal_line_start_offset"],
            status["terminal_line_end_offset"],
            raw_path.read_bytes()[:status["terminal_line_end_offset"]],
        ))
    return len(observed) == 1


def main() -> int:
    canonical = canonical_lines()
    drain_zero = replace_once(canonical, "draining_q_ovf=4",
                              "draining_q_ovf=0")
    drain_one = replace_once(canonical, "draining_q_ovf=4",
                             "draining_q_ovf=1")
    one_post_completion = replace_once(
        replace_once(
            replace_once(canonical, "quiescent_eof_epoch=2",
                         "quiescent_eof_epoch=3"),
            "quiescent_post_snapshot_eofs=4",
            "quiescent_post_snapshot_eofs=5"),
        "draining_q_ovf=4", "draining_q_ovf=5")
    two_post_completion = replace_once(
        replace_once(
            replace_once(canonical, "quiescent_eof_epoch=2",
                         "quiescent_eof_epoch=4"),
            "quiescent_post_snapshot_eofs=4",
            "quiescent_post_snapshot_eofs=6"),
        "draining_q_ovf=4", "draining_q_ovf=6")
    accepted_boundaries = (
        ("draining_q_ovf_zero", drain_zero),
        ("draining_q_ovf_nonzero", drain_one),
        ("no_post_completion_callback", canonical),
        ("one_post_completion_callback", one_post_completion),
        ("multiple_post_completion_callbacks", two_post_completion),
    )
    mutations: list[tuple[str, list[str]]] = []
    mutations.append(("missing_record", canonical[:1] + canonical[2:]))
    mutations.append(("duplicate_record", canonical[:2] + [canonical[1]] + canonical[2:]))
    mutations.append(("unknown_record", canonical[:1] +
                      ["5D2_S1_UNKNOWN schema=2 evidence_class=PHYSICAL_EXEC"] +
                      canonical[1:]))
    mutations.append(("record_reorder", [canonical[1], canonical[0], *canonical[2:]]))
    mutations.append(("leading_space_record", [" " + canonical[0], *canonical[1:]]))
    mutations.append(("real_guest_before_records",
                      [canonical[-2], *canonical[:-2], canonical[-1]]))
    replacements = (
        ("source_sha_mismatch", f"source_git_sha={EXPECTED_SHA}",
         "source_git_sha=1123456789abcdef0123456789abcdef01234567"),
        ("wrong_backend", "backend=IDF_I2S0_ES8311", "backend=OTHER"),
        ("wrong_profile", "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S_SHORT",
         "profile=AUDIO86_REAL_GUEST_PHYSICAL_I2S"),
        ("semantic_frames", "semantic_frames=13", "semantic_frames=12"),
        ("semantic_bytes", "semantic_bytes=52", "semantic_bytes=48"),
        ("crc", "semantic_crc32=f1b8c4c5", "semantic_crc32=00000000"),
        ("sha256", "semantic_sha256=d51e85a3", "semantic_sha256=051e85a3"),
        ("physical_bytes", "physical_bytes=960", "physical_bytes=959"),
        ("padding_arithmetic", "padding_bytes=908", "padding_bytes=904"),
        ("accepted_ownership", "sink_accepted_frames=13",
         "sink_accepted_frames=12"),
        ("start_history", "prepare_completed=1", "prepare_completed=0"),
        ("finish_completed", "finish_completed=1", "finish_completed=0"),
        ("controller_state", "controller_state=FINISHED",
         "controller_state=FAILED"),
        ("drain_delta_report_mismatch", "drain_post_snapshot_eofs=4",
         "drain_post_snapshot_eofs=3"),
        ("quiescent_delta_report_mismatch",
         "quiescent_post_snapshot_eofs=4",
         "quiescent_post_snapshot_eofs=5"),
        ("running_q_ovf", "running_q_ovf=0", "running_q_ovf=1"),
        ("pending", "pending_frames=0", "pending_frames=1"),
        ("discarded", "discarded_frames=0", "discarded_frames=1"),
        ("drained", "drained_frames=13", "drained_frames=12"),
        ("stale_callbacks", "stale_callbacks=0", "stale_callbacks=1"),
        ("terminal_generation", "terminal_generation=2",
         "terminal_generation=3"),
        ("callback_in_flight", "callback_in_flight=0", "callback_in_flight=1"),
        ("callbacks_active", "callbacks_active=0", "callbacks_active=1"),
        ("codec_final_muted", "codec_final_muted=1", "codec_final_muted=0"),
        ("pa_final_low", "pa_final_low=1", "pa_final_low=0"),
        ("i2s_enabled", "i2s_enabled=0", "i2s_enabled=1"),
        ("i2s_created", "i2s_created=0", "i2s_created=1"),
        ("first_error", "first_error=0", "first_error=2"),
        ("forced_abort", "forced_abort=0", "forced_abort=1"),
        ("sink_destroyed", "sink_destroyed=1", "sink_destroyed=0"),
        ("numeric_format", "submit_attempts=1", "submit_attempts=01"),
        ("complete_not_authority", "sink_state=QUIESCENT", "sink_state=FAILED"),
    )
    for name, old, new in replacements:
        mutations.append((name, replace_once(canonical, old, new)))
    mutations.extend((
        ("draining_q_ovf_exceeds_quiescent_eof",
         replace_once(two_post_completion, "draining_q_ovf=6",
                      "draining_q_ovf=7")),
        ("quiescent_delta_precedes_drain_delta",
         replace_once(
             replace_once(
                 replace_once(canonical, "quiescent_eof_epoch=2",
                              "quiescent_eof_epoch=1"),
                 "quiescent_post_snapshot_eofs=4",
                 "quiescent_post_snapshot_eofs=3"),
             "draining_q_ovf=4", "draining_q_ovf=3")),
        ("drain_delta_below_dma_desc",
         replace_once(
             replace_once(
                 replace_once(canonical, "drain_completion_eof_epoch=2",
                              "drain_completion_eof_epoch=1"),
                 "drain_post_snapshot_eofs=4",
                 "drain_post_snapshot_eofs=3"),
             "draining_q_ovf=4", "draining_q_ovf=3")),
        ("draining_q_ovf_with_sticky_error",
         replace_once(drain_one, "sticky_error=0", "sticky_error=1")),
        ("draining_q_ovf_with_ownership_failure",
         replace_once(drain_one, "drained_frames=13", "drained_frames=12")),
        ("draining_q_ovf_with_insufficient_eof",
         replace_once(
             replace_once(drain_one, "drain_completion_eof_epoch=2",
                          "drain_completion_eof_epoch=1"),
             "drain_post_snapshot_eofs=4", "drain_post_snapshot_eofs=3")),
        ("missing_quiescent_eof_field",
         replace_once(canonical, " quiescent_eof_epoch=2", "")),
        ("duplicate_quiescent_eof_field",
         replace_once(canonical, " quiescent_eof_epoch=2",
                      " quiescent_eof_epoch=2 quiescent_eof_epoch=2")),
        ("malformed_quiescent_eof_field",
         replace_once(canonical, "quiescent_eof_epoch=2",
                      "quiescent_eof_epoch=two")),
        ("old_schema_1_finish_grammar",
         replace_once(
             [line.replace("schema=2", "schema=1") for line in canonical],
             "drain_completion_eof_epoch=2 quiescent_eof_epoch=2 "
             "drain_post_snapshot_eofs=4 quiescent_post_snapshot_eofs=4",
             "finish_eof_epoch=2 post_snapshot_eofs=4")),
    ))
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        if not capture_chunking_matrix(directory, canonical):
            print("capture/validator chunking integration mismatch", file=sys.stderr)
            return 1
        for name, boundary in accepted_boundaries:
            if not accepted(directory, boundary):
                print(f"valid S1 boundary rejected: {name}", file=sys.stderr)
                return 1
        accepted_tail_cases = (
            ("terminal_raw_eof", b""),
            ("benign_same_chunk_tail", b"I cleanup: complete\r\n"),
            ("benign_later_chunk_tail", b"I cleanup: complete\r\n"),
            ("non_authoritative_telemetry",
             b"ARBITRARY_DIAGNOSTIC key=value count=7\r\n"),
            ("incomplete_benign_tail_fragment", b"partial diagnostic"),
        )
        for name, tail in accepted_tail_cases:
            if not accepted(directory, canonical, tail=tail):
                print(f"valid terminal/tail boundary rejected: {name}",
                      file=sys.stderr)
                return 1
        if not accepted(directory, canonical, terminal_delimiter=b"\n"):
            print("valid LF terminal boundary rejected", file=sys.stderr)
            return 1
        for name, mutated in mutations:
            if accepted(directory, mutated):
                print(f"mutation accepted: {name}", file=sys.stderr)
                return 1

        rejected_boundary_cases = (
            ("terminal_without_delimiter",
             accepted(directory, canonical, terminal_delimiter=b"")),
            ("panic_tail",
             accepted(directory, canonical, tail=b"Guru Meditation Error\r\n")),
            ("watchdog_tail",
             accepted(directory, canonical,
                      tail=b"Task watchdog got triggered\r\n")),
            ("second_boot_tail",
             accepted(directory, canonical,
                      tail=b"I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader\r\n")),
            ("second_boot_inside_canonical",
             accepted(
                 directory,
                 canonical,
                 boot_lines=(*BOOT_LINES,
                             "I (25) boot: ESP-IDF v5.5.4 2nd stage bootloader"),
             )),
            ("missing_canonical_boot",
             accepted(directory, canonical, boot_lines=(BOOT_LINES[1],))),
            ("duplicate_app_main_start",
             accepted(directory, canonical,
                      boot_lines=(*BOOT_LINES, BOOT_LINES[1]))),
            ("complete_then_failed",
             accepted(directory, canonical,
                      tail=FAIL_MARKER.encode("ascii") + b"\r\n")),
            ("duplicate_complete",
             accepted(directory, canonical,
                      tail=PASS_MARKER.encode("ascii") + b"\r\n")),
            ("failed_only",
             accepted(directory, [*canonical[:-1], FAIL_MARKER])),
            ("missing_terminal", accepted(directory, canonical[:-1])),
            ("additional_5d2",
             accepted(directory, canonical,
                      tail=b"5D2_S1_UNKNOWN schema=2\r\n")),
            ("additional_real_guest_result",
             accepted(directory, canonical,
                      tail=b"P4_AUDIO86_REAL_GUEST_RESULT=PASS\r\n")),
            ("post_terminal_outer_fail",
             accepted(directory, canonical,
                      tail=b"P4_NANO_AUDIO86_REAL_GUEST_STATUS=FAIL\r\n")),
            ("forged_terminal_offset",
             accepted(directory, canonical,
                      status_overrides={"terminal_line_start_offset": 0})),
            ("forged_terminal_end_offset",
             accepted(directory, canonical,
                      status_overrides={"terminal_line_end_offset": 0})),
            ("terminal_type_metadata_mismatch",
             accepted(directory, canonical,
                      status_overrides={"terminal_marker": "FAIL"})),
            ("terminal_completion_metadata_mismatch",
             accepted(directory, canonical,
                      status_overrides={"terminal_line_complete": False})),
            ("reset_count_mismatch",
             accepted(directory, canonical,
                      status_overrides={"reset_count": 2})),
            ("raw_byte_count_mismatch",
             accepted(directory, canonical,
                      status_overrides={"raw_bytes": 0})),
            ("raw_hash_mismatch",
             accepted(directory, canonical,
                      status_overrides={"raw_sha256": "0" * 64})),
            ("raw_final_line_metadata_mismatch",
             accepted(directory, canonical,
                      status_overrides={"raw_final_line_complete": False})),
            ("short_post_terminal_drain",
             accepted(directory, canonical,
                      status_overrides={"post_terminal_drain_seconds": 0.49})),
            ("status_v1_rejected",
             accepted(directory, canonical,
                      status_overrides={"schema_version": 1})),
        )
        for name, was_accepted in rejected_boundary_cases:
            if was_accepted:
                print(f"terminal boundary mutation accepted: {name}",
                      file=sys.stderr)
                return 1

        first_errors = replay_historical_bundle(
            directory, ROOT / "docs/work/p4-audio86-s1-hw-20260903-19de7a51"
        )
        second_errors = replay_historical_bundle(
            directory,
            ROOT / "docs/work/p4-audio86-s1-rerun-r4-20260903-043f506a",
        )
        third_errors = replay_historical_bundle(
            directory,
            ROOT / "docs/work/p4-audio86-s1-r6-20260903-d8f4106d",
        )
        if not first_errors or "FAILED terminal line present" not in first_errors:
            print("first historical failure was not preserved", file=sys.stderr)
            return 1
        if not second_errors or "FAILED terminal line present" not in second_errors:
            print("second historical failure was not preserved", file=sys.stderr)
            return 1
        if third_errors:
            print(f"R6 diagnostic replay rejected: {third_errors!r}", file=sys.stderr)
            return 1
    print("S1_DRAIN_Q_OVF_ACCEPTANCE_BOUNDARY_TEST=PASS "
          "accepted=drain4_quiescent4,5,6 rejected=qovf7_quiescent6")
    print("S1_Q_OVF_INTERVAL_CHANGE_SENSITIVITY=PASS")
    print("S1_FINISH_RECORD_SCHEMA_VERSIONING=PASS schema=2 old_schema=REJECTED")
    print("S1_VALIDATOR_DRAIN_Q_OVF_RULE_SEMANTIC=PASS")
    print("S1_PHYSICAL_VALIDATOR_MUTATIONS=ALL_REJECTED")
    print("S1_PHYSICAL_VALIDATOR_CHANGE_SENSITIVITY=PASS")
    print("S1_TERMINAL_COMPLETE_ACCEPTANCE_AUTHORITY=NO")
    print("CAPTURE_TERMINAL_BOUNDARY_MATRIX=PASS")
    print("RAW_CANONICAL_TAIL_BOUNDARY_MODEL=PASS")
    print("POST_TERMINAL_TAIL_POLICY=STRUCTURAL_FAIL_CLOSED")
    print("MULTIPLE_TERMINAL_POLICY=EXACTLY_ONE_COMPLETE_FOR_SUCCESS")
    print("FAILED_TERMINAL_REMAINS_FAILURE=PASS")
    print("POST_TERMINAL_RESET_DETECTION_PRESERVED=PASS")
    print("POST_TERMINAL_FATAL_DETECTION_PRESERVED=PASS")
    print("S1_VALIDATOR_USES_TERMINAL_BOUNDED_CANONICAL_PREFIX=PASS")
    print("S1_VALIDATOR_TAIL_VALIDATED_SEPARATELY=PASS")
    print("CAPTURE_STATUS_RAW_BIJECTION=PASS")
    print("CAPTURE_VALIDATOR_RESPONSIBILITY_SPLIT=PASS")
    print("RAW_TAIL_FINAL_FRAGMENT_POLICY=PERMIT_NONAUTHORITATIVE_FRAGMENT")
    print("CAPTURE_STATUS_V1_COMPATIBILITY_POLICY=DIAGNOSTIC_RECONSTRUCTION_ONLY")
    print("R6_CAPTURE_COUNTERFACTUAL_REVALIDATION=PASS_DIAGNOSTIC_ONLY")
    print("FIRST_HISTORICAL_RESULT_PRESERVED=PASS")
    print("SECOND_HISTORICAL_RESULT_PRESERVED=PASS")
    print("THIRD_HISTORICAL_RESULT_PRESERVED=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
