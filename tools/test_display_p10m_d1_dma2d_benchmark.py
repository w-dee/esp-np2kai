#!/usr/bin/env python3
"""Host/static contract for P10M-D2B DMA2D validity controls."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "tools/emu/build-production.sh").read_text(encoding="utf-8")
ADAPTER = (ROOT / "firmware/components/p4_nano_dma2d_copy/dma2d_copy.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "firmware/components/p4_nano_dma2d_copy/include/p4_nano_dma2d_copy/dma2d_copy.hpp").read_text(encoding="utf-8")
BENCH = (ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_dma2d_benchmark.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt").read_text(encoding="utf-8")
LIVE = (ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp").read_text(encoding="utf-8")


def require(value: bool, message: str) -> None:
    if not value:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    for text, name in ((BUILD, "build"), (CMAKE, "cmake"), (LIVE, "live")):
        require("P4_NANO_EXACT2X_DMA2D_BENCHMARK_PROFILE" in text or
                "exact2x_dma2d_benchmark" in text, f"missing D1 {name} routing")
    require("--exact2x-dma2d-benchmark" in BUILD, "selector missing")
    require("benchmark_display_refresh_profile=lower2" in BUILD,
            "LOWER2 is not forced")
    for marker in ("profile=lower2", "fixture=np2video-7b2d-live-vram",
                   "scene=2", "warmups=8 measured=128 final_validation=1",
                   "ppa_operations=5 horizontal_operations=10 dma_transactions=20"):
        require(marker in BENCH, f"missing benchmark marker: {marker}")
    require("exact2x_pie_tile128_aligned" in BENCH, "CONTROL helper missing")
    require("exact2x_pie_horizontal64_aligned" in BENCH, "DMA helper missing")
    require("grouped64" not in BENCH and "descriptor_chain=0" in BENCH and
            "overlap=0" in BENCH, "forbidden scheduling/geometry change")
    require("CopyTiming" in HEADER and "wait_active" in ADAPTER,
            "adapter timing/wait classification missing")
    require("on_job_cycles_during_wait" in ADAPTER and
            "on_job_cycles_outside_wait" in ADAPTER and
            "eof_cycles_during_wait" in ADAPTER, "callback split missing")
    require("PROJECT_CALLBACK_PRE_SIGNAL_CPU_DURING_WAIT" in BENCH and
            "PRIVATE DRIVER ISR CPU" not in BENCH and
            "private_driver_isr_cpu_outside_project_callbacks=UNMEASURED" in BENCH,
            "private ISR caveat missing")
    require("DMA_NONBLOCKED_TASK_WALL" in BENCH and
            "m.dma_total_us - m.dma_wait_us" in BENCH,
            "non-blocked subtraction missing")
    require("std::memcmp(destination, control_snapshot, kDestinationBytes)" in BENCH,
            "CONTROL-vs-DMA final memcmp missing")
    require("ControlStats s_control_stats" in BENCH and "DmaStats s_dma_stats" in BENCH,
            "static sample storage missing")
    require("dma2d_force_end" not in ADAPTER, "forbidden DMA force-end found")
    require("publish_completion_status(adapter, status)" in ADAPTER and
            "callback_timing_finish(adapter, kind, timing)" in ADAPTER and
            "return give_completion(adapter)" in ADAPTER,
            "commit-before-wake helper missing")
    timed_helper = ADAPTER[ADAPTER.index("signal_timed_completion("):]
    require(timed_helper.index("publish_completion_status(adapter, status);") <
            timed_helper.index("callback_timing_finish(adapter, kind, timing);") <
            timed_helper.index("return give_completion(adapter);"),
            "timed signaling helper ordering missing")
    require("return signal_timed_completion(adapter, ESP_OK, CallbackKind::Eof, timing)"
            in ADAPTER, "EOF timed signaling path missing")
    require("semaphore_give_wakeup_cpu=UNMEASURED_NOT_INCLUDED" in BENCH,
            "semaphore-give exclusion missing")
    require("timing == nullptr" in ADAPTER and
            "P10M-C1C DATA/LIFECYCLE CONTRACT CHANGED = NO" not in ADAPTER,
            "no-timing path changed")

    wrapper_start = LIVE.index(
        "esp_err_t run_exact2x_dma2d_benchmark_after_start")
    wrapper_end = LIVE.index("#endif", wrapper_start)
    wrapper = LIVE[wrapper_start:wrapper_end]
    pause_index = wrapper.index("pause_stable_pre_resume")
    clear_index = wrapper.index(
        "state->producer_pause_requested.store(false")
    resume_index = wrapper.index("xSemaphoreGive(state->isolated_pause_resume)")
    require(pause_index < clear_index < resume_index,
            "pause stability must be captured before producer resume")
    for marker in ("producer_pause_acknowledged_after_resume",
                   "pause_ack_after_resume", "P4_NANO_EXACT2X_DMA2D_BENCHMARK_LIFECYCLE",
                   "body_result=%s", "publish_failed=%d",
                   "pause_stable_pre_resume=%d", "resumed=%d",
                   "backlight_off_ok=%d", "leases_balanced=%d",
                   "producer_result=%s", "scheduling_contract=%s",
                   "vsync_valid=%s", "display_cleanup=%s", "result=%s"):
        require(marker in wrapper, f"lifecycle marker field missing: {marker}")
    for marker in ("kBenchmarkProducerCore", "kBenchmarkProducerPriority",
                   "xPortGetCoreID() == 0",
                   "uxTaskPriorityGet(nullptr)) == 1U"):
        require(marker in wrapper, f"runtime scheduling contract missing: {marker}")
    require("const bool display_cleanup_ok = cleanup_result == ESP_OK" in wrapper and
            "return display_cleanup_ok && lifecycle_ok" in wrapper,
            "cleanup result is not part of final lifecycle result")

    run_start = BENCH.index("esp_err_t run(const Input &input)")
    run_body = BENCH[run_start:]
    require("P4_NANO_EXACT2X_DMA2D_PPA_SENTINEL_CONFIG" in run_body,
            "neutral PPA sentinel config marker missing")
    for marker in ("warmups=8 measured=128", "ppa_operations=5", "pie=0",
                   "dma=0", "destination_psram_write=0", "burst=128",
                   "blocking=1", "sentinel_timer_reads=", "storage_bytes=%zu"):
        require(marker in run_body, f"sentinel config field missing: {marker}")
    require("MetricStats s_pre_control_ppa_stats" in BENCH and
            "MetricStats s_pre_dma_ppa_stats" in BENCH and
            "kSentinelStorageBytes == 2048U" in BENCH,
            "sentinel static storage missing")
    sentinel_start = BENCH.index("bool run_neutral_ppa_sample")
    sentinel_end = BENCH.index("} // namespace", sentinel_start)
    sentinel = BENCH[sentinel_start:sentinel_end]
    require("prepare_tile" in sentinel and
            "PPA_TRANS_MODE_BLOCKING" in BENCH,
            "sentinel does not use the blocking PPA helper")
    for forbidden in ("exact2x_pie_", "copy_strided", "esp_cache_msync",
                      "destination_writeback", "crc32", "memcmp"):
        require(forbidden not in sentinel,
                f"sentinel is not neutral; found {forbidden}")
    require("run_neutral_ppa_sentinel" in sentinel and
            "ppa_unregister_client(client)" in sentinel and
            "(index + 1U) % 64U == 0U" in sentinel,
            "sentinel lifecycle/health cadence missing")
    require(run_body.count("run_neutral_ppa_sentinel(input.original_source, tile") == 2,
            "sentinels must use the same held source and internal tile")
    pre_control = run_body.index("&s_pre_control_ppa_stats")
    control_client = run_body.index("ppa_client_handle_t control_client")
    pre_dma = run_body.index("&s_pre_dma_ppa_stats")
    dma_adapter = run_body.index("dma2d::Adapter *adapter")
    require(pre_control < control_client < pre_dma < dma_adapter,
            "sentinel phase sequence is not PRE_CONTROL -> CONTROL -> PRE_DMA2D -> DMA2D")
    require('print_metric("PRE_CONTROL", "PPA_NEUTRAL_WALL"' in run_body and
            'print_metric("PRE_DMA2D", "PPA_NEUTRAL_WALL"' in run_body,
            "sentinel metric output missing")
    require("PPA_COMMON_WALL" in BENCH and "PPA_DRIFT" not in BENCH,
            "old phase-PPA validity gate remains in firmware")
    require("timer_reads_control=24 timer_reads_dma=112 cycle_reads_dma=80" in BENCH,
            "formal timer/cycle accounting changed")
    print("Display Performance P10M-D2B DMA2D validity-control host/static contract passed")
    print("P10M_D2B_HARDWARE_ACCESS=NOT_PERFORMED")


if __name__ == "__main__":
    main()
