#!/usr/bin/env python3
"""Host-only contracts for the A3.6O runtime observability slice."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "firmware/components/p4_nano_audio_i2s_opngen/"
          "p4_nano_audio_i2s_opngen.cpp").read_text(encoding="utf-8")


def active_compute_us(step_elapsed_us: int, full_wait_before_us: int,
                      full_wait_after_us: int) -> int:
    """Mirror the fail-closed active-compute subtraction contract."""
    if step_elapsed_us < 0 or full_wait_before_us < 0:
        raise ValueError("negative timing input")
    if full_wait_after_us < full_wait_before_us:
        raise ValueError("full-wait counter moved backwards")
    full_wait_delta_us = full_wait_after_us - full_wait_before_us
    if full_wait_delta_us > step_elapsed_us:
        raise ValueError("full-wait duration exceeds worker-step duration")
    return step_elapsed_us - full_wait_delta_us


def lifecycle_completion(*, failed: bool, waits_pass: bool, producer_ack: bool,
                         worker_ack: bool, consumer_ack: bool,
                         workload_done: bool) -> tuple[str, int]:
    graceful = (not failed and waits_pass and producer_ack and worker_ack and
                consumer_ack and workload_done)
    return ("GRACEFUL" if graceful else "FORCED", 1 if graceful else 0)


class A36OObservabilityTests(unittest.TestCase):
    def test_esp_timer_is_the_only_formal_performance_clock(self) -> None:
        self.assertIn('#include "esp_timer.h"', SOURCE)
        self.assertGreaterEqual(SOURCE.count("esp_timer_get_time()"), 6)
        self.assertNotIn("xTaskGetTickCount", SOURCE)
        for field in ("full_wait_total_us", "full_wait_max_us",
                      "opngen_service_total_us", "compute_service_total_us"):
            self.assertIn(field, SOURCE)
        for forbidden in ("full_wait_ticks", "latency_ticks", "opngen_ticks",
                          "compute_ticks", "min_ticks", "mean_ticks"):
            self.assertNotIn(forbidden, SOURCE)

    def test_timeout_contract_is_literal_milliseconds(self) -> None:
        self.assertRegex(SOURCE, r"constexpr uint32_t kWriteTimeoutMs = 1000U;")
        self.assertIn("static_assert(kWriteTimeoutMs == 1000U", SOURCE)
        write_at = SOURCE.index("i2s_channel_write(")
        consume_at = SOURCE.index("np2opngen_pcm_ring_consume", write_at)
        write_region = SOURCE[write_at:consume_at]
        self.assertIn("kWriteTimeoutMs", write_region)
        self.assertNotIn("pdMS_TO_TICKS", write_region)
        self.assertIn("A3_I2S_WRITE_TIMEOUT_UNIT_CONTRACT=PASS", SOURCE)

    def test_write_latency_storage_and_schema(self) -> None:
        self.assertIn("uint32_t *latency_us", SOURCE)
        self.assertIn("heap_caps_calloc", SOURCE)
        for field in ("count", "min_us", "mean_us", "p50_us", "p90_us",
                      "p95_us", "p99_us", "max_us"):
            self.assertIn(field, SOURCE)
        self.assertIn("elapsed_us > UINT32_MAX", SOURCE)
        self.assertIn("timing_failure(ctx)", SOURCE)

    def test_active_compute_separation_model(self) -> None:
        self.assertEqual(active_compute_us(5000, 1000, 5000), 1000)
        self.assertEqual(active_compute_us(5000, 4000, 4000), 5000)
        self.assertEqual(active_compute_us(9000, 1000, 3000), 7000)
        with self.assertRaises(ValueError):
            active_compute_us(1000, 4000, 6001)
        with self.assertRaises(ValueError):
            active_compute_us(5000, 4000, 3000)

    def test_compute_log_is_explicitly_separated(self) -> None:
        compute = SOURCE[SOURCE.index("P4_AUDIO_I2S_OPNGEN_COMPUTE"):]
        for field in ("opngen_call_count", "opngen_service_total_us",
                      "opngen_service_mean_us", "opngen_service_max_us",
                      "compute_step_count", "compute_service_total_us",
                      "compute_service_mean_us", "compute_service_max_us",
                      "i2s_wait_separate=YES", "ring_full_wait_separate=YES"):
            self.assertIn(field, compute)

    def test_lifecycle_success_requires_every_ack_and_wait(self) -> None:
        self.assertEqual(lifecycle_completion(
            failed=False, waits_pass=True, producer_ack=True,
            worker_ack=True, consumer_ack=True, workload_done=True),
            ("GRACEFUL", 1))
        for kwargs in (
            dict(failed=True, waits_pass=True, producer_ack=True,
                 worker_ack=True, consumer_ack=True, workload_done=True),
            dict(failed=False, waits_pass=False, producer_ack=True,
                 worker_ack=True, consumer_ack=True, workload_done=True),
            dict(failed=False, waits_pass=True, producer_ack=False,
                 worker_ack=True, consumer_ack=True, workload_done=True),
            dict(failed=False, waits_pass=True, producer_ack=True,
                 worker_ack=True, consumer_ack=True, workload_done=False),
        ):
            self.assertEqual(lifecycle_completion(**kwargs), ("FORCED", 0))

    def test_lifecycle_failure_cases_are_forced(self) -> None:
        cases = {
            "producer failure before terminal ack":
                dict(failed=True, waits_pass=False, producer_ack=False,
                     worker_ack=False, consumer_ack=False, workload_done=False),
            "worker failure":
                dict(failed=True, waits_pass=False, producer_ack=True,
                     worker_ack=False, consumer_ack=True, workload_done=False),
            "consumer I2S failure":
                dict(failed=True, waits_pass=True, producer_ack=True,
                     worker_ack=True, consumer_ack=False, workload_done=False),
            "terminal wait timeout":
                dict(failed=False, waits_pass=False, producer_ack=True,
                     worker_ack=True, consumer_ack=True, workload_done=True),
            "partial task creation":
                dict(failed=True, waits_pass=False, producer_ack=False,
                     worker_ack=True, consumer_ack=False, workload_done=False),
        }
        for name, kwargs in cases.items():
            with self.subTest(name=name):
                completion, quiesced = lifecycle_completion(**kwargs)
                self.assertEqual(completion, "FORCED")
                self.assertNotEqual(quiesced, 1)

    def test_terminal_ack_is_before_terminal_semaphore(self) -> None:
        for task in ("producer", "worker", "consumer"):
            ack = SOURCE.index(f"{task}_terminal_ack.store")
            sem = SOURCE.index(f"{task}_terminal != nullptr", ack)
            self.assertLess(ack, sem)
        self.assertIn("tasks_reaped", SOURCE)
        self.assertIn("completion=%s", SOURCE)
        self.assertIn("producer_ack=%u", SOURCE)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
