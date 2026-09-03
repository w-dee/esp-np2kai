#!/usr/bin/env python3
"""Change-sensitivity tests for the 86R.5D.2 S1 physical validator."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_physical_s1_log.py"
GOLDEN = ROOT / "host/probe/audio86_guest_sync_pcm_golden.json"
EXPECTED_SHA = "0123456789abcdef0123456789abcdef01234567"
PASS_MARKER = "P4_AUDIO86_PHYSICAL_S1_TERMINAL=COMPLETE"
FAIL_MARKER = "P4_AUDIO86_PHYSICAL_S1_TERMINAL=FAILED"


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
        PASS_MARKER,
    ]


def replace_once(lines: list[str], old: str, new: str) -> list[str]:
    text = "\n".join(lines)
    changed = text.replace(old, new, 1)
    if changed == text:
        raise AssertionError(f"mutation did not apply: {old!r}")
    return changed.split("\n")


def accepted(directory: Path, lines: list[str]) -> bool:
    prefix = b"setup boot\r\n"
    canonical = ("\n".join(lines) + "\n").encode("ascii")
    raw = prefix + canonical
    raw_path = directory / "capture.raw"
    status_path = directory / "capture.status.json"
    raw_path.write_bytes(raw)
    status = {
        "schema_version": 1,
        "raw_path": str(raw_path),
        "raw_bytes": len(raw),
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "reset_byte_offset": len(prefix),
        "reset_count": 1,
        "terminal_status": "PASS",
        "terminal_marker": "PASS",
        "terminal_pass_marker_config": PASS_MARKER,
        "terminal_fail_marker_config": FAIL_MARKER,
        "exit_reason": "TERMINAL_PASS",
        "state": "TERMINAL_PASS",
        "idle_timeout_enabled": False,
        "serial_error": None,
        "final_line_complete": True,
    }
    status_path.write_text(json.dumps(status), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), str(raw_path), "--status",
         str(status_path), "--expected-source-sha", EXPECTED_SHA],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


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
    mutations.append(("terminal_not_last", [*canonical, "post-terminal diagnostic"]))
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
        for name, boundary in accepted_boundaries:
            if not accepted(directory, boundary):
                print(f"valid S1 boundary rejected: {name}", file=sys.stderr)
                return 1
        for name, mutated in mutations:
            if accepted(directory, mutated):
                print(f"mutation accepted: {name}", file=sys.stderr)
                return 1
    print("S1_DRAIN_Q_OVF_ACCEPTANCE_BOUNDARY_TEST=PASS "
          "accepted=drain4_quiescent4,5,6 rejected=qovf7_quiescent6")
    print("S1_Q_OVF_INTERVAL_CHANGE_SENSITIVITY=PASS")
    print("S1_FINISH_RECORD_SCHEMA_VERSIONING=PASS schema=2 old_schema=REJECTED")
    print("S1_VALIDATOR_DRAIN_Q_OVF_RULE_SEMANTIC=PASS")
    print(f"S1_PHYSICAL_VALIDATOR_MUTATIONS={len(mutations)}_ALL_REJECTED")
    print("S1_PHYSICAL_VALIDATOR_CHANGE_SENSITIVITY=PASS")
    print("S1_TERMINAL_COMPLETE_ACCEPTANCE_AUTHORITY=NO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
