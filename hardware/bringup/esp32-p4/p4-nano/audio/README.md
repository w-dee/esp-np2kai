# ESP32-P4-NANO speaker audio diagnostic

This is an independent, speaker-output-only bring-up diagnostic for the
Waveshare ESP32-P4-NANO. It does not modify or link against production
firmware, and it does not use Wi-Fi, BLE, SD/FATFS, a filesystem, an audio
decoder, or the microphone path.

The attached speaker is the small speaker supplied with the
Waveshare ESP32-P4-NANO-KIT-D. It is treated as the intended kit speaker for
the board connector; its impedance and power rating were not independently
measured here.

## Fixed software and hardware boundary

```text
ESP-IDF:              v5.5.4
ES8311 component:     espressif/es8311 1.0.0~1 (exact)
I2C controller:       0
I2C SDA/SCL:          GPIO7 / GPIO8
ES8311 address:       0x18
I2S controller:       0
I2S MCLK/BCLK/WS:     GPIO13 / GPIO12 / GPIO10
I2S DOUT:             GPIO9
I2S DIN:              GPIO_NUM_NC (microphone path unused)
PA enable:            GPIO53, active high
I2S format:           Philips, 48 kHz, signed 16-bit, stereo
MCLK:                 12.288 MHz (256 * 48 kHz)
Codec volume request: 55
PCM peak:             4096 (approximately -18 dBFS)
```

`espressif/es8311` is deprecated upstream in favor of `esp_codec_dev`. This
diagnostic intentionally retains the small component because the first
speaker-only bring-up needs a narrow, controlled I2C codec boundary without
introducing the Waveshare BSP, microphone/echo support, or a larger audio
framework.

The codec-to-P4 ASDOUT input on GPIO11 is not initialized. The diagnostic
only configures the DAC output path.

## Electrical safety

The board's NS4150B output is differential/BTL. The speaker must remain
connected across the board's two speaker-output terminals. Do not connect
either terminal to GND, and do not use a ground-referenced oscilloscope probe
on either terminal.

GPIO53 is configured LOW as the first audio-controlled action. The sequence
keeps the PA disabled while I2C, ES8311, codec mute/volume, I2S, and silence
priming are completed. It waits at least 150 ms after PA enable before
unmuting. Every error path and the normal final state mute the codec and
leave GPIO53 LOW.

## Deterministic stimulus

The diagnostic generates PCM in code from one exact 48-sample, 1 kHz sine
period. Left and right samples are identical. No embedded recording or
decoder is used.

```text
1 second tone
1 second silence
1 second tone
1 second silence
1 second tone
1 second silence
```

Each tone interval is 48,000 stereo frames / 192,000 bytes. The three tone
intervals total 144,000 frames / 576,000 bytes. A CRC32 over the canonical
one-second tone payload is printed as a reproducibility marker. It is not an
acoustic measurement.

The ES8311 component volume API uses a 0..100 parameter, not dB. The first
physical test requests exactly `55`; the driver-returned value is logged.
The diagnostic never increases the volume automatically.

## Measured on real hardware

The first speaker-output bring-up was **MEASURED ON REAL HARDWARE** on the
Waveshare ESP32-P4-NANO with the small speaker supplied with the
ESP32-P4-NANO-KIT-D. The speaker's exact impedance and power rating were not
independently measured.

```text
Board:                 Waveshare ESP32-P4-NANO
ESP32-P4 revision:     v1.3
ESP-IDF:               v5.5.4
ES8311:                address 0x18, espressif/es8311 1.0.0~1
I2S:                   48 kHz, Philips, signed 16-bit stereo
MCLK:                  12.288 MHz
Generated tone:        1 kHz
PA:                    NS4150B, GPIO53 active-high, final state LOW
```

The captured digital result was:

```text
P4-NANO AUDIO ES8311 DETECT: PASS address=0x18
P4-NANO AUDIO CODEC INIT: PASS
P4-NANO AUDIO CODEC VOLUME: PASS requested=55 actual=55
P4-NANO AUDIO I2S INIT: PASS
P4-NANO AUDIO SILENCE PRIME: PASS
P4-NANO AUDIO PA ENABLE: PASS
P4-NANO AUDIO TONE #1: PASS frames=48000 bytes=192000
P4-NANO AUDIO TONE #2: PASS frames=48000 bytes=192000
P4-NANO AUDIO TONE #3: PASS frames=48000 bytes=192000
P4-NANO AUDIO PCM CRC32: 0x025d0870
P4-NANO AUDIO PCM WRITE: PASS
P4-NANO AUDIO PA DISABLE: PASS
P4-NANO AUDIO DIGITAL RESULT: PASS
```

Human observation separately confirmed:

```text
P4-NANO AUDIO SPEAKER AUDIBLE: PASS
P4-NANO AUDIO THREE-TONE CADENCE: PASS
P4-NANO AUDIO FINAL SILENCE: PASS
P4-NANO AUDIO PHYSICAL RESULT: PASS
```

The supplied speaker produced three clearly audible tone bursts with the
specified tone/silence cadence. Resetting the P4 reproduced the same sequence.
An external audio spectrum analyzer observed the generated tone at nominally
1.000 kHz. This is not a calibrated frequency, THD, SPL, or output-power
measurement.

## UART markers and acceptance boundary

Layer A/B firmware markers include:

```text
P4-NANO AUDIO PA SAFE-OFF: PASS
P4-NANO AUDIO I2C INIT: PASS
P4-NANO AUDIO ES8311 DETECT: PASS address=0x18
P4-NANO AUDIO CODEC INIT: PASS
P4-NANO AUDIO CODEC VOLUME: PASS requested=55 actual=...
P4-NANO AUDIO CODEC MUTE: PASS
P4-NANO AUDIO I2S INIT: PASS
P4-NANO AUDIO SILENCE PRIME: PASS
P4-NANO AUDIO PA ENABLE: PASS
P4-NANO AUDIO TONE #1: PASS frames=48000 bytes=192000
P4-NANO AUDIO TONE #2: PASS frames=48000 bytes=192000
P4-NANO AUDIO TONE #3: PASS frames=48000 bytes=192000
P4-NANO AUDIO PCM CRC32: ...
P4-NANO AUDIO PCM WRITE: PASS
P4-NANO AUDIO PCM COUNTS: tone_frames=144000 tone_bytes=576000 silence_frames=...
P4-NANO AUDIO PA DISABLE: PASS
P4-NANO AUDIO DIGITAL RESULT: PASS
```

`PA ENABLE: PASS` means only that the software control action succeeded; it
does not prove analog amplifier operation. Firmware never prints an acoustic
or physical PASS. Human confirmation must separately assess audibility, the
three-beep cadence, gross distortion, unexpected noise, and final silence.

## Build

Use ESP-IDF v5.5.4 from `~/.espressif`:

```bash
source tools/emu/activate-idf.sh
cd hardware/bringup/esp32-p4/p4-nano/audio
idf.py set-target esp32p4
idf.py build
```

The first hardware run is intentionally one sequence only. Do not flash the
C6, erase the full Flash, use `--force`, change eFuses, raise volume, or retry
automatically. Microphone input, the ADC path, recording, echo, stereo
acoustic separation, frequency sweeps, output power, SPL, THD, WAV/music
playback, production audio integration, and simultaneous audio with other
heavy peripherals remain untested.
