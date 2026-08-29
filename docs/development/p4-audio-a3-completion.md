# P4 Audio A3 completion

## Status and scope

P4 Audio A3: **FORMAL PASS**.

The accepted final firmware candidate is commit
`e2e0c2320ab8b29e0762c568a4f3ba0ac7a03882`, built with ESP-IDF v5.5.4 for the
Waveshare ESP32-P4-NANO-KIT-D. This document records the scoped A3 evidence;
temporary capture paths and machine-local identifiers are intentionally not
part of the contract.

## Physical path

The proven path is:

```text
ESP32-P4 -> I2S0/APLL -> ES8311 -> NS4150B -> H4 -> external 8-ohm speaker
```

The power-amplifier control is GPIO53, active HIGH. GPIO51 is **not** PA
control; on the audited board it is associated with the Ethernet PHY reset
path.

## Canonical stream and transport

The A3 real-I2S stream is 48 kHz signed 16-bit little-endian stereo, in
240-frame (nominal 5 ms) quanta. The finite PCM ring has eight slots. The real
I2S sample clock is the pacing authority; A3 real-I2S mode does not use a
virtual `esp_timer` output clock.

### RETRO formal evidence

- 1,047 events, 576,960 frames, and 2,404 blocks.
- Trusted PCM SHA-256:
  `1d4d24ad9c966dea085607afee6a9ecb049c2c476863c534dbfe0e50ace1016b`.
- Generated and submitted PCM identities matched mechanically.
- Partial writes, timeouts, I2S errors, application-ring underruns,
  overruns, drops, sequence errors, and frame-offset errors were all zero.

### STRESS-60 formal evidence

- 41,127 events, 2,880,000 frames, and 12,000 blocks (nominally 60 seconds).
- Trusted PCM SHA-256:
  `4632669886930b31312b96b2534eb7c50dba4c1455eb55f4f061ae19781bb732`.
- Generated and submitted PCM identities matched mechanically.
- Formal application transport, drop, and underrun counters were zero; final
  PCM ring occupancy was zero.

## Physical observation and measured workload

A3.3b produced an audibly observed deterministic tone. During A3.6P, the
reference playback and P4-NANO speaker output were reported as subjectively
consistent, without obvious crackling, buzzing, or unintended sound.
This is subjective human evidence only; it does not establish analog
waveform equality or objective audio quality.

The following are measured A3 workload observations, not universal performance
guarantees:

| Workload | I2S blocking mean | I2S blocking p99 | OPNGEN wall-service mean |
|---|---:|---:|---:|
| RETRO | approximately 4,351 us | approximately 4,359 us | approximately 564 us |
| STRESS-60 | approximately 4,356 us | approximately 4,359 us | approximately 475 us |

`esp_timer_get_time()` measures wall-service duration here; these values are
not exact CPU-utilization measurements. Active-compute outliers are not
capacity claims.

## Hardening retained by A3

- OPNGEN context is heap-owned in INTERNAL | 8BIT memory; the final context
  size is 9,152 bytes.
- The accepted final `run_workload` target frame is 240 bytes.
- The I2S write timeout is explicitly 1,000 ms in its API unit.
- Successful completion requires explicit producer, worker, and consumer
  terminal acknowledgements. `GRACEFUL` completion is distinct from forced
  cleanup.

## Explicit nonclaims

A3 does not prove:

- full PC-9801-86 audio capacity;
- combined YM2608 FM/SSG/rhythm and PCM86 capacity;
- full-runtime mixer, timer, or IRQ capacity;
- display/audio coexistence or DMA/GDMA/PSRAM contention under display load;
- analog bit-exactness, THD+N, analog noise floor, or speaker frequency
  response/fidelity; or
- production or user-facing volume policy.

## Future gates

Separate future work may measure high-load PC-9801-86 audio capacity, audio
plus display coexistence, optional CPU-cycle profiling, and long soaks. Analog
quality measurements are only required if product requirements demand them.
These are not A3 completion conditions.
