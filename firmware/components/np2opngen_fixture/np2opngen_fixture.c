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
#include "np2opngen_pcm_canonical.h"
#include "np2opngen_synth_event.h"
#include "np2_sha256.h"

#define FIXTURE_RATE_HZ 48000U
#define FIXTURE_FRAMES 28800U
#define FIXTURE_CHANNELS 2U
#define FIXTURE_PCM_BYTES (FIXTURE_FRAMES * FIXTURE_CHANNELS * 2U)
#define FIXTURE_CAPTURE_COUNT 8U

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

struct fixture_event_render_context {
    OPNGEN state;
    SINT32 *pcm;
    enum fixture_partition partition;
    np2opngen_fixture_clock_fn clock_fn;
    void *clock_context;
    bool measure;
    uint64_t render_us;
};

/* The sole primary event vector.  It is intentionally hand-authored so the
 * event protocol, rather than a procedural generator, is what is tested. */
#define FIXTURE_EVENT_COUNT 64U
#define FIXTURE_EVENT_REG(ts, seq, reg_, value_) \
    { (ts), (seq), NP2_SYNTH_EVENT_REGISTER_WRITE, \
      { .register_write = { .chbase = 0U, .reg = (reg_), \
                            .value = (value_) } } }
#define FIXTURE_EVENT_KEY(ts, seq, channel_, value_) \
    { (ts), (seq), NP2_SYNTH_EVENT_KEY_EVENT, \
      { .key_event = { (channel_), (value_), 0U } } }

