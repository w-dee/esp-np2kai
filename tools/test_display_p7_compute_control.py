#!/usr/bin/env python3
"""Host/source contract checks for the P7 CPU1 compute-only A/B control."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_compute_control.hpp"
CONTROL = ROOT / "firmware/components/p4_nano_live_display/p4_nano_compute_control.cpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(text: str, fragment: str, name: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {name}: {fragment}")


def run_header_tests() -> None:
    program = r'''
#include <cassert>
#include <cstdint>
#include "p4_nano_live_display/p4_nano_compute_control.hpp"
int main() {
    using namespace p4_nano_compute_control;
    const std::uint32_t a = recurrence(0x13579bdfU, 17U);
    const std::uint32_t b = recurrence(0x13579bdfU, 17U);
    assert(a == b);
    assert(a != 0x13579bdfU);
    std::uint32_t iterations = 0U;
    assert(derive_chunk_iterations(1000U, 100000U, 250000U, &iterations));
    assert(iterations == 25000000U);
    assert(!derive_chunk_iterations(0U, 100000U, 250000U, &iterations));
    assert(!derive_chunk_iterations(1000U, 0U, 250000U, &iterations));
    assert(!derive_chunk_iterations(1U, 100000U, 250000U, &iterations));
    assert(!derive_chunk_iterations(1000U, 100000U, 0U, &iterations));
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="p7-compute-host-") as directory:
        binary = pathlib.Path(directory) / "compute-control-test"
        completed = subprocess.run(
            ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
             "-I", str(HEADER.parent.parent), "-x", "c++", "-o",
             str(binary), "-"],
            input=program, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stdout)
        completed = subprocess.run([str(binary)], check=False)
        if completed.returncode != 0:
            raise AssertionError("header recurrence/calibration test failed")


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    control = CONTROL.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    run_header_tests()
    require(build, "--transform-isolated-compute-control-benchmark",
            "dedicated P7 selector")
    require(live_cmake, "p4_nano_compute_control.cpp", "P7-only source")
    require(live_cmake, 'PROPERTIES COMPILE_OPTIONS "-O2"',
            "control TU optimization")
    require(main_cmake, "P4_NANO_LIVE_DISPLAY_TRANSFORM_COMPUTE_CONTROL_BENCHMARK_PROFILE_ACTIVE",
            "P7 profile gate")
    require(live, "P4_NANO_COMPUTE_CONTROL_SEQUENCE=A_then_B", "fixed A/B order")
    require(live, "benchmark_run_isolated_samples(state, false)", "A phase")
    require(live, "benchmark_run_isolated_samples(state, true)", "B phase")
    require(live, "!p4_nano_compute_control::begin()",
            "B blocked after begin failure")
    require(live, "layout_validity ? \"PASS\" : \"FAIL\"",
            "explicit layout diagnostic")
    require(live, "!layout_validity", "layout validity final gate")
    for layout_check in (
        "p4_nano_compute_control::stack_internal()",
        "p4_nano_compute_control::tcb_internal()",
        "p4_nano_compute_control::state_internal()",
        "esp_ptr_in_iram",
        "esp_ptr_executable",
    ):
        require(live, layout_check, f"layout hard gate {layout_check}")
    isolated_samples = live[live.index("bool benchmark_run_isolated_samples"):
                            live.index("esp_err_t run_isolated_benchmark_after_start")]
    if "xSemaphore" in isolated_samples:
        raise AssertionError("P7 must not add per-transform semaphore synchronization")
    begin = control[control.index("bool begin()"):
                    control.index("bool stop()")]
    require(begin, "s_runtime.running.load(std::memory_order_acquire)",
            "active-state acquire wait")
    require(begin, "xTaskGetTickCount()", "bounded active-state timeout")
    require(begin, "return false", "bounded begin failure")
    require(control, "xTaskCreateStaticPinnedToCore", "static CPU1 task")
    require(control, "vTaskDelay(1)", "TWDT idle relief")
    require(control, "IRAM_ATTR std::uint32_t run_chunk", "IRAM hot loop")
    require(control, "DRAM_ATTR Runtime s_runtime", "internal static state")
    if "taskYIELD" in control:
        raise AssertionError("taskYIELD must not be the TWDT relief primitive")
    loop = control[control.index("IRAM_ATTR std::uint32_t run_chunk"):]
    if "esp_timer_get_time" in loop:
        raise AssertionError("timer reads must not be in the hot-loop function")
    if "P4_NANO_TASK_WDT" in control or "esp_task_wdt_" in control:
        raise AssertionError("P7 must not change or bypass TWDT")
    print("Display Performance P7B.1 compute-control validity contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
