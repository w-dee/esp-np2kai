#include "np2opngen_pcm_canonical.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint16_t canonical_s16(SINT32 value, bool *clipped)
{
    if (value > 32767) {
        *clipped = true;
        return UINT16_C(32767);
    }
    if (value < -32768) {
        *clipped = true;
        return UINT16_C(0x8000);
    }
    *clipped = false;
    return (uint16_t)(int16_t)value;
}

int np2opngen_pcm_canonicalize_s16le(
    const SINT32 *pcm, size_t frame_count, uint16_t channels,
    uint8_t *canonical_pcm, size_t canonical_bytes,
    struct np2opngen_pcm_stats *stats)
{
    size_t sample_count;
    size_t expected_bytes;
    size_t frame;

    if ((pcm == 0 && frame_count != 0U) || channels == 0U ||
        canonical_pcm == 0 || stats == 0 ||
        frame_count > SIZE_MAX / (size_t)channels) {
        return -1;
    }
    sample_count = frame_count * (size_t)channels;
    if (sample_count > SIZE_MAX / 2U) {
        return -1;
    }
    expected_bytes = sample_count * 2U;
    if (canonical_bytes < expected_bytes) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    for (frame = 0U; frame < frame_count; ++frame) {
        uint16_t channel;
        for (channel = 0U; channel < channels; ++channel) {
            const size_t sample_index = frame * (size_t)channels + channel;
            const SINT32 value = pcm[sample_index];
            const int64_t widened = (int64_t)value;
            const uint64_t magnitude = widened < 0 ? (uint64_t)(-widened)
                                                   : (uint64_t)widened;
            bool clipped;
            const int16_t sample =
                (int16_t)canonical_s16(value, &clipped);
            const uint16_t encoded = (uint16_t)sample;
            const size_t byte_offset = sample_index * 2U;

            if (magnitude > stats->s32_abs_peak) {
                stats->s32_abs_peak = magnitude;
            }
            if (clipped) {
                ++stats->clip_samples;
            }
            if (sample != 0) {
                ++stats->nonzero_s16_samples;
            }
            if (channel == 0U) {
                stats->l_sumsq += (uint64_t)((int64_t)sample * sample);
            } else if (channel == 1U) {
                stats->r_sumsq += (uint64_t)((int64_t)sample * sample);
            }
            canonical_pcm[byte_offset] = (uint8_t)(encoded & 0xffU);
            canonical_pcm[byte_offset + 1U] =
                (uint8_t)((encoded >> 8) & 0xffU);
        }
    }
    return 0;
}
