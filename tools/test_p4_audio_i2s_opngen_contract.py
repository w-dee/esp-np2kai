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
            SOURCE.index("i2s_channel_enable(ctx.tx)"),
            "I2S cannot be enabled before ring prefill")
    require(SOURCE.index("i2s_channel_enable(ctx.tx)") <
            SOURCE.index("pa_service_enable()"),
            "PA must remain LOW until I2S is enabled")
    require(SOURCE.index("P4_AUDIO_I2S_OPNGEN_PA_SETTLE") <
            SOURCE.index("codec_mute(ctx.codec, false)"),
            "codec unmute must follow the 150 ms PA settle")
    cleanup = SOURCE[SOURCE.index("cleanup:"):]
    require(cleanup.index("pa_service_disable") < cleanup.index("codec_mute(ctx.codec, true)"),
            "shutdown must lower PA before codec mute")
    require(cleanup.index("codec_mute(ctx.codec, true)") <
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
    print("P4_AUDIO_I2S_OPNGEN_STATIC_CONTRACT=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
