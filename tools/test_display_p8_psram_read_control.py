#!/usr/bin/env python3
"""Host/source contract checks for the P8 CPU1 PSRAM read control."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_psram_read_control.hpp"
CONTROL = ROOT / "firmware/components/p4_nano_live_display/p4_nano_psram_read_control.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
MAIN = ROOT / "firmware/main/main.cpp"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(text: str, fragment: str, name: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {name}: {fragment}")


def run_header_test() -> None:
    source = r'''
#include <cassert>
#include <cstdint>
#include "p4_nano_live_display/p4_nano_psram_read_control.hpp"
int main() {
    using namespace p4_nano_psram_read_control;
    static_assert(kBufferBytes == 4U * 1024U * 1024U);
    assert(expected_sweep_checksum() != 0U);
    std::uint32_t result = 0U;
    assert(derive_sweeps_per_relief(50000U, 1U, 250000U,
                                    kMaxSweepsPerRelief, &result));
    assert(result == 5U);
    assert(!derive_sweeps_per_relief(0U, 1U, 250000U,
                                     kMaxSweepsPerRelief, &result));
    assert(!derive_sweeps_per_relief(1U, 0U, 250000U,
                                     kMaxSweepsPerRelief, &result));
    assert(!derive_sweeps_per_relief(1U, 1U, 0U,
                                     kMaxSweepsPerRelief, &result));
    assert(!derive_sweeps_per_relief(1U, 1U, 1U, 0U, &result));
    assert(!derive_sweeps_per_relief(UINT64_MAX, UINT32_MAX, UINT32_MAX,
                                     kMaxSweepsPerRelief, &result));
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="p8-psram-host-") as directory:
        binary = pathlib.Path(directory) / "p8-header-test"
        completed = subprocess.run(
            ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
             "-I", str(HEADER.parent.parent), "-x", "c++", "-o",
             str(binary), "-"],
            input=source, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stdout)
        completed = subprocess.run([str(binary)], check=False)
        if completed.returncode != 0:
            raise AssertionError("P8 pattern/calibration test failed")


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    control = CONTROL.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    run_header_test()
    require(build, "--transform-isolated-psram-read-control-benchmark",
            "dedicated P8 selector")
    require(build, "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE",
            "P8 CMake selector")
    require(live_cmake, "p4_nano_psram_read_control.cpp", "P8-only source")
    require(live_cmake, 'PROPERTIES COMPILE_OPTIONS "-O2"',
            "P8 control TU optimization")
    require(main_cmake,
            "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE_ACTIVE",
            "P8 profile gate")
    require(main, "P4_NANO_LIVE_DISPLAY_TRANSFORM_PSRAM_READ_CONTROL_BENCHMARK_PROFILE",
            "main P8 profile dispatch")
    require(live, "P4_NANO_PSRAM_READ_CONTROL_SEQUENCE=A_then_B",
            "fixed A/B order")
    require(live, "benchmark_p8_prepare_buffer", "buffer preparation")
    require(live, "ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |",
            "one-time C2M data invalidate")
    require(live, "P4_NANO_PSRAM_READ_CONTROL_HEALTH", "health output")
    require(live, "last_sweep_checksum != state->p8_expected_checksum",
            "expected checksum gate")
    require(live, "!p4_nano_psram_read_control::begin()",
            "active handshake gate")
    require(live, "p8_control_start_us", "outer B wall start")
    require(live, "p8_control_end_us", "outer B wall end")
    require(live, "payload_mib_per_second", "payload throughput accounting")
    require(header, "active_start_us", "reader active start state")
    require(header, "active_end_us", "reader active end state")
    require(control, "health.active_start_us", "reader active start timestamp")
    require(control, "health.active_end_us", "reader active end timestamp")
    require(live, "reader_active_wall_us", "reader active denominator")
    require(live, "!reader_active_wall_valid", "reader active-window gate")
    require(live, "!payload_rate_valid", "payload rate gate")
    require(live, "health.total_bytes, reader_active_wall_us",
            "reader active throughput denominator")
    task = control[control.index("void read_task(void *)"):control.index(
        "} // namespace")]
    active_start = task.index("health.active_start_us")
    if active_start >= task.index("read_sweep(", active_start):
        raise AssertionError("active start must precede first active sweep")
    if task.index("health.active_end_us") <= task.rfind("read_sweep("):
        raise AssertionError("active end must follow final active sweep")
    if control.index("health.active_start_us") >= control.index(
            "s_runtime.running.store(true"):
        raise AssertionError("active start must precede running publication")
    if control.index("health.active_end_us") >= control.index(
            "s_runtime.running.store(false"):
        raise AssertionError("active end must precede running=false publication")
    isolated_samples = live[live.index("bool benchmark_run_isolated_samples"):
                            live.index("esp_err_t run_isolated_benchmark_after_start")]
    if "xSemaphore" in isolated_samples:
        raise AssertionError("P8 must not add per-transform semaphore synchronization")
    kernel = control[control.index("__attribute__((noinline))"):
                     control.index("bool start_and_calibrate")]
    for forbidden in ("esp_timer_get_time", "vTaskDelay", "xTask",
                      "esp_cache_msync", "printf", "memcpy"):
        if forbidden in kernel:
            raise AssertionError(f"P8 hot kernel contains forbidden call: {forbidden}")
    require(kernel, "input[index + 0U]", "real sequential loads")
    require(control, "xTaskCreateStaticPinnedToCore", "static CPU1 task")
    require(control, "vTaskDelay(1)", "TWDT relief")
    require(control, "DRAM_ATTR Runtime s_runtime", "internal static state")
    require(header, "IRAM_ATTR std::uint32_t read_sweep", "IRAM kernel declaration")
    require(control, "__attribute__((noinline))", "noinline read kernel")
    if "taskYIELD" in control:
        raise AssertionError("P8 must use vTaskDelay(1), not taskYIELD")
    if "esp_task_wdt_" in control or "CONFIG_ESP_TASK_WDT_TIMEOUT_S" in control:
        raise AssertionError("P8 must not change TWDT policy")
    require(live, "benchmark_run_isolated_samples(state, false, false)",
            "Phase A")
    require(live, "benchmark_run_isolated_samples(state, false, true)",
            "Phase B")
    print("Display Performance P8B PSRAM read-control contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
