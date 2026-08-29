# P4 Audio A3.4 I2S sink contract

The `P4_NANO_AUDIO_I2S_OPNGEN_PROFILE` routes the existing A2 producer and
OPNGEN worker through the existing eight-slot, 240-frame canonical PCM ring.
The ring consumer is a task on core 0 at priority 5.  It submits one complete
960-byte S16 stereo slot with the ESP-IDF v5.5.4 `i2s_channel_write()` API and
does not release the slot until the call returns `ESP_OK` with
`bytes_written == 960`.

## Source-buffer lifetime

The v5.5.4 implementation in
`components/esp_driver_i2s/i2s_common.c` copies the caller buffer into the
selected DMA descriptor with `memcpy()` inside `i2s_channel_write()`.  The
function reports the number of bytes copied through `bytes_written`; the
destination is DMA-owned storage, and a full successful call therefore ends
the caller-buffer lifetime for that transfer.
The consumer updates the submitted identity and calls
`np2opngen_pcm_ring_consume()` only after that complete return.  A timeout,
driver error, or short copy is fail-closed and leaves the slot owned by the
ring.

## Clock and DMA authority

I2S0 is configured as an APLL-driven 48 kHz Philips S16 stereo master.  The
channel uses four DMA descriptors of 240 frames (960 bytes each, 3840 bytes in
the DMA pipeline).  No `esp_timer` callback or 5 ms timer decides when a slot
is consumed; the blocking write and the I2S sample clock provide pacing.

## Startup and final drain

PA is initialized LOW, the shared I2C ES8311 lease and I2S channel are
configured, and the producer/worker prefill at least four ring slots before
I2S is enabled.  PA is then raised, the proven 150 ms settle interval is
observed, and the codec is unmuted only after a mute-bit readback.

When the producer has published the final ring slot, the consumer drains all
remaining slots.  An empty ring is treated as normal completion only when the
producer has published its final frame; otherwise it is an underrun and a
sticky failure.  After the final slot is written and the ring is empty, the
consumer waits 20 ms (the bounded drain for the four-descriptor, 48 kHz
pipeline) before reporting completion.  Task users are then quiesced, PA is
returned LOW, the codec is muted, I2S is disabled/deleted, and the I2C lease is
released.
