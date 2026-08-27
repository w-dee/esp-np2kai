#include "np2opngen_fixture.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compiler.h>
#include <sound/opngen.h>
#include <sound/opngencfg.h>

#include "np2_crc32.h"
#include "np2_sha256.h"

#define FIXTURE_RATE_HZ 48000U
#define FIXTURE_FRAMES 28800U
#define FIXTURE_CHANNELS 2U
#define FIXTURE_PCM_BYTES (FIXTURE_FRAMES * FIXTURE_CHANNELS * 2U)

enum fixture_partition {
    FIXTURE_NORMAL,
    FIXTURE_CHUNK240,
    FIXTURE_CHUNK1,
};

struct fixture_options {
    unsigned frequency_mode;
    bool omit_keyoff;
    bool silence;
};

struct fixture_capture {
    uint8_t *pcm;
    uint32_t sintable_crc32;
    uint32_t envtable_crc32;
    uint32_t envcurve_crc32;
    uint32_t ratebit;
    int32_t calc1024;
    int32_t fmvol;
    uint32_t pcm_crc32;
    uint8_t sha256[NP2_SHA256_DIGEST_SIZE];
    uint64_t s32_abs_peak;
    uint64_t nonzero_s16_samples;
    uint64_t clip_samples;
    uint64_t l_sumsq;
    uint64_t r_sumsq;
    uint64_t l_rms_q16;
    uint64_t r_rms_q16;
    uint64_t init_us;
    uint64_t render_us;
};

static uint64_t fixture_now(np2opngen_fixture_clock_fn clock_fn, void *context)
{
    return clock_fn != 0 ? clock_fn(context) : 0;
}

static uint32_t fixture_crc_words(const SINT32 *words, size_t count)
{
    uint32_t running = np2_crc32_iso_hdlc_init();
    size_t i;
    for (i = 0; i < count; ++i) {
        uint32_t value = (uint32_t)words[i];
        uint8_t bytes[4] = {
            (uint8_t)(value & 0xffU),
            (uint8_t)((value >> 8) & 0xffU),
            (uint8_t)((value >> 16) & 0xffU),
            (uint8_t)((value >> 24) & 0xffU),
        };
        running = np2_crc32_iso_hdlc_update(running, bytes, sizeof(bytes));
    }
    return np2_crc32_iso_hdlc_finish(running);
}

static void fixture_capture_tables(struct fixture_capture *capture)
{
    capture->sintable_crc32 = fixture_crc_words(opncfg.sintable, SIN_ENT);
    capture->envtable_crc32 = fixture_crc_words(opncfg.envtable, EVC_ENT);
    capture->envcurve_crc32 = fixture_crc_words(opncfg.envcurve, EVC_ENT * 2U + 1U);
    capture->ratebit = (uint32_t)opncfg.ratebit;
    capture->calc1024 = (int32_t)opncfg.calc1024;
    capture->fmvol = (int32_t)opncfg.fmvol;
}

static void fixture_setreg(OPNGEN state, const uint8_t *pairs, size_t pair_count)
{
    size_t i;
    for (i = 0; i < pair_count; ++i) {
        opngen_setreg(state, 0, pairs[i * 2U], pairs[i * 2U + 1U]);
    }
}

static void fixture_program_channel0(OPNGEN state)
{
    static const uint8_t pairs[] = {
        0x30, 0x01, 0x34, 0x01, 0x38, 0x02, 0x3c, 0x01,
        0x40, 0x28, 0x44, 0x30, 0x48, 0x20, 0x4c, 0x00,
        0x50, 0x1f, 0x54, 0x1a, 0x58, 0x16, 0x5c, 0x1f,
        0x60, 0x0e, 0x64, 0x0a, 0x68, 0x08, 0x6c, 0x10,
        0x70, 0x06, 0x74, 0x05, 0x78, 0x04, 0x7c, 0x08,
        0x80, 0x1f, 0x84, 0x1f, 0x88, 0x1f, 0x8c, 0x1f,
        0xb0, 0x36, 0xb4, 0xc0, 0xa4, 0x22, 0xa0, 0x69,
    };
    fixture_setreg(state, pairs, sizeof(pairs) / 2U);
}

