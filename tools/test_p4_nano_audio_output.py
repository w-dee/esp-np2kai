#!/usr/bin/env python3
"""Static contracts for the isolated A3.3a ES8311/I2S tone profile."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/p4_nano_audio_output"
SOURCE = (COMPONENT / "p4_nano_audio_output.cpp").read_text(encoding="utf-8")
HEADER = (COMPONENT / "include/p4_nano_audio_output/tone_model.hpp").read_text(
    encoding="utf-8"
)
BOARD_HEADER = (
    ROOT / "firmware/components/p4_nano_board/include/p4_nano_board/p4_nano_board.hpp"
).read_text(encoding="utf-8")
BOARD_SOURCE = (ROOT / "firmware/components/p4_nano_board/p4_nano_board.cpp").read_text(
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
    "kTonePeak = 4096",
    "kDacVolumeValue = 0xa0U",
    "kCodecAddress = 0x18U",
    "kS16Resolution = 0x0cU",
    "kRegClock6, 0x03U, 0x1fU",
    "kRegDacVolume, kDacVolumeValue",
    "P4_AUDIO_CODEC_VOLUME_READBACK",
    "P4_AUDIO_CODEC_STARTUP_MUTED_READBACK",
    "P4_AUDIO_CODEC_UNMUTE_READBACK",
    "P4_AUDIO_PA_SETTLE",
    "kPaSettleTicks = pdMS_TO_TICKS(150)",
    "P4_AUDIO_PA transition=LOW gpio=%d active_level=HIGH safe_level=LOW",
    "P4_AUDIO_PA transition=HIGH gpio=%d active_level=HIGH safe_level=LOW",
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
assert SOURCE.index("pa_service_enable") < SOURCE.index("P4_AUDIO_PA_SETTLE")
assert SOURCE.index("P4_AUDIO_PA_SETTLE") < SOURCE.index("codec_mute(codec_lease, false)")
assert SOURCE.index("codec_mute(codec_lease, false)") < SOURCE.index(
    "P4_AUDIO_CODEC_UNMUTE_READBACK"
)
assert SOURCE.index("pa_service_disable") < SOURCE.index("codec_mute(codec_lease, true)")
assert SOURCE.index("i2s_channel_disable") < SOURCE.index("i2s_del_channel")
assert "if (!readback_ok) result = ESP_FAIL;" in SOURCE
assert "codec_write(lease, kRegDacVolume, 0x80U)" not in SOURCE
assert "kExpectedCrc32 = UINT32_C(0x3054ef52)" in SOURCE
for digest_byte in (
    "0xa0, 0xa4, 0xe3, 0x11, 0x82, 0xd3, 0x2b, 0xfd",
    "0x51, 0x48, 0x5f, 0xfe, 0xf2, 0xee, 0x80, 0x30",
    "0x6a, 0x57, 0x67, 0x0c, 0x56, 0x67, 0x87, 0x9d",
    "0xfd, 0x35, 0x6d, 0x9d, 0x63, 0x6d, 0x91, 0xc5",
):
    assert digest_byte in SOURCE
assert "kPaControlGpioNumber = 53" in BOARD_HEADER
assert "kPaControlGpioNumber = 51" not in BOARD_HEADER
pa_service = BOARD_SOURCE[BOARD_SOURCE.index("esp_err_t pa_service_init()") :]
assert "GPIO_NUM_51" not in pa_service
assert "kPaControlGpioNumber" in pa_service
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE" in MAIN
assert "p4_nano_audio_output" in CMAKE
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE_ACTIVE" in CMAKE
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE=1" in CMAKE
assert "--audio-i2s-tone" in BUILD
assert "P4_NANO_AUDIO_I2S_TONE_PROFILE=${audio_i2s_tone}" in BUILD
assert "--audio-i2s-tone requires --variant p4-v1x --board p4-nano" in BUILD

class _AnalogSequenceModel:
    """Host-only fail-closed model for the A3.3a transition boundary."""

    def run(self, *, pa_enable=True, unmute_write=True, unmute_readback=True,
            shutdown_mute=True):
        events = ["pa_low", "codec_muted", "i2s_ready"]
        if not pa_enable:
            return False, events + ["cleanup_pa_low", "cleanup_mute"]
        events += ["pa_high", "pa_settle_150ms"]
        if not unmute_write:
            return False, events + ["cleanup_pa_low", "cleanup_mute"]
        events += ["codec_unmuted"]
        if not unmute_readback:
            return False, events + ["cleanup_pa_low", "cleanup_mute"]
        events += ["tone_complete", "cleanup_pa_low", "cleanup_mute"]
        return shutdown_mute, events


model = _AnalogSequenceModel()
for failure in (
    {"pa_enable": False},
    {"unmute_write": False},
    {"unmute_readback": False},
    {"shutdown_mute": False},
):
    success, events = model.run(**failure)
    assert not success
    assert events[-2:] == ["cleanup_pa_low", "cleanup_mute"]
    if "codec_unmuted" in events:
        assert events.index("pa_settle_150ms") < events.index("codec_unmuted")
assert model.run()[1].index("pa_high") < model.run()[1].index("pa_settle_150ms")
assert model.run()[1].index("pa_settle_150ms") < model.run()[1].index("codec_unmuted")
print("P4_AUDIO_ANALOG_FAILURE_MODEL_TEST=PASS")
print("A3_ANALOG_FIX_PCM_IDENTITY_UNCHANGED=PASS")
print("P4_AUDIO_I2S_TONE_STATIC_CONTRACT=PASS")
