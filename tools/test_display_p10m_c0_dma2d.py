#!/usr/bin/env python3
"""Host/static contract for the P10M-C0 private DMA2D prototype."""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "firmware/components/p4_nano_dma2d_copy/dma2d_copy.cpp"
ADAPTER_HEADER = ROOT / "firmware/components/p4_nano_dma2d_copy/include/p4_nano_dma2d_copy/dma2d_copy.hpp"
PROTO = ROOT / "firmware/components/p4_nano_live_display/p4_nano_exact2x_dma2d.cpp"
PROTO_HEADER = ROOT / "firmware/components/p4_nano_live_display/include/p4_nano_live_display/p4_nano_exact2x_dma2d.hpp"
LIVE = ROOT / "firmware/components/p4_nano_live_display/p4_nano_live_display.cpp"
LIVE_CMAKE = ROOT / "firmware/components/p4_nano_live_display/CMakeLists.txt"
DISPLAY_CMAKE = ROOT / "firmware/components/p4_nano_display/CMakeLists.txt"
ASM = ROOT / "firmware/components/p4_nano_display/p4_nano_display_exact2x_pie.S"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
BUILD = ROOT / "tools/emu/build-production.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def objdump_path() -> str | None:
    found = shutil.which("riscv32-esp-elf-objdump")
    if found:
        return found
    candidates = sorted((pathlib.Path.home() / ".espressif" / "tools").glob(
        "riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump"))
    return str(candidates[-1]) if candidates else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=pathlib.Path,
                        help="optional C0 ELF for symbol/disassembly checks")
    args = parser.parse_args()

    adapter = ADAPTER.read_text(encoding="utf-8")
    adapter_header = ADAPTER_HEADER.read_text(encoding="utf-8")
    proto = PROTO.read_text(encoding="utf-8")
    proto_header = PROTO_HEADER.read_text(encoding="utf-8")
    live = LIVE.read_text(encoding="utf-8")
    live_cmake = LIVE_CMAKE.read_text(encoding="utf-8")
    display_cmake = DISPLAY_CMAKE.read_text(encoding="utf-8")
    assembly = ASM.read_text(encoding="utf-8")
    main_cpp = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    integration = (adapter + adapter_header + proto + proto_header + live +
                   live_cmake + display_cmake + main_cpp + main_cmake + build)

    for fragment in (
        "--exact2x-dma2d-correctness",
        "P4_NANO_EXACT2X_DMA2D_CORRECTNESS_PROFILE",
        "p4_nano_dma2d_copy",
        "P4_NANO_EXACT2X_DMA2D_CORRECTNESS_VARIANT",
        "P4_NANO_EXACT2X_DMA2D_CORRECTNESS_PROFILE=1",
    ):
        require(fragment in integration, f"missing C0 routing: {fragment}")
    require("p4-v1x" in build and "p4-nano" in build,
            "C0 board/variant gate missing")
    require("P4_NANO_DSI_USE_DMA2D" in main_cmake and
            "requires DSI DMA2D copy disabled" in main_cmake,
            "C0 must keep DSI DMA2D scanout disabled")

    for fragment in (
        "ESP_IDF_VERSION_VAL(5, 5, 4)",
        "CONFIG_IDF_TARGET_ESP32P4", "SOC_DMA2D_SUPPORTED",
        "SOC_PSRAM_DMA_CAPABLE", "esp_private/dma2d.h",
        "hal/dma2d_types.h", "soc/dma2d_channel.h",
        "DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING",
        "SOC_DMA2D_TRIG_PERIPH_M2M_TX",
        "SOC_DMA2D_TRIG_PERIPH_M2M_RX",
        "dma2d_acquire_pool", "dma2d_release_pool", "dma2d_enqueue",
        "dma2d_force_end", "dma2d_set_desc_addr", "dma2d_start",
        "xSemaphoreGiveFromISR", "xSemaphoreTake",
        "state.store(State::RetainedAmbiguous",
    ):
        require(fragment in adapter, f"adapter contract missing: {fragment}")
    require(adapter.count("dma2d_enqueue(") == 1,
            "one private adapter transaction must be enqueued per pass")
    require(adapter.count("dma2d_start(") == 2,
            "sibling TX/RX start pair missing")
    require("dma2d_append" not in adapter and "next = nullptr" in adapter,
            "C0 must use fixed single descriptors without chaining")
    require("kSourceWidthPixels = 800U" in adapter_header and
            "kDestinationVirtualWidthPixels = 1600U" in adapter_header and
            "kChunkRows = 64U" in adapter_header,
            "DMA2D virtual geometry changed")
    require("kEvenXOffsetPixels = 0U" in adapter_header and
            "kOddXOffsetPixels = 800U" in adapter_header,
            "even/odd destination offsets missing")
    require("kDescriptorAlignment = 64U" in adapter and
            "kDescriptorStorageBytes = 64U" in adapter and
            "sizeof(dma2d_descriptor_t) == 24U" in adapter,
            "descriptor retained-storage contract missing")

    # The private header must stay quarantined in the adapter component.
    private_users = []
    for path in (ROOT / "firmware/components").rglob("*"):
        if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
            if "esp_private/dma2d.h" in path.read_text(encoding="utf-8"):
                private_users.append(path.relative_to(ROOT).as_posix())
    require(private_users == [ADAPTER.relative_to(ROOT).as_posix()],
            f"private DMA2D API leaked outside adapter: {private_users}")

    for fragment in (
        "kTileBytes = 102400U", "kStagingBytes = 102400U",
        "kDestinationBytes = 2048000U", "kTileCount = 5U",
        "kChunkBytes = 51200U", "kChunkDestinationDelta = 204800U",
        "kExpectedDestinationCrc = 0xc8a10b55U",
        "kExpectedRotatedCrc = 0x379511d7U",
        "kExpectedSourceCrc = 0x8dadbf82U",
        "PPA_TRANS_MODE_BLOCKING", "max_pending_trans_num = 1U",
        "exact2x_pie_horizontal64_aligned",
        "kEvenXOffsetPixels", "kOddXOffsetPixels",
        "std::memcmp(destination, reference, kDestinationBytes)",
        "expected_frame_matches", "source_crc_before",
        'print_memory_preflight("before_alloc")',
        'print_memory_preflight("after_alloc")',
        "P4_NANO_EXACT2X_DMA2D_CLEANUP result=RETAINED",
    ):
        require(fragment in proto, f"prototype correctness contract missing: {fragment}")
    require("PPA_TRANS_MODE_NON_BLOCKING" not in proto,
            "C0 PPA preparation must remain blocking")
    require("esp_timer_get_time" not in proto,
            "C0 must not add timing instrumentation")
    require(proto.index("prepare_tile") < proto.index("horizontal64_aligned") <
            proto.index("copy_strided"),
            "required PPA -> horizontal PIE -> DMA order changed")
    require(proto.index("kEvenXOffsetPixels") <
            proto.index("kOddXOffsetPixels"),
            "even DMA pass must precede odd DMA pass")
    require("lifetime_must_be_retained() noexcept" in proto_header,
            "ambiguous cleanup lifetime result is not exposed")

    helper_start = assembly.index("exact2x_pie_horizontal64_aligned:")
    helper_end = assembly.index(".size exact2x_pie_horizontal64_aligned", helper_start)
    helper = assembly[helper_start:helper_end]
    for fragment in ("li      a5, 64", "li      a4, 50",
                     "esp.vld.128.ip  q0, a0, 16",
                     "esp.orq         q1, q0, q0",
                     "esp.vzip.16     q0, q1",
                     "esp.vst.128.ip  q0, a1, 16",
                     "esp.vst.128.ip  q1, a1, 16"):
        require(fragment in helper, f"horizontal PIE helper changed: {fragment}")
    require(not re.search(r"\bq[2-7]\b", helper),
            "horizontal helper must use q0/q1 only")
    require("addi    a5, a5, -1" in helper and
            "bnez    a5, .Lexact2x_horizontal64_row" in helper,
            "horizontal helper row loop missing")

    require("P4_NANO_EXACT2X_DMA2D_CORRECTNESS_PROFILE" in live and
            "run_exact2x_dma2d_correctness_after_start" in live,
            "live C0 dispatch missing")
    require("p4_nano_exact2x_dma2d.cpp" in live_cmake and
            "p4_nano_exact2x_internal_source.cpp" in live_cmake,
            "C0 source linkage missing")
    require("p4_nano_display_exact2x_pie.S" in display_cmake,
            "C0 horizontal PIE object linkage missing")
    require("P4_NANO_EXACT2X_DMA2D_MODE" in live and
            "descriptor_chain=0 overlap=0" in live,
            "C0 mode marker missing")

    if args.elf is not None:
        require(args.elf.is_file(), f"ELF does not exist: {args.elf}")
        objdump = objdump_path()
        require(objdump is not None, "riscv32-esp-elf-objdump is required for --elf")
        symbols = subprocess.run([objdump, "-t", str(args.elf)], check=True,
                                 capture_output=True, text=True).stdout
        require("exact2x_pie_horizontal64_aligned" in symbols,
                "C0 ELF is missing horizontal PIE helper")
        disassembly = subprocess.run([objdump, "-d", "-C", str(args.elf)],
                                     check=True, capture_output=True,
                                     text=True).stdout
        start = disassembly.index("<exact2x_pie_horizontal64_aligned>:")
        end = disassembly.find("\n\n", start)
        elf_helper = disassembly[start:] if end < 0 else disassembly[start:end]
        require("vld" in elf_helper and "vzip" in elf_helper and
                "q0" in elf_helper and "q1" in elf_helper,
                "C0 ELF horizontal helper instructions missing")
        require(not re.search(r"\bq[2-7]\b", elf_helper),
                "C0 ELF horizontal helper clobbers forbidden q registers")
        print("P10M_C0_ELF_HORIZONTAL_HELPER=PASS")

    print("Display Performance P10M-C0 DMA2D correctness host/static contract passed")
    print("P10M_C0_HARDWARE_ACCESS=NOT_PERFORMED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
