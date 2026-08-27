#!/usr/bin/env python3
"""Host/static contract for the P10M-C0 private DMA2D prototype."""

from __future__ import annotations

import argparse
import json
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
LIVE_GOLDEN = ROOT / "tests/guest/np2video-live/golden.json"
TEXT_GOLDEN = ROOT / "tests/guest/np2video/golden.json"


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

    descriptor_start = build.index("np2video_descriptor=")
    descriptor_end = build.index("\nfi", descriptor_start)
    descriptor_selector = build[descriptor_start:descriptor_end]
    require("exact2x_dma2d_correctness" in descriptor_selector and
            "tests/guest/np2video-live/golden.json" in descriptor_selector,
            "C1 selector must choose the live NP2VIDEO descriptor")
    header_selector_starts = [match.start() for match in re.finditer(
        r"if \(\( live_display \|\|", build[descriptor_end:])]
    require(len(header_selector_starts) >= 2,
            "C1 must retain initial and persistent live-header selectors")
    for relative_start in header_selector_starts:
        header_selector_start = descriptor_end + relative_start
        header_selector_end = build.index("\nfi", header_selector_start)
        header_selector = build[header_selector_start:header_selector_end]
        require("exact2x_dma2d_correctness" in header_selector,
                "C1 live-header selector omitted from one generation path")
    live_golden = json.loads(LIVE_GOLDEN.read_text(encoding="utf-8"))
    text_golden = json.loads(TEXT_GOLDEN.read_text(encoding="utf-8"))
    require(live_golden.get("schema_version") == 1 and
            live_golden.get("fixture_id") == "np2video-7b2d-live-vram" and
            live_golden.get("scene_id") == 2 and
            live_golden.get("fixture_sha256") ==
                "81975ad74c7b1769a5aa63977ee9c18b020d6381e858522cb4cb7c7861f85604" and
            live_golden.get("image_size") == 1261568,
            "live fixture metadata changed")
    require(live_golden != text_golden,
            "fixture selector regression must distinguish live and text metadata")

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
        "dma2d_set_desc_addr", "dma2d_start",
        "xSemaphoreGiveFromISR", "xSemaphoreTake",
        "State::RetainedAmbiguous",
        "static_assert(std::atomic<State>::is_always_lock_free)",
        "terminalize_ambiguous", "activate_from_queue",
        "complete_from_active", "return_to_idle", "signal_timed_completion",
    ):
        require(fragment in adapter, f"adapter contract missing: {fragment}")
    require("dma2d_force_end" not in adapter,
            "v5.5.4 force_end must be disabled in the private adapter")
    destination_calls = [match.start() for match in re.finditer(
        r"esp_cache_msync\(destination", adapter)]
    require(len(destination_calls) == 2,
            "adapter must retain exactly one pre-DMA and one post-DMA destination sync")
    pre_destination_sync = adapter[destination_calls[0]:destination_calls[1]]
    post_destination_sync = adapter[destination_calls[1]:]
    post_destination_sync = post_destination_sync[:post_destination_sync.index(") != ESP_OK")]
    require("ESP_CACHE_MSYNC_FLAG_DIR_C2M" in pre_destination_sync and
            "ESP_CACHE_MSYNC_FLAG_INVALIDATE" in pre_destination_sync and
            "ESP_CACHE_MSYNC_FLAG_UNALIGNED" in pre_destination_sync,
            "pre-DMA destination cache policy changed")
    require("ESP_CACHE_MSYNC_FLAG_DIR_M2C" in post_destination_sync and
            "ESP_CACHE_MSYNC_FLAG_UNALIGNED" not in post_destination_sync and
            "kDestinationSpanBytes" in post_destination_sync,
            "post-DMA destination M2C must be aligned and retain its span")
    require("esp_cache_msync(adapter->rx_descriptor, kDescriptorStorageBytes,\n                        ESP_CACHE_MSYNC_FLAG_DIR_M2C)" in adapter,
            "descriptor M2C validation sync missing")
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
    require("if (state != State::Idle && state != State::Failed)" in adapter,
            "quiescent descriptor/cache failures must remain cleanable")
    require("if (observed == State::RetainedAmbiguous)" in adapter and
            "if (!activate_from_queue(adapter))" in adapter,
            "terminal RetainedAmbiguous transition guard missing")
    timeout_start = adapter.index("if (wait_result != pdTRUE)")
    timeout_end = adapter.index("if (adapter->state.load", timeout_start)
    timeout_path = adapter[timeout_start:timeout_end]
    require("terminalize_ambiguous(adapter)" in timeout_path and
            "ESP_ERR_TIMEOUT" in timeout_path and
            "dma2d_force_end" not in timeout_path,
            "timeout must retain the ambiguous adapter without cancellation")
    setup_failure = adapter.index(
        "if (result != ESP_OK)",
        adapter.index("dma2d_job_picked_callback"))
    setup_failure_end = adapter.index("return false;", setup_failure)
    setup_path = adapter[setup_failure:setup_failure_end]
    require("terminalize_ambiguous(adapter)" in setup_path and
            ("signal_completion(adapter, result)" in setup_path or
             "signal_timed_completion(adapter, result" in setup_path),
            "job-pick setup/start failure must retain and wake the task")

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
        "run_pipeline", "pipeline_failure", "FailureStage::DmaEven",
        "FailureStage::DmaOdd", "reason=retained_reentry",
        "dma_timeout_retained", "dma_setup_failure_retained",
        "reason=ambiguous_dma_ownership", "reason=input",
        "reason=allocation", "reason=ppa_register", "reason=cleanup_retained",
        "result=PASS state=Idle", "retain_resources",
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
    require("if (s_retained_resources.adapter != nullptr)" in proto,
            "retained resource set must never be overwritten")

    pipeline_start = proto.index("PipelineResult run_pipeline(")
    pipeline_end = proto.index("\n}\n\nconst char *failure_stage_name", pipeline_start)
    pipeline_body = proto[pipeline_start:pipeline_end]
    require("return pipeline_failure(FailureStage::Ppa" in pipeline_body and
            "return pipeline_failure(FailureStage::Horizontal" in pipeline_body,
            "PPA/horizontal failures must return from the whole pipeline")
    require(re.search(
        r"const esp_err_t even_result = dma2d::copy_strided\(.*?\);\s*"
        r"if \(even_result != ESP_OK\) \{\s*"
        r"return pipeline_failure\(FailureStage::DmaEven",
        pipeline_body, re.S),
            "EVEN DMA failure must return before any later pipeline work")
    require(re.search(
        r"const esp_err_t odd_result = dma2d::copy_strided\(.*?\);\s*"
        r"if \(odd_result != ESP_OK\) \{\s*"
        r"return pipeline_failure\(FailureStage::DmaOdd",
        pipeline_body, re.S),
            "ODD DMA failure must return before any later pipeline work")
    require("break;" not in pipeline_body,
            "fail-stop pipeline must not rely on an inner-loop break")
    require(pipeline_body.count("prepare_tile(") == 1 and
            pipeline_body.count("exact2x_pie_horizontal64_aligned(") == 1 and
            pipeline_body.count("dma2d::copy_strided(") == 2,
            "normal pipeline operation structure changed")
    source_gate = proto.index("PipelineResult pipeline = source_crc_before ==")
    require(proto.index("const std::uint32_t source_crc_before") < source_gate and
            "pipeline_failure(FailureStage::Input" in proto[source_gate:source_gate + 320],
            "source prerequisite failure must stop before PPA/PIE/DMA work")
    reentry_guard = proto.index("if (lifetime_must_be_retained())")
    first_alloc = proto.index("heap_caps_aligned_alloc")
    require(reentry_guard < first_alloc,
            "retained-resource re-entry guard must precede allocation")
    retain_guard = proto.index("if (retain) {")
    destination_sync = proto.index("esp_cache_msync(destination", retain_guard)
    destination_memcmp = proto.index("std::memcmp(destination, reference", destination_sync)
    retain_return = proto.index("return ESP_FAIL;", retain_guard)
    retain_block = proto[retain_guard:retain_return]
    require(retain_guard < destination_sync < destination_memcmp and
            "retain_resources(adapter, tile, staging, destination)" in retain_block and
            "esp_cache_msync" not in retain_block and
            "std::memcmp" not in retain_block and
            "heap_caps_free" not in retain_block,
            "ambiguous ownership must bypass destination validation and free")

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
