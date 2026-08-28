#!/usr/bin/env python3
"""Static contracts for the isolated A3.2 ES8311/I2S tone profile."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/p4_nano_audio_output"
SOURCE = (COMPONENT / "p4_nano_audio_output.cpp").read_text(encoding="utf-8")
HEADER = (COMPONENT / "include/p4_nano_audio_output/tone_model.hpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "firmware/main/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "firmware/main/CMakeLists.txt").read_text(encoding="utf-8")
BUILD = (ROOT / "tools/emu/build-production.sh").read_text(encoding="utf-8")

for token in (
    "I2S_NUM_0",
    "GPIO_NUM_13",
    "GPIO_NUM_12",
    "GPIO_NUM_10",
    "GPIO_NUM_9",
    "GPIO_NUM_11",
    "I2S_CLK_SRC_APLL",
    "I2S_MCLK_MULTIPLE_256",
    "I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG",
    "i2s_channel_preload_data",
    "i2s_channel_write",
    "dma_desc_num = kDmaDescriptorCount",
    "dma_frame_num = kBlockFrames",
    "kSampleRateHz = 48000U",
    "kToneFrequencyHz = 1000U",
    "kSamplesPerCycle = 48U",
    "kExpectedFrames =",
    "kBlockFrames = 240U",
    "kDmaDescriptorCount = 4U",
    "kCodecAddress = 0x18U",
    "kS16Resolution = 0x0cU",
    "kRegClock6, 0x03U, 0x1fU",
    "pa_service_init",
    "pa_service_enable",
    "pa_service_disable",
    "pa_service_shutdown",
    "shared_i2c_acquire_device",
    "shared_i2c_release_device",
    "P4_AUDIO_I2S_TONE_RESULT=",
):
    assert token in SOURCE or token in HEADER or token in MAIN, token

assert '#include "driver/i2s_std.h"' in SOURCE
assert "I2S_NUM_AUTO" not in SOURCE
assert "driver/i2s.h" not in SOURCE
assert "i2s_driver_install" not in SOURCE
assert "i2s_new_channel" in SOURCE
assert "i2s_channel_register_event_callback" not in SOURCE
assert "i2c_new_master_bus" not in SOURCE
assert "i2c_del_master_bus" not in SOURCE
assert "gpio_config" not in SOURCE
assert SOURCE.index("pa_service_init") < SOURCE.index("shared_i2c_acquire_device")
assert SOURCE.index("pa_service_enable") < SOURCE.index("codec_mute(codec_lease, false)")
assert SOURCE.index("pa_service_disable") < SOURCE.index("codec_mute(codec_lease, true)")
assert SOURCE.index("i2s_channel_disable") < SOURCE.index("i2s_del_channel")
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE" in MAIN
assert "p4_nano_audio_output" in CMAKE
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE_ACTIVE" in CMAKE
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE=1" in CMAKE
assert "--audio-i2s-tone" in BUILD
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE=${audio_i2s_tone}" in BUILD
assert "--audio-i2s-tone requires --variant p4-v1x --board p4-nano" in BUILD

print("P4_AUDIO_I2S_TONE_STATIC_CONTRACT=PASS")