static void fixture_program_channel1(OPNGEN state)
{
    static const uint8_t pairs[] = {
        0x31, 0x11, 0x35, 0x01, 0x39, 0x12, 0x3d, 0x02,
        0x41, 0x38, 0x45, 0x48, 0x49, 0x20, 0x4d, 0x04,
        0x51, 0x1b, 0x55, 0x17, 0x59, 0x13, 0x5d, 0x1f,
        0x61, 0x0c, 0x65, 0x0a, 0x69, 0x08, 0x6d, 0x10,
        0x71, 0x06, 0x75, 0x05, 0x79, 0x04, 0x7d, 0x08,
        0x81, 0x1f, 0x85, 0x1f, 0x89, 0x1f, 0x8d, 0x1f,
        0xb1, 0x2d, 0xb5, 0x80, 0xa5, 0x24, 0xa1, 0xd0,
    };
    fixture_setreg(state, pairs, sizeof(pairs) / 2U);
}

static int fixture_render(OPNGEN state, SINT32 *pcm, uint32_t cursor,
                          uint32_t frames, enum fixture_partition partition,
                          np2opngen_fixture_clock_fn clock_fn, void *context,
                          bool measure, uint64_t *render_us)
{
    uint32_t remaining = frames;
    uint32_t offset = cursor;
    while (remaining != 0U) {
        uint32_t count = remaining;
        uint64_t start;
        uint64_t end;
        if (partition == FIXTURE_CHUNK240 && count > 240U) {
            count = 240U;
        } else if (partition == FIXTURE_CHUNK1) {
            count = 1U;
        }
        start = measure ? fixture_now(clock_fn, context) : 0;
        opngen_getpcm(state, pcm + (size_t)offset * FIXTURE_CHANNELS, count);
        end = measure ? fixture_now(clock_fn, context) : 0;
        if (measure && end >= start) {
            *render_us += end - start;
        }
        offset += count;
        remaining -= count;
    }
    return offset == cursor + frames ? 0 : -1;
}

