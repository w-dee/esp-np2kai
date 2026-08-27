#!/usr/bin/env python3
"""Host/static contract for P10M-D1 same-binary DMA2D instrumentation."""

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
    require("PROJECT_CALLBACK_CPU_DURING_WAIT" in BENCH and
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
    print("Display Performance P10M-D1 DMA2D benchmark host/static contract passed")
    print("P10M_D1_HARDWARE_ACCESS=NOT_PERFORMED")


if __name__ == "__main__":
    main()
