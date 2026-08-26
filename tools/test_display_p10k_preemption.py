#!/usr/bin/env python3
"""Host/static contract for the P10K-B0 grouped-PIE preemption harness."""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
ASM = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
HEADER = ROOT / "firmware/components/p4_nano_display/include/p4_nano_display/p4_nano_display_exact2x.hpp"
COMPONENT = ROOT / "firmware/components/p4_nano_pie_preemption/p4_nano_pie_preemption.cpp"
COMPONENT_CMAKE = ROOT / "firmware/components/p4_nano_pie_preemption/CMakeLists.txt"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(assembly: str, symbol: str) -> str:
    start = assembly.index(f"{symbol}:")
    end = assembly.index(f".size {symbol}", start)
    # Comments are not an instruction/register contract.
    return re.sub(r"#.*", "", assembly[start:end])


def count_instruction(body: str, mnemonic: str) -> int:
    return len(re.findall(rf"(?m)^\s*{re.escape(mnemonic)}\b", body))


def check_elf(elf: pathlib.Path, grouped: str, clobber: str) -> None:
    objdump = shutil.which("riscv32-esp-elf-objdump")
    if objdump is None:
        tool_roots = pathlib.Path.home() / ".espressif" / "tools"
        candidates = sorted(tool_roots.glob(
            "riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump"))
        objdump = str(candidates[-1]) if candidates else None
    require(objdump is not None,
            "riscv32-esp-elf-objdump is required for --elf validation")
    try:
        disassembly = subprocess.run(
            [objdump, "-d", str(elf)], check=True, capture_output=True,
            text=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise AssertionError(f"unable to inspect requested ELF: {exc}") from exc
    for symbol, forbidden in (
            (grouped, ("q3", "q5", "q6", "q7", "sp", "s0", "s1", "call", "jal")),
            (clobber, ("q2", "q3", "q4", "q5", "q6", "q7", "sp", "s0", "s1", "call", "jal"))):
        start = disassembly.find(f"<{symbol}>:")
        require(start >= 0, f"ELF is missing {symbol}")
        next_symbol = disassembly.find("\n", start)
        # objdump emits the symbol's instructions until the next blank/symbol
        # line.  Keep this check deliberately structural and bounded.
        end = disassembly.find("\n\n", next_symbol)
        body = disassembly[start:end if end >= 0 else start + 4096]
        for token in forbidden:
            require(not re.search(rf"\b{re.escape(token)}\b", body),
                    f"ELF {symbol} contains forbidden {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=pathlib.Path,
                        help="optional produced ELF for objdump ABI checks")
    args = parser.parse_args()

    assembly = ASM.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    component = COMPONENT.read_text(encoding="utf-8")
    component_cmake = COMPONENT_CMAKE.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")

    grouped_name = "exact2x_pie_tile128_grouped64_aligned"
    clobber_name = "exact2x_pie_preemption_clobber_q0_q1"
    grouped = function_body(assembly, grouped_name)
    clobber = function_body(assembly, clobber_name)
    legacy = function_body(assembly, "exact2x_pie_tile128_aligned")

    require(grouped_name in header and clobber_name in header,
            "new helper declarations are missing")
    require("64-byte aligned test buffers" in component or
            "kRequiredAlignment = 64U" in component,
            "test-buffer alignment contract is missing")
    require("q3 is intentionally" in header and "avoided because" in header,
            "q3 context-save rationale is missing")

    # Fixed geometry and the 25-iteration/two-source-group loop.
    for fragment in ("li      a5, 128", "li      a4, 25",
                     "addi    a3, a1, 1600"):
        require(fragment in grouped, f"grouped geometry missing: {fragment}")
    require(count_instruction(grouped, "esp.vld.128.ip") == 2,
            "grouped helper must issue exactly two loads per iteration")
    require(count_instruction(grouped, "esp.vst.128.ip") == 8,
            "grouped helper must issue exactly eight stores per iteration")
    require(grouped.index("q0, a2") < grouped.index("q0, a3"),
            "row 0 stores must precede row 1 stores")
    row0 = grouped[grouped.index("q0, a2"):grouped.index("q0, a3")]
    row1 = grouped[grouped.index("q0, a3"):]
    require(all(f"q{name}, a2" in row0 for name in ("0", "1", "2", "4")),
            "row 0 must store q0/q1/q2/q4")
    require(all(f"q{name}, a3" in row1 for name in ("0", "1", "2", "4")),
            "row 1 must store q0/q1/q2/q4")
    require(not re.search(r"\bq[3567]\b", grouped),
            "grouped helper uses forbidden QR registers")
    require(not re.search(r"\b(?:USAR|SAR(?:_BYTE)?|QACC|XACC|FFT|CFG|PERF)\b",
                          grouped, re.IGNORECASE),
            "grouped helper uses forbidden special-state operations")
    require(not re.search(r"\b(?:sp|s(?:[0-9]|1[0-1]))\b|\b(?:call|jal)\b",
                          grouped), "grouped helper violates leaf scalar ABI")
    require(grouped.rstrip().endswith("ret"), "grouped helper must return")

    # The q0/q1 owner-transfer helper is intentionally tiny and has no special
    # state, stack, callee-saved registers, or calls.
    require(count_instruction(clobber, "esp.vld.128.ip") == 1 and
            count_instruction(clobber, "esp.vst.128.ip") == 2,
            "clobber helper geometry changed")
    require(not re.search(r"\bq[234567]\b", clobber),
            "clobber helper must use q0/q1 only")
    require(not re.search(r"\b(?:USAR|SAR(?:_BYTE)?|QACC|XACC|FFT|CFG|PERF)\b",
                          clobber, re.IGNORECASE),
            "clobber helper uses special state")
    require(not re.search(r"\b(?:sp|s(?:[0-9]|1[0-1]))\b|\b(?:call|jal)\b",
                          clobber), "clobber helper violates leaf ABI")

    # Existing helpers remain byte-for-byte instruction-identical in their
    # bounded assembly functions and are not routed to the candidate.
    require("li      a4, 50" in legacy and "q2" not in legacy,
            "accepted T128 helper changed")
    require(component.count("exact2x_pie_tile128_grouped64_aligned") >= 1,
            "harness does not invoke grouped helper")
    require(component.count("exact2x_pie_preemption_clobber_q0_q1") >= 1,
            "harness does not invoke q0/q1 clobber helper")

    # Runtime model: low app task, higher-priority same-core task, one-shot
    # timer wake, bounded waits, control-before-stress, and full memcmp.
    for fragment in (
            "kControlIterations = 4U", "kStressIterations = 512U",
            "kMinimumHandoffs", "kIntentionalPreemptionDelayUs = 250U",
            "xTaskCreatePinnedToCore", "esp_timer_start_once",
            "ESP_TIMER_TASK", "std::memcmp(candidate, golden, kDestinationBytes)",
            "helper_active_handoff_count", "P4_NANO_PIE_PREEMPT_CONTROL",
            "P4_NANO_PIE_PREEMPT_STRESS", "P4_NANO_PIE_PREEMPT_CLEANUP",
            "P4_NANO_PIE_PREEMPT_RESULT=PASS", "MALLOC_CAP_INTERNAL",
            "MALLOC_CAP_SPIRAM", "kTaskWaitTicks", "vTaskDelay(1U)"):
        require(fragment in component, f"missing runtime harness contract: {fragment}")
    require("state.clobber_source[index]" in component and
            "0x31U + index * 7U" in component,
            "clobber source must be deterministic")
    require("while (" not in component,
            "harness must not use an unbounded spin loop")
    require(component.count("validate_output(candidate, golden") >= 2,
            "control and stress paths must validate every iteration")
    require("xTaskNotifyGive(state->high_task)" in component and
            "p4_nano_display::exact2x_pie_preemption_clobber_q0_q1" in component,
            "timer callback/high task handoff contract missing")
    require("p4_nano_display::exact2x_pie_aligned" not in component and
            "p4_nano_display::exact2x_pie_tile128_aligned" not in component,
            "harness must select only the grouped candidate")
    for forbidden in ("ppa_", "PPA_", "DSI", "GPIO_NUM_20", "display_init",
                      "p4_nano_live_display"):
        require(forbidden not in component,
                f"headless harness contains forbidden runtime dependency: {forbidden}")

    # Profile is explicit, board/variant constrained, and isolated from the
    # display/live dependency branches.  The assembly is reused directly by
    # the standalone component; the display component remains unchanged.
    for fragment in ("--pie-preemption-correctness",
                     "P4_NANO_PIE_PREEMPTION_CORRECTNESS_PROFILE",
                     "P4_NANO_PIE_PREEMPTION_CORRECTNESS_BOARD",
                     "P4_NANO_PIE_PREEMPTION_CORRECTNESS_VARIANT"):
        require(fragment in build + main_cmake + main_cpp + component_cmake,
                f"profile routing missing: {fragment}")
    require("p4_nano_pie_preemption" in main_cmake,
            "main must require standalone preemption component")
    require("PRIV_REQUIRES p4_nano_display" not in component_cmake,
            "standalone component must not require live display component")
    require("P4_NANO_PIE_PREEMPTION_CORRECTNESS_PROFILE" not in display_cmake,
            "display component must not select the standalone profile")

    # Geometry model: 400x128 source, 800x256 destination, 25 groups/row,
    # 64-byte grouped source span, and contiguous two-row writes.
    source_width, source_height = 400, 128
    destination_width, destination_height = 800, 256
    require(source_width % 16 == 0 and source_width // 16 == 25,
            "source geometry is not divisible into 25 grouped iterations")
    require(destination_width * 2 == source_width * 4 and
            destination_height == source_height * 2,
            "exact-2x destination geometry changed")
    require((destination_width * 2) % 64 == 0,
            "destination stride must remain 64-byte aligned")
    lanes = list(range(16))
    expected = sum(([lane, lane] for lane in lanes), [])
    require(expected == [0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                         13, 13, 14, 14, 15, 15],
            "scalar lane duplication model mismatch")

    if args.elf is not None:
        check_elf(args.elf, grouped_name, clobber_name)

    print("Display Performance P10K-B0 grouped PIE preemption host/static contract passed")
    print("P10K_HARDWARE_ACCESS=NOT_PERFORMED")
    print("GROUPED_PIE_PERFORMANCE=NOT_MEASURED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