static const struct np2opngen_synth_event fixture_primary_events[
    FIXTURE_EVENT_COUNT] = {
    FIXTURE_EVENT_REG(480U, 0U, 0x30U, 0x01U),
    FIXTURE_EVENT_REG(480U, 1U, 0x34U, 0x01U),
    FIXTURE_EVENT_REG(480U, 2U, 0x38U, 0x02U),
    FIXTURE_EVENT_REG(480U, 3U, 0x3cU, 0x01U),
    FIXTURE_EVENT_REG(480U, 4U, 0x40U, 0x28U),
    FIXTURE_EVENT_REG(480U, 5U, 0x44U, 0x30U),
    FIXTURE_EVENT_REG(480U, 6U, 0x48U, 0x20U),
    FIXTURE_EVENT_REG(480U, 7U, 0x4cU, 0x00U),
    FIXTURE_EVENT_REG(480U, 8U, 0x50U, 0x1fU),
    FIXTURE_EVENT_REG(480U, 9U, 0x54U, 0x1aU),
    FIXTURE_EVENT_REG(480U, 10U, 0x58U, 0x16U),
    FIXTURE_EVENT_REG(480U, 11U, 0x5cU, 0x1fU),
    FIXTURE_EVENT_REG(480U, 12U, 0x60U, 0x0eU),
    FIXTURE_EVENT_REG(480U, 13U, 0x64U, 0x0aU),
    FIXTURE_EVENT_REG(480U, 14U, 0x68U, 0x08U),
    FIXTURE_EVENT_REG(480U, 15U, 0x6cU, 0x10U),
    FIXTURE_EVENT_REG(480U, 16U, 0x70U, 0x06U),
    FIXTURE_EVENT_REG(480U, 17U, 0x74U, 0x05U),
    FIXTURE_EVENT_REG(480U, 18U, 0x78U, 0x04U),
    FIXTURE_EVENT_REG(480U, 19U, 0x7cU, 0x08U),
    FIXTURE_EVENT_REG(480U, 20U, 0x80U, 0x1fU),
    FIXTURE_EVENT_REG(480U, 21U, 0x84U, 0x1fU),
    FIXTURE_EVENT_REG(480U, 22U, 0x88U, 0x1fU),
    FIXTURE_EVENT_REG(480U, 23U, 0x8cU, 0x1fU),
    FIXTURE_EVENT_REG(480U, 24U, 0xb0U, 0x36U),
    FIXTURE_EVENT_REG(480U, 25U, 0xb4U, 0xc0U),
    FIXTURE_EVENT_REG(480U, 26U, 0xa4U, 0x22U),
    FIXTURE_EVENT_REG(480U, 27U, 0xa0U, 0x69U),
    FIXTURE_EVENT_KEY(480U, 28U, 0U, 0xf0U),
    FIXTURE_EVENT_REG(5280U, 29U, 0xa4U, 0x24U),
    FIXTURE_EVENT_REG(5280U, 30U, 0xa0U, 0x20U),
    FIXTURE_EVENT_REG(7680U, 31U, 0x31U, 0x11U),
    FIXTURE_EVENT_REG(7680U, 32U, 0x35U, 0x01U),
    FIXTURE_EVENT_REG(7680U, 33U, 0x39U, 0x12U),
    FIXTURE_EVENT_REG(7680U, 34U, 0x3dU, 0x02U),
    FIXTURE_EVENT_REG(7680U, 35U, 0x41U, 0x38U),
    FIXTURE_EVENT_REG(7680U, 36U, 0x45U, 0x48U),
    FIXTURE_EVENT_REG(7680U, 37U, 0x49U, 0x20U),
    FIXTURE_EVENT_REG(7680U, 38U, 0x4dU, 0x04U),
    FIXTURE_EVENT_REG(7680U, 39U, 0x51U, 0x1bU),
    FIXTURE_EVENT_REG(7680U, 40U, 0x55U, 0x17U),
    FIXTURE_EVENT_REG(7680U, 41U, 0x59U, 0x13U),
    FIXTURE_EVENT_REG(7680U, 42U, 0x5dU, 0x1fU),
    FIXTURE_EVENT_REG(7680U, 43U, 0x61U, 0x0cU),
    FIXTURE_EVENT_REG(7680U, 44U, 0x65U, 0x0aU),
    FIXTURE_EVENT_REG(7680U, 45U, 0x69U, 0x08U),
    FIXTURE_EVENT_REG(7680U, 46U, 0x6dU, 0x10U),
    FIXTURE_EVENT_REG(7680U, 47U, 0x71U, 0x06U),
    FIXTURE_EVENT_REG(7680U, 48U, 0x75U, 0x05U),
    FIXTURE_EVENT_REG(7680U, 49U, 0x79U, 0x04U),
    FIXTURE_EVENT_REG(7680U, 50U, 0x7dU, 0x08U),
    FIXTURE_EVENT_REG(7680U, 51U, 0x81U, 0x1fU),
    FIXTURE_EVENT_REG(7680U, 52U, 0x85U, 0x1fU),
    FIXTURE_EVENT_REG(7680U, 53U, 0x89U, 0x1fU),
    FIXTURE_EVENT_REG(7680U, 54U, 0x8dU, 0x1fU),
    FIXTURE_EVENT_REG(7680U, 55U, 0xb1U, 0x2dU),
    FIXTURE_EVENT_REG(7680U, 56U, 0xb5U, 0x80U),
    FIXTURE_EVENT_REG(7680U, 57U, 0xa5U, 0x24U),
    FIXTURE_EVENT_REG(7680U, 58U, 0xa1U, 0xd0U),
    FIXTURE_EVENT_KEY(7680U, 59U, 1U, 0xf0U),
    FIXTURE_EVENT_REG(12480U, 60U, 0x4cU, 0x18U),
    FIXTURE_EVENT_REG(12480U, 61U, 0x6cU, 0x14U),
    FIXTURE_EVENT_KEY(14880U, 62U, 0U, 0x00U),
    FIXTURE_EVENT_KEY(19680U, 63U, 1U, 0x00U),
};

#undef FIXTURE_EVENT_REG
#undef FIXTURE_EVENT_KEY

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

static int fixture_event_render(void *context, uint64_t cursor,
                                uint64_t frame_count)
{
    struct fixture_event_render_context *render =
        (struct fixture_event_render_context *)context;
    if (cursor > FIXTURE_FRAMES || frame_count > FIXTURE_FRAMES - cursor ||
        frame_count > UINT32_MAX) {
        return -1;
    }
    if (frame_count == 0U) {
        return 0;
    }
    return fixture_render(render->state, render->pcm, (uint32_t)cursor,
                          (uint32_t)frame_count, render->partition,
                          render->clock_fn, render->clock_context,
                          render->measure, &render->render_us);
}