static uint64_t fixture_isqrt(uint64_t value)
{
    uint64_t bit = UINT64_C(1) << 62;
    uint64_t result = 0;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static uint16_t fixture_canonical_s16(SINT32 value, bool *clipped)
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

static void fixture_finalize_pcm(struct fixture_capture *capture,
                                 const SINT32 *pcm)
{
    np2_sha256_context sha;
    uint32_t crc = np2_crc32_iso_hdlc_init();
    uint64_t frame;
    np2_sha256_init(&sha);
    for (frame = 0; frame < FIXTURE_FRAMES; ++frame) {
        unsigned channel;
        for (channel = 0; channel < FIXTURE_CHANNELS; ++channel) {
            const SINT32 value = pcm[frame * FIXTURE_CHANNELS + channel];
            const int64_t widened = (int64_t)value;
            const uint64_t magnitude = widened < 0 ? (uint64_t)(-widened)
                                                   : (uint64_t)widened;
            bool clipped;
            const int16_t sample = (int16_t)fixture_canonical_s16(value, &clipped);
            const uint16_t encoded = (uint16_t)sample;
            uint8_t bytes[2] = {
                (uint8_t)(encoded & 0xffU),
                (uint8_t)((encoded >> 8) & 0xffU),
            };
            const size_t byte_offset =
                (frame * FIXTURE_CHANNELS + channel) * 2U;
            if (magnitude > capture->s32_abs_peak) {
                capture->s32_abs_peak = magnitude;
            }
            if (clipped) {
                ++capture->clip_samples;
            }
            if (sample != 0) {
                ++capture->nonzero_s16_samples;
            }
            if (channel == 0U) {
                capture->l_sumsq += (uint64_t)((int64_t)sample * sample);
            } else {
                capture->r_sumsq += (uint64_t)((int64_t)sample * sample);
            }
            capture->pcm[byte_offset] = bytes[0];
            capture->pcm[byte_offset + 1U] = bytes[1];
            crc = np2_crc32_iso_hdlc_update(crc, bytes, sizeof(bytes));
            np2_sha256_update(&sha, bytes, sizeof(bytes));
        }
    }
    capture->pcm_crc32 = np2_crc32_iso_hdlc_finish(crc);
    np2_sha256_final(&sha, capture->sha256);
    capture->l_rms_q16 = fixture_isqrt((capture->l_sumsq / FIXTURE_FRAMES << 32) +
                                       ((capture->l_sumsq % FIXTURE_FRAMES << 32) /
                                        FIXTURE_FRAMES));
    capture->r_rms_q16 = fixture_isqrt((capture->r_sumsq / FIXTURE_FRAMES << 32) +
                                       ((capture->r_sumsq % FIXTURE_FRAMES << 32) /
                                        FIXTURE_FRAMES));
}

static int fixture_run_vector(enum fixture_partition partition,
                              const struct fixture_options *options,
                              np2opngen_fixture_clock_fn clock_fn, void *context,
                              bool measure, struct fixture_capture *capture)
{
    _OPNGEN state;
    SINT32 *pcm;
    uint32_t cursor = 0;
    uint64_t start;
    uint64_t end;
    int result = 0;

    memset(capture, 0, sizeof(*capture));
    capture->pcm = (uint8_t *)calloc(FIXTURE_PCM_BYTES, 1U);
    pcm = (SINT32 *)calloc(FIXTURE_FRAMES * FIXTURE_CHANNELS, sizeof(*pcm));
    if (capture->pcm == 0 || pcm == 0) {
        free(capture->pcm);
        free(pcm);
        capture->pcm = 0;
        return -1;
    }

    start = measure ? fixture_now(clock_fn, context) : 0;
    opngen_initialize(FIXTURE_RATE_HZ);
    end = measure ? fixture_now(clock_fn, context) : 0;
    capture->init_us = measure && end >= start ? end - start : 0;
    opngen_setvol(128U);
    opngen_reset(&state);
    opngen_setcfg(&state, 3U, (UINT32)(OPN_STEREO | 0x003U));
    fixture_capture_tables(capture);

    if (options->silence) {
        result = fixture_render(&state, pcm, 0, 480U, partition, clock_fn,
                                context, false, &capture->render_us);
    } else {
        result = fixture_render(&state, pcm, cursor, 480U, partition, clock_fn,
                                context, measure, &capture->render_us);
        cursor += 480U;
        fixture_program_channel0(&state);
        opngen_keyon(&state, 0U, 0xf0U);
        result |= fixture_render(&state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        if (options->frequency_mode == 0U) {
            opngen_setreg(&state, 0, 0xa4, 0x24);
            opngen_setreg(&state, 0, 0xa0, 0x20);
        } else {
            opngen_setreg(&state, 0, 0xa4, 0x25);
            opngen_setreg(&state, 0, 0xa0, 0x40);
        }
        result |= fixture_render(&state, pcm, cursor, 2400U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 2400U;
        fixture_program_channel1(&state);
        opngen_keyon(&state, 1U, 0xf0U);
        result |= fixture_render(&state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        opngen_setreg(&state, 0, 0x4c, 0x18);
        opngen_setreg(&state, 0, 0x6c, 0x14);
        result |= fixture_render(&state, pcm, cursor, 2400U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 2400U;
        if (!options->omit_keyoff) {
            opngen_keyon(&state, 0U, 0x00U);
        }
        result |= fixture_render(&state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        if (!options->omit_keyoff) {
            opngen_keyon(&state, 1U, 0x00U);
        }
        result |= fixture_render(&state, pcm, cursor, 9120U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 9120U;
        if (cursor != FIXTURE_FRAMES) {
            result = -1;
        }
    }

    fixture_finalize_pcm(capture, pcm);
    free(pcm);
    return result;
}

static bool fixture_capture_equal(const struct fixture_capture *left,
                                  const struct fixture_capture *right)
{
    return left->pcm != 0 && right->pcm != 0 &&
           memcmp(left->pcm, right->pcm, FIXTURE_PCM_BYTES) == 0 &&
           left->sintable_crc32 == right->sintable_crc32 &&
           left->envtable_crc32 == right->envtable_crc32 &&
           left->envcurve_crc32 == right->envcurve_crc32 &&
           left->ratebit == right->ratebit &&
           left->calc1024 == right->calc1024 && left->fmvol == right->fmvol &&
           left->pcm_crc32 == right->pcm_crc32 &&
           memcmp(left->sha256, right->sha256, sizeof(left->sha256)) == 0 &&
           left->s32_abs_peak == right->s32_abs_peak &&
           left->nonzero_s16_samples == right->nonzero_s16_samples &&
           left->clip_samples == right->clip_samples &&
           left->l_sumsq == right->l_sumsq && left->r_sumsq == right->r_sumsq &&
           left->l_rms_q16 == right->l_rms_q16 &&
           left->r_rms_q16 == right->r_rms_q16;
}

static void fixture_free_capture(struct fixture_capture *capture)
{
    free(capture->pcm);
    capture->pcm = 0;
}

static void fixture_print_sha(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    for (i = 0; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

static bool fixture_all_zero(const struct fixture_capture *capture)
{
    size_t i;
    for (i = 0; i < FIXTURE_PCM_BYTES; ++i) {
        if (capture->pcm[i] != 0U) {
            return false;
        }
    }
    return true;
}

int np2opngen_fixture_run(np2opngen_fixture_clock_fn clock_fn, void *context)
{
    const struct fixture_options normal_options = {0U, false, false};
    const struct fixture_options frequency_options = {1U, false, false};
    const struct fixture_options keyoff_options = {0U, true, false};
    const struct fixture_options silence_options = {0U, false, true};
    struct fixture_capture normal;
    struct fixture_capture repeat;
    struct fixture_capture chunk240;
    struct fixture_capture chunk1;
    struct fixture_capture silence;
    struct fixture_capture frequency;
    struct fixture_capture keyoff;
    int result = 0;

    result |= fixture_run_vector(FIXTURE_NORMAL, &normal_options, clock_fn,
                                 context, true, &normal);
    result |= fixture_run_vector(FIXTURE_NORMAL, &normal_options, clock_fn,
                                 context, false, &repeat);
    result |= fixture_run_vector(FIXTURE_CHUNK240, &normal_options, clock_fn,
                                 context, false, &chunk240);
    result |= fixture_run_vector(FIXTURE_CHUNK1, &normal_options, clock_fn,
                                 context, false, &chunk1);
    result |= fixture_run_vector(FIXTURE_NORMAL, &silence_options, clock_fn,
                                 context, false, &silence);
    result |= fixture_run_vector(FIXTURE_NORMAL, &frequency_options, clock_fn,
                                 context, false, &frequency);
    result |= fixture_run_vector(FIXTURE_NORMAL, &keyoff_options, clock_fn,
                                 context, false, &keyoff);

    if (normal.pcm == 0 || repeat.pcm == 0 || chunk240.pcm == 0 ||
        chunk1.pcm == 0 || silence.pcm == 0 || frequency.pcm == 0 ||
        keyoff.pcm == 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=allocation\n");
        result = -1;
        goto cleanup;
    }
    if (!fixture_capture_equal(&normal, &repeat)) {
        result = -1;
    }
    const bool partition_240_ok = fixture_capture_equal(&normal, &chunk240);
    const bool partition_1_ok = fixture_capture_equal(&normal, &chunk1);
    const bool silence_ok = fixture_all_zero(&silence);
    const bool nontrivial_ok = normal.nonzero_s16_samples != 0U;
    const bool frequency_ok = memcmp(normal.pcm, frequency.pcm, FIXTURE_PCM_BYTES) != 0;
    const bool keyoff_ok = memcmp(normal.pcm, keyoff.pcm, FIXTURE_PCM_BYTES) != 0;
    if (!partition_240_ok || !partition_1_ok || !silence_ok || !nontrivial_ok ||
        !frequency_ok || !keyoff_ok) {
        result = -1;
    }

    if (result != 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=invariant\n");
        goto cleanup;
    }

    printf("E1_OPNGEN_META version=1 rate_hz=%u frames=%u channels=%u pcm_bytes=%u volume=128\n",
           FIXTURE_RATE_HZ, FIXTURE_FRAMES, FIXTURE_CHANNELS, FIXTURE_PCM_BYTES);
    printf("E1_OPNGEN_TABLE ratebit=%" PRIu32 " calc1024=%" PRId32
           " fmvol=%" PRId32 " sintable_crc32=0x%08" PRIx32
           " envtable_crc32=0x%08" PRIx32 " envcurve_crc32=0x%08" PRIx32 "\n",
           normal.ratebit, normal.calc1024, normal.fmvol,
           normal.sintable_crc32, normal.envtable_crc32, normal.envcurve_crc32);
    printf("E1_OPNGEN_PCM crc_algorithm=crc32_iso_hdlc crc32=0x%08" PRIx32
           " sha256=", normal.pcm_crc32);
    fixture_print_sha(normal.sha256);
    printf(" s32_abs_peak=%" PRIu64 " nonzero_s16_samples=%" PRIu64
           " clip_samples=%" PRIu64 "\n",
           normal.s32_abs_peak, normal.nonzero_s16_samples, normal.clip_samples);
    printf("E1_OPNGEN_METRICS l_sumsq=%" PRIu64 " r_sumsq=%" PRIu64
           " l_rms_q16=%" PRIu64 " r_rms_q16=%" PRIu64 "\n",
           normal.l_sumsq, normal.r_sumsq, normal.l_rms_q16, normal.r_rms_q16);
    printf("E1_OPNGEN_INVARIANTS repeat=PASS partition_240=%s partition_1=%s"
           " silence=%s frequency_change=%s keyoff=%s nontrivial=%s\n",
           partition_240_ok ? "PASS" : "FAIL", partition_1_ok ? "PASS" : "FAIL",
           silence_ok ? "PASS" : "FAIL", frequency_ok ? "PASS" : "FAIL",
           keyoff_ok ? "PASS" : "FAIL", nontrivial_ok ? "PASS" : "FAIL");
    {
        const uint64_t logical_us = UINT64_C(600000);
        const uint64_t us_per_frame_q16 =
            normal.render_us == 0U ? 0U
                                   : (normal.render_us * UINT64_C(65536)) /
                                         FIXTURE_FRAMES;
        const uint64_t realtime_factor_q16 =
            normal.render_us == 0U
                ? 0U
                : (logical_us * UINT64_C(65536)) / normal.render_us;
        printf("E1_OPNGEN_TIMING init_us=%" PRIu64 " render_us=%" PRIu64
               " us_per_frame_q16=%" PRIu64 " realtime_factor_q16=%" PRIu64 "\n",
               normal.init_us, normal.render_us, us_per_frame_q16,
               realtime_factor_q16);
    }
    printf("E1_OPNGEN_RESULT=PASS\n");

cleanup:
    fixture_free_capture(&normal);
    fixture_free_capture(&repeat);
    fixture_free_capture(&chunk240);
    fixture_free_capture(&chunk1);
    fixture_free_capture(&silence);
    fixture_free_capture(&frequency);
    fixture_free_capture(&keyoff);
    return result == 0 ? 0 : -1;
}
