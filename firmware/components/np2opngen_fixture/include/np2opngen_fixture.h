#ifndef NP2_OPNGEN_FIXTURE_H
#define NP2_OPNGEN_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*np2opngen_fixture_clock_fn)(void *context);

/* The PCM buffer is valid only for the duration of this callback.  The sink
 * must not retain or free it. */
typedef int (*np2opngen_fixture_pcm_sink_fn)(
    const uint8_t *canonical_pcm, size_t pcm_bytes,
    uint32_t sample_rate_hz, uint16_t channels, uint16_t bits_per_sample,
    void *context);

/* Run the portable E1 direct OPNGEN fixture and print its structured result. */
int np2opngen_fixture_run(np2opngen_fixture_clock_fn clock_fn, void *context);

/* Run the same fixture and optionally expose its validated canonical PCM. */
int np2opngen_fixture_run_with_sink(
    np2opngen_fixture_clock_fn clock_fn, void *clock_context,
    np2opngen_fixture_pcm_sink_fn pcm_sink, void *pcm_sink_context);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_FIXTURE_H */