static int fixture_event_apply(void *context,
                               const struct np2opngen_synth_event *event)
{
    const struct fixture_event_render_context *render =
        (const struct fixture_event_render_context *)context;
    OPNGEN state = render->state;
    if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE) {
        opngen_setreg(state, event->payload.register_write.chbase,
                      event->payload.register_write.reg,
                      event->payload.register_write.value);
        return 0;
    }
    if (event->type == NP2_SYNTH_EVENT_KEY_EVENT) {
        opngen_keyon(state, event->payload.key_event.channel,
                     event->payload.key_event.value);
        return 0;
    }
    return -1;
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

static void fixture_finalize_pcm(struct fixture_capture *capture,
                                 const SINT32 *pcm)
{
    np2_sha256_context sha;
    struct np2opngen_pcm_stats stats;

    if (np2opngen_pcm_canonicalize_s16le(
            pcm, FIXTURE_FRAMES, FIXTURE_CHANNELS, capture->pcm,
            FIXTURE_PCM_BYTES, &stats) != 0) {
        memset(capture->pcm, 0, FIXTURE_PCM_BYTES);
        memset(&stats, 0, sizeof(stats));
    }

    capture->pcm_crc32 = np2_crc32_iso_hdlc_finish(
        np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(), capture->pcm,
                                  FIXTURE_PCM_BYTES));
    np2_sha256_init(&sha);
    np2_sha256_update(&sha, capture->pcm, FIXTURE_PCM_BYTES);
    np2_sha256_final(&sha, capture->sha256);
    capture->s32_abs_peak = stats.s32_abs_peak;
    capture->nonzero_s16_samples = stats.nonzero_s16_samples;
    capture->clip_samples = stats.clip_samples;
    capture->l_sumsq = stats.l_sumsq;
    capture->r_sumsq = stats.r_sumsq;
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
    OPNGEN state;
    SINT32 *pcm;
    uint32_t cursor = 0;
    uint64_t start;
    uint64_t end;
    int result = 0;

    memset(capture, 0, sizeof(*capture));
    capture->pcm = (uint8_t *)calloc(FIXTURE_PCM_BYTES, 1U);
    pcm = (SINT32 *)calloc(FIXTURE_FRAMES * FIXTURE_CHANNELS, sizeof(*pcm));
    state = (OPNGEN)calloc(1U, sizeof(*state));
    if (capture->pcm == 0 || pcm == 0 || state == 0) {
        free(capture->pcm);
        free(pcm);
        free(state);
        capture->pcm = 0;
        return -1;
    }

    start = measure ? fixture_now(clock_fn, context) : 0;
    opngen_initialize(FIXTURE_RATE_HZ);
    end = measure ? fixture_now(clock_fn, context) : 0;
    capture->init_us = measure && end >= start ? end - start : 0;
    opngen_setvol(128U);
    opngen_reset(state);
    opngen_setcfg(state, 3U, (UINT32)(OPN_STEREO | 0x003U));
    fixture_capture_tables(capture);

    if (options->silence) {
        result = fixture_render(state, pcm, 0, 480U, partition, clock_fn,
                                context, false, &capture->render_us);
    } else {
        result = fixture_render(state, pcm, cursor, 480U, partition, clock_fn,
                                context, measure, &capture->render_us);
        cursor += 480U;
        fixture_program_channel0(state);
        opngen_keyon(state, 0U, 0xf0U);
        result |= fixture_render(state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        if (options->frequency_mode == 0U) {
            opngen_setreg(state, 0, 0xa4, 0x24);
            opngen_setreg(state, 0, 0xa0, 0x20);
        } else {
            opngen_setreg(state, 0, 0xa4, 0x25);
            opngen_setreg(state, 0, 0xa0, 0x40);
        }
        result |= fixture_render(state, pcm, cursor, 2400U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 2400U;
        fixture_program_channel1(state);
        opngen_keyon(state, 1U, 0xf0U);
        result |= fixture_render(state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        opngen_setreg(state, 0, 0x4c, 0x18);
        opngen_setreg(state, 0, 0x6c, 0x14);
        result |= fixture_render(state, pcm, cursor, 2400U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 2400U;
        if (!options->omit_keyoff) {
            opngen_keyon(state, 0U, 0x00U);
        }
        result |= fixture_render(state, pcm, cursor, 4800U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 4800U;
        if (!options->omit_keyoff) {
            opngen_keyon(state, 1U, 0x00U);
        }
        result |= fixture_render(state, pcm, cursor, 9120U, partition, clock_fn,
                                 context, measure, &capture->render_us);
        cursor += 9120U;
        if (cursor != FIXTURE_FRAMES) {
            result = -1;
        }
    }

    fixture_finalize_pcm(capture, pcm);
    free(pcm);
    free(state);
    return result;
}

static int fixture_run_event_list(
    enum fixture_partition partition,
    const struct np2opngen_synth_event *events, size_t event_count,
    np2opngen_fixture_clock_fn clock_fn, void *context, bool measure,
    struct fixture_capture *capture,
    struct np2opngen_synth_event_observer *observer)
{
    struct fixture_event_render_context render;
    OPNGEN state;
    SINT32 *pcm;
    uint64_t start;
    uint64_t end;
    int status;

    memset(capture, 0, sizeof(*capture));
    capture->pcm = (uint8_t *)calloc(FIXTURE_PCM_BYTES, 1U);
    pcm = (SINT32 *)calloc(FIXTURE_FRAMES * FIXTURE_CHANNELS, sizeof(*pcm));
    if (capture->pcm == 0 || pcm == 0) {
        free(capture->pcm);
        free(pcm);
        capture->pcm = 0;
        return -1;
    }

    /* Validate the complete list before touching the OPNGEN state. */
    status = np2opngen_synth_event_validate(
        events, event_count, FIXTURE_FRAMES, 0U, FIXTURE_FRAMES);
    if (status != NP2_SYNTH_EVENT_STATUS_OK) {
        free(capture->pcm);
        free(pcm);
        capture->pcm = 0;
        return -1;
    }

    state = (OPNGEN)calloc(1U, sizeof(*state));
    if (state == 0) {
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
    opngen_reset(state);
    opngen_setcfg(state, 3U, (UINT32)(OPN_STEREO | 0x003U));
    fixture_capture_tables(capture);

    memset(&render, 0, sizeof(render));
    render.state = state;
    render.pcm = pcm;
    render.partition = partition;
    render.clock_fn = clock_fn;
    render.clock_context = context;
    render.measure = measure;
    status = np2opngen_synth_event_interpret(
        events, event_count, FIXTURE_FRAMES, 0U, FIXTURE_FRAMES,
        fixture_event_render, fixture_event_apply, &render, observer);
    capture->render_us = render.render_us;
    fixture_finalize_pcm(capture, pcm);
    free(pcm);
    free(state);
    return status == NP2_SYNTH_EVENT_STATUS_OK ? 0 : -1;
}

static int fixture_run_event_vector(enum fixture_partition partition,
                                    const struct fixture_options *options,
                                    np2opngen_fixture_clock_fn clock_fn,
                                    void *context, bool measure,
                                    struct fixture_capture *capture,
                                    struct np2opngen_synth_event_observer *observer)
{
    const struct np2opngen_synth_event *events = fixture_primary_events;
    struct np2opngen_synth_event *mutable_events = 0;
    size_t event_count = 0U;
    int status;

    memset(capture, 0, sizeof(*capture));
    if (options->silence) {
        return fixture_run_event_list(partition, 0, 0U, clock_fn, context,
                                      measure, capture, observer);
    }

    event_count = options->omit_keyoff ? FIXTURE_EVENT_COUNT - 2U
                                       : FIXTURE_EVENT_COUNT;
    if (options->frequency_mode != 0U) {
        mutable_events = (struct np2opngen_synth_event *)calloc(
            FIXTURE_EVENT_COUNT, sizeof(*mutable_events));
        if (mutable_events == 0) {
            return -1;
        }
        memcpy(mutable_events, fixture_primary_events,
               FIXTURE_EVENT_COUNT * sizeof(*mutable_events));
        mutable_events[29U].payload.register_write.value = 0x25U;
        mutable_events[30U].payload.register_write.value = 0x40U;
        events = mutable_events;
    }

    status = fixture_run_event_list(partition, events, event_count, clock_fn,
                                    context, measure, capture, observer);
    free(mutable_events);
    return status;
}

static void fixture_free_capture(struct fixture_capture *capture);
static bool fixture_all_zero(const struct fixture_capture *capture);

static int fixture_run_order_control(bool reverse,
                                     struct fixture_capture *capture,
                                     struct np2opngen_synth_event_observer *observer)
{
    enum { FIXTURE_ORDER_SETUP_COUNT = 26U, FIXTURE_ORDER_EVENT_COUNT = 29U };
    struct np2opngen_synth_event *events =
        (struct np2opngen_synth_event *)calloc(FIXTURE_ORDER_EVENT_COUNT,
                                                sizeof(*events));
    int status;

    memset(capture, 0, sizeof(*capture));
    if (events == 0) {
        return -1;
    }
    memcpy(events, fixture_primary_events,
           FIXTURE_ORDER_SETUP_COUNT * sizeof(*events));
    events[26U] = fixture_primary_events[reverse ? 27U : 26U];
    events[26U].sequence = 26U;
    events[27U] = fixture_primary_events[reverse ? 26U : 27U];
    events[27U].sequence = 27U;
    events[28U] = fixture_primary_events[28U];
    events[28U].sequence = 28U;
    status = fixture_run_event_list(FIXTURE_NORMAL, events,
                                    FIXTURE_ORDER_EVENT_COUNT, 0, 0, false,
                                    capture, observer);
    free(events);
    return status;
}

static bool fixture_order_sensitive_control(void)
{
    struct fixture_capture first_capture = {0};
    struct fixture_capture reordered_capture = {0};
    struct np2opngen_synth_event_observer observer;
    bool changed;

    if (fixture_run_order_control(false, &first_capture, &observer) != 0 ||
        fixture_run_order_control(true, &reordered_capture, &observer) != 0) {
        fixture_free_capture(&first_capture);
        fixture_free_capture(&reordered_capture);
        return false;
    }
    changed = first_capture.pcm != 0 && reordered_capture.pcm != 0 &&
              !fixture_all_zero(&first_capture) &&
              !fixture_all_zero(&reordered_capture) &&
              memcmp(first_capture.pcm, reordered_capture.pcm,
                     FIXTURE_PCM_BYTES) != 0;
    fixture_free_capture(&first_capture);
    fixture_free_capture(&reordered_capture);
    return changed;
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

int np2opngen_fixture_run_with_sink(
    np2opngen_fixture_clock_fn clock_fn, void *context,
    np2opngen_fixture_pcm_sink_fn pcm_sink, void *pcm_sink_context)
{
    const struct fixture_options normal_options = {0U, false, false};
    const struct fixture_options frequency_options = {1U, false, false};
    const struct fixture_options keyoff_options = {0U, true, false};
    const struct fixture_options silence_options = {0U, false, true};
    struct fixture_capture *captures = (struct fixture_capture *)calloc(
        FIXTURE_CAPTURE_COUNT, sizeof(*captures));
    struct fixture_capture *reference;
    struct fixture_capture *normal;
    struct fixture_capture *repeat;
    struct fixture_capture *chunk240;
    struct fixture_capture *chunk1;
    struct fixture_capture *silence;
    struct fixture_capture *frequency;
    struct fixture_capture *keyoff;
    struct np2opngen_synth_event_observer normal_observer;
    uint32_t event_trace_crc32 = 0U;
    uint8_t event_trace_sha256[NP2_SHA256_DIGEST_SIZE];
    int event_trace_status;
    int result = 0;

    if (captures == 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=allocation\n");
        return -1;
    }
    reference = &captures[0];
    normal = &captures[1];
    repeat = &captures[2];
    chunk240 = &captures[3];
    chunk1 = &captures[4];
    silence = &captures[5];
    frequency = &captures[6];
    keyoff = &captures[7];

    /* Keep this procedural vector as an independent reference implementation. */
    result |= fixture_run_vector(FIXTURE_NORMAL, &normal_options, clock_fn,
                                 context, false, reference);
    result |= fixture_run_event_vector(FIXTURE_NORMAL, &normal_options, clock_fn,
                                       context, true, normal,
                                       &normal_observer);
    result |= fixture_run_event_vector(FIXTURE_NORMAL, &normal_options, clock_fn,
                                       context, false, repeat, 0);
    result |= fixture_run_event_vector(FIXTURE_CHUNK240, &normal_options, clock_fn,
                                       context, false, chunk240, 0);
    result |= fixture_run_event_vector(FIXTURE_CHUNK1, &normal_options, clock_fn,
                                       context, false, chunk1, 0);
    result |= fixture_run_event_vector(FIXTURE_NORMAL, &silence_options, clock_fn,
                                       context, false, silence, 0);
    result |= fixture_run_event_vector(FIXTURE_NORMAL, &frequency_options, clock_fn,
                                       context, false, frequency, 0);
    result |= fixture_run_event_vector(FIXTURE_NORMAL, &keyoff_options, clock_fn,
                                       context, false, keyoff, 0);

    if (reference->pcm == 0 || normal->pcm == 0 || repeat->pcm == 0 ||
        chunk240->pcm == 0 || chunk1->pcm == 0 || silence->pcm == 0 ||
        frequency->pcm == 0 || keyoff->pcm == 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=allocation\n");
        result = -1;
        goto cleanup;
    }
    const bool reference_match = fixture_capture_equal(reference, normal);
    if (!reference_match || !fixture_capture_equal(normal, repeat)) {
        result = -1;
    }
    const bool partition_240_ok = fixture_capture_equal(normal, chunk240);
    const bool partition_1_ok = fixture_capture_equal(normal, chunk1);
    const bool silence_ok = fixture_all_zero(silence);
    const bool nontrivial_ok = normal->nonzero_s16_samples != 0U;
    const bool frequency_ok = memcmp(normal->pcm, frequency->pcm, FIXTURE_PCM_BYTES) != 0;
    const bool keyoff_ok = memcmp(normal->pcm, keyoff->pcm, FIXTURE_PCM_BYTES) != 0;
    const bool order_sensitive_ok = fixture_order_sensitive_control();
    event_trace_status = np2opngen_synth_event_trace(
        fixture_primary_events, FIXTURE_EVENT_COUNT, &event_trace_crc32,
        event_trace_sha256);
    if (!partition_240_ok || !partition_1_ok || !silence_ok || !nontrivial_ok ||
        !frequency_ok || !keyoff_ok || !order_sensitive_ok ||
        normal_observer.applied_count != FIXTURE_EVENT_COUNT ||
        !normal_observer.has_last_sequence ||
        !normal_observer.order_valid ||
        normal_observer.last_sequence != FIXTURE_EVENT_COUNT - 1U ||
        normal_observer.expected_sequence != FIXTURE_EVENT_COUNT ||
        event_trace_status != NP2_SYNTH_EVENT_STATUS_OK) {
        result = -1;
    }

    if (result != 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=invariant\n");
        goto cleanup;
    }

    if (pcm_sink != 0 &&
        pcm_sink(normal->pcm, FIXTURE_PCM_BYTES, FIXTURE_RATE_HZ,
                 FIXTURE_CHANNELS, 16U, pcm_sink_context) != 0) {
        printf("E1_OPNGEN_RESULT=FAIL reason=pcm_sink\n");
        result = -1;
        goto cleanup;
    }

    printf("E1_OPNGEN_META version=1 rate_hz=%u frames=%u channels=%u pcm_bytes=%u volume=128\n",
           FIXTURE_RATE_HZ, FIXTURE_FRAMES, FIXTURE_CHANNELS, FIXTURE_PCM_BYTES);
    printf("E1_OPNGEN_TABLE ratebit=%" PRIu32 " calc1024=%" PRId32
           " fmvol=%" PRId32 " sintable_crc32=0x%08" PRIx32
           " envtable_crc32=0x%08" PRIx32 " envcurve_crc32=0x%08" PRIx32 "\n",
           normal->ratebit, normal->calc1024, normal->fmvol,
           normal->sintable_crc32, normal->envtable_crc32, normal->envcurve_crc32);
    printf("E1_OPNGEN_PCM crc_algorithm=crc32_iso_hdlc crc32=0x%08" PRIx32
           " sha256=", normal->pcm_crc32);
    fixture_print_sha(normal->sha256);
    printf(" s32_abs_peak=%" PRIu64 " nonzero_s16_samples=%" PRIu64
           " clip_samples=%" PRIu64 "\n",
           normal->s32_abs_peak, normal->nonzero_s16_samples, normal->clip_samples);
    printf("E1_OPNGEN_METRICS l_sumsq=%" PRIu64 " r_sumsq=%" PRIu64
           " l_rms_q16=%" PRIu64 " r_rms_q16=%" PRIu64 "\n",
           normal->l_sumsq, normal->r_sumsq, normal->l_rms_q16, normal->r_rms_q16);
    printf("E1_OPNGEN_INVARIANTS repeat=PASS partition_240=%s partition_1=%s"
           " silence=%s frequency_change=%s keyoff=%s nontrivial=%s\n",
           partition_240_ok ? "PASS" : "FAIL", partition_1_ok ? "PASS" : "FAIL",
           silence_ok ? "PASS" : "FAIL", frequency_ok ? "PASS" : "FAIL",
           keyoff_ok ? "PASS" : "FAIL", nontrivial_ok ? "PASS" : "FAIL");
    {
        const uint64_t logical_us = UINT64_C(600000);
        const uint64_t us_per_frame_q16 =
            normal->render_us == 0U ? 0U
                                   : (normal->render_us * UINT64_C(65536)) /
                                         FIXTURE_FRAMES;
        const uint64_t realtime_factor_q16 =
            normal->render_us == 0U
                ? 0U
                : (logical_us * UINT64_C(65536)) / normal->render_us;
        printf("E1_OPNGEN_TIMING init_us=%" PRIu64 " render_us=%" PRIu64
               " us_per_frame_q16=%" PRIu64 " realtime_factor_q16=%" PRIu64 "\n",
               normal->init_us, normal->render_us, us_per_frame_q16,
               realtime_factor_q16);
    }
    printf("E1A_SYNTH_EVENT_META version=1 count=%u record_bytes=24\n",
           FIXTURE_EVENT_COUNT);
    printf("E1A_SYNTH_EVENT_TRACE crc32=0x%08" PRIx32 " sha256=",
           event_trace_crc32);
    fixture_print_sha(event_trace_sha256);
    printf("\n");
    printf("E1A_SYNTH_EVENT_INVARIANTS validation=PASS reference_match=%s"
           " order_sensitive=%s\n",
           reference_match ? "PASS" : "FAIL",
           order_sensitive_ok ? "PASS" : "FAIL");
    printf("E1A_SYNTH_EVENT_RESULT=PASS\n");
    printf("E1_OPNGEN_RESULT=PASS\n");

cleanup:
    if (captures != 0) {
        size_t i;
        for (i = 0U; i < FIXTURE_CAPTURE_COUNT; ++i) {
            fixture_free_capture(&captures[i]);
        }
        free(captures);
    }
    return result == 0 ? 0 : -1;
}

int np2opngen_fixture_run(np2opngen_fixture_clock_fn clock_fn, void *context)
{
    return np2opngen_fixture_run_with_sink(clock_fn, context, 0, 0);
}

int np2opngen_fixture_get_e1_events(
    const struct np2opngen_synth_event **events, size_t *count,
    uint64_t *end_frame)
{
    if (events == 0 || count == 0 || end_frame == 0) {
        return -1;
    }
    *events = fixture_primary_events;
    *count = FIXTURE_EVENT_COUNT;
    *end_frame = FIXTURE_FRAMES;
    return 0;
}
