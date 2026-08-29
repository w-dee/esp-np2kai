#ifndef NP2_AUDIO86_FIXTURE_H
#define NP2_AUDIO86_FIXTURE_H

#include <stdint.h>

#include "np2_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP2_AUDIO86_RATE_HZ 48000U
#define NP2_AUDIO86_QUANTUM_FRAMES 240U
#define NP2_AUDIO86_DURATION_FRAMES 2880000U
#define NP2_AUDIO86_QUANTA 12000U
#define NP2_AUDIO86_CHANNELS 2U
#define NP2_AUDIO86_PCM_BYTES \
    (NP2_AUDIO86_DURATION_FRAMES * NP2_AUDIO86_CHANNELS * 2U)
#define NP2_AUDIO86_PCM86_SOURCE_RATE_HZ 44100U
#define NP2_AUDIO86_PCM86_SOURCE_CHANNELS 2U
#define NP2_AUDIO86_PCM86_SOURCE_BITS 16U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES 8192U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES \
    (NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES * 4U)

_Static_assert(NP2_AUDIO86_DURATION_FRAMES ==
                   NP2_AUDIO86_RATE_HZ * 60U,
               "86H.2 duration geometry");
_Static_assert(NP2_AUDIO86_DURATION_FRAMES % NP2_AUDIO86_QUANTUM_FRAMES == 0U,
               "86H.2 quantum geometry");
_Static_assert(NP2_AUDIO86_QUANTA * NP2_AUDIO86_QUANTUM_FRAMES ==
                   NP2_AUDIO86_DURATION_FRAMES,
               "86H.2 quantum count");
_Static_assert(NP2_AUDIO86_PCM_BYTES == 11520000U,
               "86H.2 byte geometry");

struct np2audio86_fixture_result {
    uint64_t frames;
    uint64_t bytes;
    uint64_t quanta;
    uint32_t pcm_crc32;
    uint8_t pcm_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t control_crc32;
    uint8_t control_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t source_crc32;
    uint8_t source_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t control_events;
    uint32_t mid_quantum_events;
    uint64_t pcm86_bytes_supplied;
    uint64_t pcm86_bytes_consumed;
    uint32_t pcm86_refills;
    int32_t pcm86_fifo_min;
    int32_t pcm86_fifo_max;
    uint64_t mix_peak_abs;
    uint64_t clamped_samples;
    uint8_t fm_contribution;
    uint8_t psg_contribution;
    uint8_t rhythm_contribution;
    uint8_t pcm86_contribution;
    uint8_t pcm86_fifo_underrun;
    uint8_t sequence_error;
    uint8_t arithmetic_error;
};

/* Render the complete synchronous native reference into compact identities. */
int np2audio86_fixture_render(struct np2audio86_fixture_result *result);

/* Compare a result with the frozen compact golden constants. */
int np2audio86_fixture_matches_golden(
    const struct np2audio86_fixture_result *result);

/* Emit the stable machine-readable summary markers. */
void np2audio86_fixture_print_result(
    const struct np2audio86_fixture_result *result);

#ifdef __cplusplus
}
#endif

#endif /* NP2_AUDIO86_FIXTURE_H */
