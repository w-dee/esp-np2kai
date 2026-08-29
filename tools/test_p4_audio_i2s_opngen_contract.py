#!/usr/bin/env python3
"""Static contracts for the A3.4 real-I2S OPNGEN integration profile."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "firmware/components/p4_nano_audio_i2s_opngen/"
          "p4_nano_audio_i2s_opngen.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "firmware/components/p4_nano_audio_i2s_opngen/include/"
          "p4_nano_audio_i2s_opngen/p4_nano_audio_i2s_opngen.hpp").read_text(
              encoding="utf-8")
COMPONENT = (ROOT / "firmware/components/p4_nano_audio_i2s_opngen/CMakeLists.txt"
             ).read_text(encoding="utf-8")
MAIN = (ROOT / "firmware/main/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text(encoding="utf-8")
SDKCONFIG = (ROOT / "firmware/sdkconfig").read_text(encoding="utf-8")
BUILD = (ROOT / "tools/emu/build-production.sh").read_text(encoding="utf-8")
DOC = (ROOT / "docs/development/p4-audio-a3.4-i2s-sink.md").read_text(
    encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    for token in (
        "i2s_channel_write",
        "NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES",
        "NP2_OPNGEN_PCM_RING_SLOT_BYTES",
        "constexpr uint32_t kPrefillTarget = 4U",
        "constexpr uint32_t kDmaDescriptorCount = 4U",
        "chan_cfg.dma_desc_num = kDmaDescriptorCount",
        "chan_cfg.dma_frame_num = kQuantumFrames",
        "I2S_NUM_0",
        "I2S_CLK_SRC_APLL",
        "I2S_MCLK_MULTIPLE_256",
        "I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG",
        "GPIO_NUM_13",
        "GPIO_NUM_12",
        "GPIO_NUM_10",
        "GPIO_NUM_9",
        "GPIO_NUM_11",
        "kCodecAddress = 0x18U",
        "kDacVolume = 0xa0U",
        "pa_service_init",
        "shared_i2c_acquire_device",
        "P4_AUDIO_I2S_OPNGEN_PREFILL",
        "P4_AUDIO_I2S_OPNGEN_CODEC_UNMUTE_READBACK",
        "P4_AUDIO_I2S_OPNGEN_RESULT=",
        "P4_AUDIO_I2S_OPNGEN_SHUTDOWN",
        "i2s_consumer_underrun_count",
        "full_wait_count",
        "occupancy_hist",
        "i2s_wait_separate=YES",
    ):
        require(token in SOURCE, f"missing integrated contract token {token}")
    require("P4_NANO_AUDIO_I2S_OPNGEN_PROFILE" in MAIN and
            "P4_NANO_AUDIO_I2S_OPNGEN_PROFILE" in CMAKE,
            "missing integrated profile macro routing")

    require("esp_timer" not in SOURCE,
            "integrated I2S consumer must not import esp_timer pacing")
    require("FMGEN" not in SOURCE,
            "integrated profile must use the accepted OPNGEN worker boundary")
    require("i2s_channel_register_event_callback" not in SOURCE,
            "integrated profile must not use a user I2S ISR callback")
    require("p4_nano_display" not in SOURCE and "display" not in COMPONENT.lower(),
            "integrated profile must not depend on display components")

    run_workload_at = SOURCE.index("static bool run_workload")
    run_workload = SOURCE[run_workload_at:SOURCE.index("\n} // namespace", run_workload_at)]
    require("Context ctx{};" not in run_workload,
            "run_workload must not place Context on the task stack")
    require("Context *ctx" in run_workload and
            "heap_caps_calloc(1U, context_bytes, context_caps)" in run_workload,
            "Context must use one explicit zeroed heap allocation")
    require("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in run_workload,
            "Context allocation must require INTERNAL and 8-bit capabilities")
    for token in (
        "heap_caps_get_free_size",
        "heap_caps_get_largest_free_block",
        "internal_free_before",
        "internal_largest_before",
        "internal_free_after",
        "internal_largest_after",
        "context_bytes",
    ):
        require(token in run_workload, f"missing Context heap evidence token {token}")
    allocation_failure_at = run_workload.index("if (ctx == nullptr)")
    hardware_init_at = run_workload.index("configure_i2s_and_codec")
    task_creation_at = run_workload.index("xTaskCreatePinnedToCore")
    require(allocation_failure_at < hardware_init_at,
            "Context allocation failure must precede hardware initialization")
    require(allocation_failure_at < task_creation_at,
            "Context allocation failure must precede task creation")
    require("successful = true" not in run_workload[allocation_failure_at:task_creation_at],
            "Context allocation failure must not emit success semantics")
    require(run_workload.count("heap_caps_free(ctx);") == 1 and
            run_workload.index("ctx->~Context();") < run_workload.index("heap_caps_free(ctx);"),
            "Context must be destroyed and freed exactly once at cleanup tail")
    require("static_assert(sizeof(Context) == 9112U" in SOURCE,
            "Context size evidence must remain machine-visible")
    require("xTaskCreatePinnedToCore(worker_task, \"p4_i2s_worker\", 8192, ctx" in run_workload and
            "xTaskCreatePinnedToCore(producer_task, \"p4_i2s_producer\", 8192, ctx" in run_workload and
            "xTaskCreatePinnedToCore(consumer_task, \"p4_i2s_consumer\", 8192, ctx" in run_workload,
            "all tasks must receive the stable heap Context pointer")
    cleanup = run_workload[run_workload.index("cleanup:"):]
    require(cleanup.index("vTaskDelete") < cleanup.index("np2opngen_e1b_worker_destroy") <
            cleanup.index("heap_caps_free(ctx->metrics.latency_ticks)") <
            cleanup.index("ctx->~Context()") < cleanup.index("heap_caps_free(ctx);"),
            "Context lifetime cleanup order is not fail-safe")
    require("CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584" in SDKCONFIG,
            "main-task stack configuration unexpectedly changed")
    write_at = SOURCE.index("i2s_channel_write(")
    consume_at = SOURCE.index("np2opngen_pcm_ring_consume", write_at)
    require(consume_at > write_at,
            "ring slot must be consumed only after i2s_channel_write")
    write_region = SOURCE[write_at:consume_at]
    require("written != kQuantumBytes" in write_region and "fail(ctx)" in write_region,
            "short I2S writes must fail closed before ring consume")
    require("submitted_update(ctx, slot->pcm, written);" in write_region,
            "submitted identity must be updated at the write boundary")
    require("xSemaphoreGive(ctx->pcm_space);" in SOURCE[consume_at:],
            "ring space must be returned only after consume")

    # Startup ordering is intentionally narrow and machine-checkable.
    require(SOURCE.index("P4_AUDIO_I2S_OPNGEN_PREFILL") <
            SOURCE.index("i2s_channel_enable(ctx->tx)"),
            "I2S cannot be enabled before ring prefill")
    require(SOURCE.index("i2s_channel_enable(ctx->tx)") <
            SOURCE.index("pa_service_enable()"),
            "PA must remain LOW until I2S is enabled")
    require(SOURCE.index("P4_AUDIO_I2S_OPNGEN_PA_SETTLE") <
            SOURCE.index("codec_mute(ctx->codec, false)"),
            "codec unmute must follow the 150 ms PA settle")
    cleanup = SOURCE[SOURCE.index("cleanup:"):]
    require(cleanup.index("pa_service_disable") < cleanup.index("codec_mute(ctx->codec, true)"),
            "shutdown must lower PA before codec mute")
    require(cleanup.index("codec_mute(ctx->codec, true)") <
            cleanup.index("i2s_channel_disable"),
            "shutdown must mute codec before disabling I2S")

    for token in (
        "P4_NANO_AUDIO_I2S_OPNGEN_PROFILE_ACTIVE",
        "p4_nano_audio_i2s_opngen",
        "P4_NANO_AUDIO_I2S_OPNGEN_PROFILE=1",
    ):
        require(token in CMAKE or token in MAIN, f"missing profile routing token {token}")
    for token in (
        "--audio-i2s-opngen",
        "audio_i2s_opngen=0",
        "audio_i2s_opngen=1",
        "P4_NANO_AUDIO_I2S_OPNGEN_PROFILE=${audio_i2s_opngen}",
    ):
        require(token in BUILD, f"missing build routing token {token}")
    require("--audio-i2s-tone" in BUILD and "P4_NANO_AUDIO_I2S_TONE_PROFILE" in MAIN,
            "isolated tone profile must remain routed separately")

    for token in (
        "memcpy()",
        "bytes_written",
        "DMA-owned",
        "20 ms",
        "No `esp_timer` callback",
    ):
        require(token in DOC, f"missing source-lifetime/drain documentation token {token}")

    print("A3_I2S_HARDWARE_PACING_CONTRACT=PASS")
    print("A3_CONTEXT_HEAP_PLACEMENT_CONTRACT=PASS")
    print("A3_MAIN_STACK_CONFIG_UNCHANGED=PASS")
    print("P4_AUDIO_I2S_OPNGEN_STATIC_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
