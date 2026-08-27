#ifndef NP2_OPNGEN_PCM_CANONICAL_H
#define NP2_OPNGEN_PCM_CANONICAL_H

#include <stddef.h>
#include <stdint.h>

#include <compiler.h>
#include <sound/sound.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Statistics are part of the E1 observability contract.  The converter owns
 * the exact S32-to-S16LE interpretation used by both E1 and E1B. */
struct np2opngen_pcm_stats {
    uint64_t s32_abs_peak;
    uint64_t nonzero_s16_samples;
    uint64_t clip_samples;
    uint64_t l_sumsq;
    uint64_t r_sumsq;
};

/* Convert interleaved signed 32-bit PCM to canonical interleaved S16LE.
 * Values above 32767 clamp to 32767; values below -32768 clamp to -32768.
 * The output and statistics are written only when all arguments are valid. */
int np2opngen_pcm_canonicalize_s16le(
    const SINT32 *pcm, size_t frame_count, uint16_t channels,
    uint8_t *canonical_pcm, size_t canonical_bytes,
    struct np2opngen_pcm_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_PCM_CANONICAL_H */
