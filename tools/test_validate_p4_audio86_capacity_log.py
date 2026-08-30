#!/usr/bin/env python3
"""Focused contract tests for the P4 capacity-log validator."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "tools/emu/validate_p4_audio86_capacity_log.py"
PCM_SHA = "7f1bc0cdcab519690c0d3580746827199f86dd270868f33ceb01d230e096310e"
CONTROL_SHA = "22fccc625378d2ae4a0715dd94a187a5c04cd5324a42828ac454293f8b1b328d"
SOURCE_SHA = "29783320f21cafc17330b56bc3484e8caf6543044f2f7280dc5703728cad5529"
TRANSPORT_SHA = "b2e50daab772920049b61ee2fec18b2fe46e672147bc67402bf00ee6ed844875"


def full_pass_log() -> str:
    return "\n".join([
        "AUDIO86_P4_CONFIG git_sha=" + "0" * 40 +
        " profile=P4_NANO_AUDIO86_CAPACITY_PROFILE mode=PACED_FORMAL"
        " cpu_hz=360000000 tick_hz=100 psram_bytes=33554432 psram_mhz=200"
        " rate=48000 quantum_frames=240 quantum_us=5000 quanta=12000",
        "AUDIO86_P4_IDENTITY frames=2880000 bytes=11520000 quanta=12000"
        " pcm_crc32=0x58929f1f pcm_sha256=" + PCM_SHA +
        " control_events=10 control_crc32=0xafa2dd74 control_sha256=" + CONTROL_SHA +
        " source_crc32=0x905d2517 source_sha256=" + SOURCE_SHA +
        " transport_events=333 transport_crc32=0x8fc674d3 transport_sha256=" + TRANSPORT_SHA +
        " pcm86_data_runs=323 pcm86_supplied=10584064 pcm86_consumed=10575000"
        " pcm86_fifo_min=4096 pcm86_fifo_max=36860 pcm86_underrun=0 peak_abs=4182"
        " clamped_samples=0 fm=1 psg=1 rhythm=1 pcm86=1 mid_quantum_events=4",
        "AUDIO86_P4_TRANSPORT event_wait_count=1 byte_wait_count=1 worker_wait_count=1"
        " event_high_water=128 byte_high_water=65536 final_event_occupancy=0 final_byte_occupancy=0",
        "AUDIO86_P4_STACK coordinator_hwm=512 producer_hwm=512 worker_hwm=1024",
        "AUDIO86_P4_PROGRESS planned_quanta=12000 planned_frames=2880000"
        " planned_bytes=11520000 completed_quanta=12000 completed_frames=2880000"
        " completed_bytes=11520000 canonicalized_quanta=12000 service_sample_count=12000"
        " failed_quantum=NONE",
        "AUDIO86_P4_PACING_EPOCH t0_us=100 worker_release_us=101"
        " q0_service_start_us=102 first_timer_callback_us=5100",
        "AUDIO86_P4_WORKER_TIMING sample_count=12000 min=1 mean=1 p50=1 p90=1 p95=1"
        " p99=1 p999=1 max=1 absolute_deadline_miss_count=0 pacing_backlog_count=0"
        " paced_input_starvation_count=0",
        "AUDIO86_P4_EVENT_SPLIT count=4 q0=0 q1=2 q2=4 q3=6",
        "AUDIO86_P4_PCM86_REFILL count=323 p99=1 max=1 non_refill_count=11677"
        " non_refill_p99=1 non_refill_max=1 planned_refill_quanta=323"
        " planned_non_refill_quanta=11677",
        "AUDIO86_P4_LIFECYCLE producer_done=1 worker_done=1 terminal=PASS first_error=0",
        "AUDIO86_P4_RESULT=PASS",
    ]) + "\n"


def target_fail_log() -> str:
    return "\n".join([
        "AUDIO86_P4_CONFIG git_sha=" + "0" * 40 +
        " profile=P4_NANO_AUDIO86_CAPACITY_PROFILE mode=PACED_FORMAL"
        " cpu_hz=360000000 tick_hz=100 psram_bytes=33554432 psram_mhz=200"
        " rate=48000 quantum_frames=240 quantum_us=5000 quanta=12000",
        "AUDIO86_P4_MEMORY_PREALLOC requested_context_bytes=512"
        " requested_owned_bytes=305400 internal_free_before=589784 internal_largest_before=393216",
        "AUDIO86_P4_ALLOC seq=1 name=context api=heap_caps_calloc requested_bytes=512"
        " managed_bytes=0 caps=INTERNAL|8BIT free_before=589784 largest_before=393216"
        " result=FAIL free_after=589784 largest_after=393216",
        "AUDIO86_P4_FAILURE first_error=ALLOCATION allocation=context",
        "AUDIO86_P4_RESULT=FAIL",
    ]) + "\n"


def runtime_target_fail_log() -> str:
    return "\n".join([
        "AUDIO86_P4_CONFIG git_sha=" + "0" * 40 +
        " profile=P4_NANO_AUDIO86_CAPACITY_PROFILE mode=PACED_FORMAL"
        " cpu_hz=360000000 tick_hz=100 psram_bytes=33554432 psram_mhz=200"
        " rate=48000 quantum_frames=240 quantum_us=5000 quanta=12000",
        "AUDIO86_P4_MEMORY_PREALLOC requested_context_bytes=600"
        " requested_owned_bytes=305400 internal_free_before=589784 internal_largest_before=393216",
        "AUDIO86_P4_ALLOC seq=1 name=context api=heap_caps_calloc requested_bytes=600"
        " managed_bytes=0 caps=INTERNAL|8BIT free_before=589784 largest_before=393216"
        " result=PASS free_after=589184 largest_after=393216",
        "AUDIO86_P4_PROGRESS planned_quanta=12000 planned_frames=2880000"
        " planned_bytes=11520000 completed_quanta=2 completed_frames=480"
        " completed_bytes=1920 canonicalized_quanta=3 service_sample_count=3"
        " failed_quantum=2",
        "AUDIO86_P4_WORKER_TIMING sample_count=3 min=1 mean=1 p50=1 p90=1 p95=1"
        " p99=1 p999=1 max=1 absolute_deadline_miss_count=1 pacing_backlog_count=0"
        " paced_input_starvation_count=0",
        "AUDIO86_P4_PCM86_REFILL count=1 p99=1 max=1 non_refill_count=2"
        " non_refill_p99=1 non_refill_max=1 planned_refill_quanta=323"
        " planned_non_refill_quanta=11677",
        "AUDIO86_P4_FAILURE first_error=14",
        "AUDIO86_P4_RESULT=FAIL",
    ]) + "\n"


class ValidatorTests(unittest.TestCase):
    def run_validator(self, text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.raw"
            path.write_text(text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(VALIDATOR), "--log", str(path)],
                text=True, capture_output=True, check=False,
            )

    def test_known_good_full_pass(self) -> None:
        result = self.run_validator(full_pass_log())
        self.assertEqual(result.returncode, 0)
        self.assertIn("FULL_FORMAL_PASS_VALIDATION", result.stdout)

    def test_well_formed_allocation_fail_is_not_pass(self) -> None:
        result = self.run_validator(target_fail_log())
        self.assertEqual(result.returncode, 1)
        self.assertIn("WELL_FORMED_TARGET_FAIL_VALIDATION", result.stdout)

    def test_well_formed_runtime_fail_is_not_pass(self) -> None:
        result = self.run_validator(runtime_target_fail_log())
        self.assertEqual(result.returncode, 1)
        self.assertIn("failure_class=14", result.stdout)
        self.assertIn("progress=PRESENT", result.stdout)

    def test_legacy_runtime_fail_is_explicitly_accepted(self) -> None:
        result = self.run_validator(runtime_target_fail_log().replace(
            "AUDIO86_P4_PROGRESS planned_quanta=12000 planned_frames=2880000 planned_bytes=11520000 completed_quanta=2 completed_frames=480 completed_bytes=1920 canonicalized_quanta=3 service_sample_count=3 failed_quantum=2\n",
            "").replace(
            "AUDIO86_P4_WORKER_TIMING sample_count=3 min=1 mean=1 p50=1 p90=1 p95=1 p99=1 p999=1 max=1 absolute_deadline_miss_count=1 pacing_backlog_count=0 paced_input_starvation_count=0\n",
            "").replace(
            "AUDIO86_P4_PCM86_REFILL count=1 p99=1 max=1 non_refill_count=2 non_refill_p99=1 non_refill_max=1 planned_refill_quanta=323 planned_non_refill_quanta=11677\n",
            ""))
        self.assertEqual(result.returncode, 1)
        self.assertIn("progress=LEGACY_UNAVAILABLE", result.stdout)

    def test_partial_progress_is_accepted(self) -> None:
        result = self.run_validator(runtime_target_fail_log())
        self.assertEqual(result.returncode, 1)

    def test_partial_timing_cannot_claim_planned_samples(self) -> None:
        result = self.run_validator(runtime_target_fail_log().replace(
            "sample_count=3", "sample_count=12000"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_completed_cannot_exceed_canonicalized(self) -> None:
        result = self.run_validator(runtime_target_fail_log().replace(
            "completed_quanta=2", "completed_quanta=4"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_completed_frame_geometry_is_checked(self) -> None:
        result = self.run_validator(runtime_target_fail_log().replace(
            "completed_frames=480", "completed_frames=481"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_partial_refill_counts_match_samples(self) -> None:
        result = self.run_validator(runtime_target_fail_log().replace(
            "non_refill_count=2", "non_refill_count=1"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_full_pass_requires_progress(self) -> None:
        progress = next(line for line in full_pass_log().splitlines()
                        if line.startswith("AUDIO86_P4_PROGRESS"))
        result = self.run_validator(full_pass_log().replace(progress + "\n", ""))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_full_pass_progress_must_be_complete(self) -> None:
        result = self.run_validator(full_pass_log().replace(
            "completed_quanta=12000", "completed_quanta=11999"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_pacing_epoch_order_is_checked(self) -> None:
        result = self.run_validator(full_pass_log().replace(
            "worker_release_us=101", "worker_release_us=99"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_malformed_early_fail_without_allocation(self) -> None:
        result = self.run_validator(target_fail_log().replace("AUDIO86_P4_ALLOC seq=1 name=context api=heap_caps_calloc requested_bytes=512 managed_bytes=0 caps=INTERNAL|8BIT free_before=589784 largest_before=393216 result=FAIL free_after=589784 largest_after=393216\n", ""))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_duplicate_and_contradictory_terminal(self) -> None:
        for suffix in ("AUDIO86_P4_RESULT=PASS\n", "AUDIO86_P4_RESULT=FAIL\n"):
            result = self.run_validator(full_pass_log() + suffix)
            self.assertEqual(result.returncode, 1)
            self.assertIn("MALFORMED_LOG", result.stdout)

    def test_full_pass_missing_identity(self) -> None:
        result = self.run_validator(full_pass_log().replace(next(line for line in full_pass_log().splitlines() if line.startswith("AUDIO86_P4_IDENTITY")) + "\n", ""))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_full_pass_wrong_pcm_sha(self) -> None:
        result = self.run_validator(full_pass_log().replace(PCM_SHA, "0" * 64))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)

    def test_full_pass_deadline_violation(self) -> None:
        result = self.run_validator(full_pass_log().replace("absolute_deadline_miss_count=0", "absolute_deadline_miss_count=1"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("MALFORMED_LOG", result.stdout)


if __name__ == "__main__":
    unittest.main()
