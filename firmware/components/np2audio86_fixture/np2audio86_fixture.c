#include "np2audio86_fixture.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NP2_AUDIO86_ASYNC_HOST
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#endif

#include <compiler.h>
#include <sound/opngen.h>
#include <sound/pcmmix.h>
#include <sound/pcm86.h>
#include <sound/psggen.h>

#include "np2_crc32.h"
#include "np2audio86_fixture_golden.h"
#include "np2opngen_pcm_canonical.h"

#define AUDIO86_EVENT_FM_KEYON 1U
#define AUDIO86_EVENT_FM_KEYOFF 2U
#define AUDIO86_EVENT_PSG_WRITE 3U
#define AUDIO86_FM_CHANNELS NP2_AUDIO86_FM_CHANNELS
#define AUDIO86_FM_OPERATORS 24U
#define AUDIO86_PSG_CHANNELS 3U
#define AUDIO86_RHYTHM_TRACKS NP2_AUDIO86_RHYTHM_TRACKS
#define AUDIO86_PCM86_FIFO_BYTES NP2_AUDIO86_PCM86_FIFO_BYTES
#define AUDIO86_PCM86_REFILL_BYTES NP2_AUDIO86_PCM86_REFILL_BYTES
#define AUDIO86_RHYTHM_MAX_SAMPLES NP2_AUDIO86_RHYTHM_MAX_SAMPLES

struct audio86_control_event {
    uint64_t frame;
    uint8_t opcode;
    uint8_t target;
    uint16_t value;
    uint32_t auxiliary;
};

static const struct audio86_control_event control_events[] = {
    {0U, AUDIO86_EVENT_FM_KEYON, 0U, 0xf0U, 0U},
    {0U, AUDIO86_EVENT_FM_KEYON, 1U, 0xf0U, 0U},
    {0U, AUDIO86_EVENT_FM_KEYON, 2U, 0xf0U, 0U},
    {0U, AUDIO86_EVENT_FM_KEYON, 3U, 0xf0U, 0U},
    {0U, AUDIO86_EVENT_FM_KEYON, 4U, 0xf0U, 0U},
    {0U, AUDIO86_EVENT_FM_KEYON, 5U, 0xf0U, 0U},
    {37U, AUDIO86_EVENT_FM_KEYOFF, 2U, 0x00U, 0U},
    {481U, AUDIO86_EVENT_FM_KEYON, 2U, 0xf0U, 0U},
    {1007U, AUDIO86_EVENT_PSG_WRITE, 1U, 0x0fU, 8U},
    {1619U, AUDIO86_EVENT_PSG_WRITE, 0U, 0x09U, 13U},
};

typedef struct np2audio86_pcm86_feed audio86_pcm86_feed;
typedef struct np2audio86_render_state audio86_state;
#define audio86_pcm86_feed np2audio86_pcm86_feed
#define audio86_state np2audio86_render_state

/* pcm86g.c is the upstream generator/resampler.  Its device-layer checkbuf
 * callback is intentionally replaced by this narrow synchronous adapter; the
 * fixture owns FIFO accounting and never calls pcm86c.c or pcm86io.c. */
void SOUNDCALL pcm86gen_checkbuf(PCM86 pcm86, UINT nCount)
{
    (void)pcm86;
    (void)nCount;
}

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8U);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static void put_le64(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0U; i < 8U; ++i) {
        out[i] = (uint8_t)(value >> (i * 8U));
    }
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void build_pcm86_source(uint8_t *bytes)
{
    uint32_t random_state = UINT32_C(0x86a2f17d);
    uint32_t frame;
    for (frame = 0U; frame < NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES;
         ++frame) {
        int32_t left = (int32_t)(xorshift32(&random_state) & 0xffffU) -
                       32768;
        int32_t right = (int32_t)(xorshift32(&random_state) & 0xffffU) -
                        32768;
        /* Keep the source deliberately below full scale while retaining both
         * signs and a deterministic left/right difference. */
        left = (left * 3) / 8;
        right = (right * 5) / 12 + 257;
        put_le16(bytes + frame * 4U, (uint16_t)(int16_t)left);
        put_le16(bytes + frame * 4U + 2U, (uint16_t)(int16_t)right);
    }
}

int np2audio86_fixture_generate_source(uint8_t *source)
{
    if (source == NULL) {
        return -1;
    }
    build_pcm86_source(source);
    return 0;
}

static int nonzero_samples(const SINT32 *samples, size_t count)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        if (samples[i] != 0) {
            return 1;
        }
    }
    return 0;
}

void np2audio86_fixture_hash_control(struct np2audio86_fixture_result *result)
{
    np2_sha256_context sha;
    uint32_t crc = np2_crc32_iso_hdlc_init();
    size_t i;
    np2_sha256_init(&sha);
    for (i = 0U; i < sizeof(control_events) / sizeof(control_events[0]);
         ++i) {
        uint8_t serialized[16];
        put_le64(serialized, control_events[i].frame);
        serialized[8] = control_events[i].opcode;
        serialized[9] = control_events[i].target;
        put_le16(serialized + 10U, control_events[i].value);
        put_le32(serialized + 12U, control_events[i].auxiliary);
        crc = np2_crc32_iso_hdlc_update(crc, serialized, sizeof(serialized));
        np2_sha256_update(&sha, serialized, sizeof(serialized));
    }
    result->control_crc32 = np2_crc32_iso_hdlc_finish(crc);
    np2_sha256_final(&sha, result->control_sha256);
    result->control_events = (uint32_t)(sizeof(control_events) /
                                        sizeof(control_events[0]));
    result->mid_quantum_events = 0U;
    for (i = 0U; i < sizeof(control_events) / sizeof(control_events[0]);
         ++i) {
        if ((control_events[i].frame % NP2_AUDIO86_QUANTUM_FRAMES) != 0U) {
            ++result->mid_quantum_events;
        }
    }
}

static void configure_fm(OPNGEN fm)
{
    unsigned channel;
    opngen_initialize(NP2_AUDIO86_RATE_HZ);
    opngen_setvol(48U);
    opngen_reset(fm);
    opngen_setcfg(fm, AUDIO86_FM_CHANNELS,
                  OPN_STEREO | ((UINT32_C(1) << AUDIO86_FM_CHANNELS) - 1U));
    for (channel = 0U; channel < AUDIO86_FM_CHANNELS; ++channel) {
        unsigned base = channel < 3U ? 0U : 3U;
        unsigned index = channel % 3U;
        unsigned op;
        /* The API call is made for every channel; zero selects the normal
         * four-operator state while the six-channel configuration enables the
         * YM2608 extension channel set. */
        opngen_setextch(fm, channel, 0U);
        for (op = 0U; op < 4U; ++op) {
            const unsigned reg = 0x30U + op * 4U + index;
            opngen_setreg(fm, (REG8)base, reg, 0x01U);
            opngen_setreg(fm, (REG8)base, 0x40U + op * 4U + index, 0x50U);
            opngen_setreg(fm, (REG8)base, 0x50U + op * 4U + index, 0x1fU);
            opngen_setreg(fm, (REG8)base, 0x60U + op * 4U + index, 0x0fU);
            opngen_setreg(fm, (REG8)base, 0x70U + op * 4U + index, 0x0fU);
            opngen_setreg(fm, (REG8)base, 0x80U + op * 4U + index, 0x0fU);
        }
        opngen_setreg(fm, (REG8)base, 0xa0U + index, 0x80U);
        opngen_setreg(fm, (REG8)base, 0xa4U + index, 0x22U);
        opngen_setreg(fm, (REG8)base, 0xb0U + index, 0x3fU);
        opngen_setreg(fm, (REG8)base, 0xb4U + index,
                      channel & 1U ? 0x40U : 0x80U);
    }
}

static void configure_psg(PSGGEN psg)
{
    psggen_initialize(NP2_AUDIO86_RATE_HZ);
    psggen_setvol(24U);
    psggen_reset(psg);
    psggen_setreg(psg, 0U, 0x40U);
    psggen_setreg(psg, 1U, 0U);
    psggen_setreg(psg, 2U, 0x60U);
    psggen_setreg(psg, 3U, 0U);
    psggen_setreg(psg, 4U, 0x90U);
    psggen_setreg(psg, 5U, 0U);
    psggen_setreg(psg, 6U, 0x05U);
    psggen_setreg(psg, 7U, 0U); /* three tones and noise enabled */
    psggen_setreg(psg, 8U, 0x10U);
    psggen_setreg(psg, 9U, 0x10U);
    psggen_setreg(psg, 10U, 0x10U);
    psggen_setreg(psg, 11U, 0x20U);
    psggen_setreg(psg, 12U, 0U);
    psggen_setreg(psg, 13U, 0x0aU);
    psggen_setpan(psg, 0U, 0U);
    psggen_setpan(psg, 1U, 1U);
    psggen_setpan(psg, 2U, 2U);
}

static void configure_rhythm(struct audio86_state *state)
{
    unsigned track;
    memset(&state->rhythm, 0, sizeof(state->rhythm));
    state->rhythm.hdr.enable = (UINT32_C(1) << AUDIO86_RHYTHM_TRACKS) - 1U;
    state->rhythm.hdr.playing = state->rhythm.hdr.enable;
    for (track = 0U; track < AUDIO86_RHYTHM_TRACKS; ++track) {
        const unsigned length = 97U + track * 37U;
        unsigned i;
        for (i = 0U; i < length; ++i) {
            const int32_t phase = (int32_t)((i + track * 19U) % 64U);
            const int32_t triangle = phase < 32 ? phase * 2 : 126 - phase * 2;
            state->rhythm_samples[track][i] =
                (SINT16)((triangle - 31) * (24 + (int)track));
        }
        state->rhythm_tracks[track].pcm = state->rhythm_samples[track];
        state->rhythm_tracks[track].remain = length;
        state->rhythm_tracks[track].data.sample = state->rhythm_samples[track];
        state->rhythm_tracks[track].data.samples = length;
        state->rhythm_tracks[track].flag = PMIXFLAG_LOOP |
                                            (track & 1U ? PMIXFLAG_R :
                                                           PMIXFLAG_L);
        state->rhythm_tracks[track].volume = 1024 + (SINT32)track * 64;
    }
    memcpy(state->rhythm.trk, state->rhythm_tracks,
           sizeof(state->rhythm_tracks));
}

static void configure_pcm86(struct audio86_pcm86_feed *feed,
                            const uint8_t *source)
{
    memset(feed, 0, sizeof(*feed));
    if (source == NULL) {
        build_pcm86_source(feed->source);
    } else {
        memcpy(feed->source, source, sizeof(feed->source));
    }
    feed->pcm.fifo = 0x80U | 0U; /* FIFO enabled; 44.1 kHz source selector */
    feed->pcm.dactrl = 0x30U;     /* signed 16-bit stereo */
    /* pcm86g's fixed-point phase is expressed in the upstream eight-times
     * rate domain (pcm86rate8[] in pcm86c.c).  44.1 kHz therefore enters as
     * 352.8 kHz here while the declared source identity remains 44.1 kHz. */
    feed->pcm.div = (SINT32)((NP2_AUDIO86_PCM86_SOURCE_RATE_HZ * 8U * 128U) /
                             NP2_AUDIO86_RATE_HZ);
    feed->pcm.div2 = (SINT32)((NP2_AUDIO86_RATE_HZ * 8192U) /
                              (NP2_AUDIO86_PCM86_SOURCE_RATE_HZ * 8U));
    feed->pcm.divremain = 0;
    feed->pcm.volume = 64;
    feed->pcm.fifosize = 0x80;
    feed->pcm.readpos = 0U;
    feed->pcm.wrtpos = 0U;
    feed->fifo_min = INT_MAX;
    feed->fifo_max = 0;
}

static void feed_pcm86(struct audio86_pcm86_feed *feed, int32_t required)
{
    while (feed->pcm.realbuf < required) {
        int32_t available = AUDIO86_PCM86_FIFO_BYTES - feed->pcm.realbuf;
        uint32_t chunk;
        uint32_t offset;
        if (available <= 0) {
            feed->underrun = 1U;
            return;
        }
        chunk = (uint32_t)available;
        if (chunk > AUDIO86_PCM86_REFILL_BYTES) {
            chunk = AUDIO86_PCM86_REFILL_BYTES;
        }
        chunk &= ~3U;
        offset = feed->pcm.wrtpos & PCM86_BUFMSK;
        {
            uint32_t remaining = chunk;
            uint32_t destination = offset;
            uint32_t source_offset = feed->source_frame * 4U;
            while (remaining != 0U) {
                uint32_t destination_part = PCM86_BUFSIZE - destination;
                uint32_t source_part = NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES -
                                       source_offset;
                uint32_t part = remaining;
                if (part > destination_part) {
                    part = destination_part;
                }
                if (part > source_part) {
                    part = source_part;
                }
                memcpy(feed->pcm.buffer + destination,
                       feed->source + source_offset, part);
                destination = (destination + part) & PCM86_BUFMSK;
                source_offset = (source_offset + part) %
                                NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES;
                remaining -= part;
            }
        }
        feed->pcm.wrtpos = (feed->pcm.wrtpos + chunk) & PCM86_BUFMSK;
        feed->pcm.realbuf += (SINT32)chunk;
        feed->source_frame = (feed->source_frame + chunk / 4U) %
                             NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES;
        feed->supplied += chunk;
        ++feed->refills;
    }
    if (feed->pcm.realbuf < feed->fifo_min) {
        feed->fifo_min = feed->pcm.realbuf;
    }
    if (feed->pcm.realbuf > feed->fifo_max) {
        feed->fifo_max = feed->pcm.realbuf;
    }
}

static int add_source(SINT32 *mix, const SINT32 *source, size_t samples,
                      uint8_t *arithmetic_error)
{
    size_t i;
    for (i = 0U; i < samples; ++i) {
        const int64_t sum = (int64_t)mix[i] + source[i];
        if (sum > INT32_MAX || sum < INT32_MIN) {
            *arithmetic_error = 1U;
            return -1;
        }
        mix[i] = (SINT32)sum;
    }
    return 0;
}

static int render_span(struct audio86_state *state, SINT32 *mix, size_t frames,
                       struct np2audio86_fixture_result *result,
                       int refill_pcm86)
{
    const size_t samples = frames * NP2_AUDIO86_CHANNELS;
    SINT32 *fm = state->fm_scratch;
    SINT32 *psg = state->psg_scratch;
    SINT32 *rhythm = state->rhythm_scratch;
    SINT32 *pcm86 = state->pcm86_scratch;
#if defined(NP2_AUDIO86_PROFILE)
    uint64_t profile_begin;
#endif
    memset(fm, 0, sizeof(state->fm_scratch));
    memset(psg, 0, sizeof(state->psg_scratch));
    memset(rhythm, 0, sizeof(state->rhythm_scratch));
    memset(pcm86, 0, sizeof(state->pcm86_scratch));
    if (refill_pcm86) {
        feed_pcm86(&state->pcm86, 4096);
    }
#if defined(NP2_AUDIO86_PROFILE)
    profile_begin = state->profile_now_us != NULL
                        ? state->profile_now_us(state->profile_clock_opaque)
                        : 0U;
#endif
    opngen_getpcm(&state->fm, fm, (UINT)frames);
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL) {
        state->profile_opngen_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
        profile_begin = state->profile_now_us(state->profile_clock_opaque);
    }
#endif
    psggen_getpcm(&state->psg, psg, (UINT)frames);
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL) {
        state->profile_psggen_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
        profile_begin = state->profile_now_us(state->profile_clock_opaque);
    }
#endif
    pcmmix_getpcm((PCMMIX)&state->rhythm, rhythm, (UINT)frames);
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL) {
        state->profile_rhythm_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
        profile_begin = state->profile_now_us(state->profile_clock_opaque);
    }
#endif
    pcm86gen_getpcm(&state->pcm86.pcm, pcm86, (UINT)frames);
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL) {
        state->profile_pcm86_generation_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
        profile_begin = state->profile_now_us(state->profile_clock_opaque);
    }
#endif
    if (nonzero_samples(fm, samples)) {
        result->fm_contribution = 1U;
    }
    if (nonzero_samples(psg, samples)) {
        result->psg_contribution = 1U;
    }
    if (nonzero_samples(rhythm, samples)) {
        result->rhythm_contribution = 1U;
    }
    if (nonzero_samples(pcm86, samples)) {
        result->pcm86_contribution = 1U;
    }
    if (add_source(mix, fm, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, psg, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, rhythm, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, pcm86, samples, &result->arithmetic_error) != 0) {
        return -1;
    }
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL) {
        state->profile_mix_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
    }
#endif
    state->rendered_frames += frames;
    return 0;
}

static int apply_event(struct audio86_state *state,
                       const struct audio86_control_event *event)
{
    switch (event->opcode) {
    case AUDIO86_EVENT_FM_KEYON:
    case AUDIO86_EVENT_FM_KEYOFF:
        opngen_keyon(&state->fm, event->target,
                     event->opcode == AUDIO86_EVENT_FM_KEYON ? 0xf0U : 0U);
        return 0;
    case AUDIO86_EVENT_PSG_WRITE:
        psggen_setreg(&state->psg, event->auxiliary, (REG8)event->value);
        return 0;
    default:
        return -1;
    }
}

int np2audio86_render_init_with_source(struct np2audio86_render_state *state,
                                       const uint8_t *source)
{
    if (state == NULL) {
        return -1;
    }
    memset(state, 0, sizeof(*state));
    configure_fm(&state->fm);
    configure_psg(&state->psg);
    configure_rhythm(state);
    configure_pcm86(&state->pcm86, source);
    return 0;
}

int np2audio86_render_init(struct np2audio86_render_state *state)
{
    return np2audio86_render_init_with_source(state, NULL);
}

static void worker_pcm_set_rate(PCM86 pcm, uint8_t rate_index)
{
    static const uint32_t rates[8] = {
        352800U, 264600U, 176400U, 132300U,
        88200U, 66150U, 44010U, 33075U
    };
    const uint32_t rate = rates[rate_index & 7U];
    pcm->div = (SINT32)((rate << (PCM86_DIVBIT - 3U)) /
                        NP2_AUDIO86_RATE_HZ);
    pcm->div2 = (SINT32)((NP2_AUDIO86_RATE_HZ << (PCM86_DIVBIT + 3U)) /
                         rate);
}

static void worker_rhythm_setreg(struct np2audio86_render_state *state,
                                 uint8_t value)
{
    unsigned track;
    for (track = 0U; track < AUDIO86_RHYTHM_TRACKS; ++track) {
        if ((value & (uint8_t)(1U << track)) != 0U) {
            PMIXTRK *track_state = &state->rhythm.trk[track];
            track_state->pcm = track_state->data.sample;
            track_state->remain = track_state->data.samples;
            state->rhythm.hdr.playing |= (UINT32_C(1) << track);
        }
    }
}

int np2audio86_render_apply_opna_register(
    struct np2audio86_render_state *state, uint16_t address, uint8_t value)
{
    const uint8_t bank = address >= 0x100U ? 3U : 0U;
    const uint8_t reg = (uint8_t)(address & 0xffU);
    if (state == NULL) {
        return -1;
    }
    if (reg < 0x10U && address < 0x100U) {
        psggen_setreg(&state->psg, reg, value);
    } else if (reg >= 0x10U && reg <= 0x1fU && address < 0x100U) {
        /* OPNA 0x10 is the rhythm trigger; timer and PIC effects stay in G. */
        if (reg == 0x10U) {
            worker_rhythm_setreg(state, value);
        }
    } else if (reg == 0x28U && address < 0x100U) {
        opngen_keyon(&state->fm, value & 7U, value & 0xf0U);
    } else if (reg >= 0x30U) {
        opngen_setreg(&state->fm, bank, reg, value);
    }
    return 0;
}

int np2audio86_render_apply_opna_csm(struct np2audio86_render_state *state)
{
    if (state == NULL) {
        return -1;
    }
    opngen_csm(&state->fm);
    return 0;
}

int np2audio86_render_apply_pcm86_control(
    struct np2audio86_render_state *state, uint8_t register_index,
    uint8_t value)
{
    PCM86 pcm;
    uint8_t old;
    if (state == NULL) {
        return -1;
    }
    pcm = &state->pcm86.pcm;
    switch (register_index) {
    case 0x00U:
        /* A460 extension selection belongs entirely to the guest domain. */
        break;
    case 0x06U:
        if ((value & 0xe0U) == 0xa0U) {
            const uint8_t vol5 = (~value) & 15U;
            pcm->volume = 64 * vol5;
        }
        break;
    case 0x08U:
        old = pcm->fifo;
        if ((value & 8U) && !(old & 8U)) {
            pcm->wrtpos = 0U;
            pcm->readpos = 0U;
            pcm->realbuf = 0;
        }
        if ((old ^ value) & 7U) {
            worker_pcm_set_rate(pcm, value);
        }
        pcm->fifo = value;
        break;
    case 0x0aU:
        if (!(pcm->fifo & 0x20U) && (value & 15U) != 15U) {
            pcm->dactrl = value;
            worker_pcm_set_rate(pcm, pcm->fifo);
        }
        break;
    default:
        return -1;
    }
    return 0;
}

int np2audio86_render_reset(struct np2audio86_render_state *state)
{
    return np2audio86_render_init_with_source(state, NULL);
}

void np2audio86_render_set_profile_clock(
    struct np2audio86_render_state *state,
    uint64_t (*now_us)(void *opaque), void *opaque)
{
    if (state == NULL) {
        return;
    }
#if defined(NP2_AUDIO86_PROFILE)
    state->profile_now_us = now_us;
    state->profile_clock_opaque = opaque;
#else
    (void)now_us;
    (void)opaque;
#endif
}

int np2audio86_render_apply_event(struct np2audio86_render_state *state,
                                  const struct np2audio86_event *event)
{
    struct audio86_control_event control;
    if (state == NULL || event == NULL) {
        return -1;
    }
    memset(&control, 0, sizeof(control));
    control.frame = event->frame_timestamp;
    switch (event->opcode) {
    case NP2_AUDIO86_EVENT_FM_KEY:
        control.target = (uint8_t)(event->payload & 0xffU);
        control.value = (uint16_t)((event->payload >> 8U) & 0xffU);
        control.opcode = control.value == 0U ? AUDIO86_EVENT_FM_KEYOFF
                                              : AUDIO86_EVENT_FM_KEYON;
        break;
    case NP2_AUDIO86_EVENT_PSG_REGISTER:
        control.opcode = AUDIO86_EVENT_PSG_WRITE;
        control.auxiliary = event->payload & 0xffU;
        control.value = (uint16_t)((event->payload >> 8U) & 0xffU);
        break;
    default:
        return -1;
    }
    return apply_event(state, &control);
}

int np2audio86_render_pcm86_push(struct np2audio86_render_state *state,
                                 const uint8_t *bytes, size_t count)
{
    struct np2audio86_pcm86_feed *feed;
    uint32_t destination;
    size_t first;
    if (state == NULL || bytes == NULL || count == 0U ||
        count > NP2_AUDIO86_PCM86_REFILL_BYTES || (count & 3U) != 0U) {
        return -1;
    }
    feed = &state->pcm86;
    if (feed->pcm.realbuf < 0 ||
        feed->pcm.realbuf > PCM86_BUFSIZE - (SINT32)count) {
        feed->underrun = 1U;
        return -1;
    }
    destination = feed->pcm.wrtpos & PCM86_BUFMSK;
    first = PCM86_BUFSIZE - destination;
    if (first > count) {
        first = count;
    }
    memcpy(feed->pcm.buffer + destination, bytes, first);
    if (count > first) {
        memcpy(feed->pcm.buffer, bytes + first, count - first);
    }
    feed->pcm.wrtpos = (feed->pcm.wrtpos + (UINT32)count) & PCM86_BUFMSK;
    feed->pcm.realbuf += (SINT32)count;
    feed->supplied += count;
    ++feed->refills;
    if (feed->pcm.realbuf < feed->fifo_min) {
        feed->fifo_min = feed->pcm.realbuf;
    }
    if (feed->pcm.realbuf > feed->fifo_max) {
        feed->fifo_max = feed->pcm.realbuf;
    }
    return 0;
}

int np2audio86_render_span(struct np2audio86_render_state *state,
                           SINT32 *mix, size_t frames,
                           struct np2audio86_fixture_result *result)
{
    if (state == NULL || mix == NULL || result == NULL || frames == 0U ||
        frames > NP2_AUDIO86_QUANTUM_FRAMES ||
        state->pcm86.pcm.realbuf < 4096) {
        if (state != NULL) {
            state->pcm86.underrun = 1U;
        }
        return -1;
    }
    {
        if (state->pcm86.pcm.realbuf < state->pcm86.fifo_min) {
            state->pcm86.fifo_min = state->pcm86.pcm.realbuf;
        }
        if (state->pcm86.pcm.realbuf > state->pcm86.fifo_max) {
            state->pcm86.fifo_max = state->pcm86.pcm.realbuf;
        }
        return render_span(state, mix, frames, result, 0);
    }
}

int np2audio86_render_quantum(struct np2audio86_render_state *state,
                              const struct np2audio86_event *plan,
                              size_t plan_count, uint8_t *canonical,
                              size_t canonical_bytes,
                              struct np2audio86_fixture_result *result)
{
    size_t offset = 0U;
    size_t i;
    struct np2opngen_pcm_stats stats;
    if (state == NULL || plan == NULL || canonical == NULL || result == NULL ||
        plan_count == 0U || canonical_bytes < NP2_AUDIO86_QUANTUM_FRAMES * 4U) {
        return -1;
    }
    memset(state->mix_scratch, 0, sizeof(state->mix_scratch));
    while (offset < NP2_AUDIO86_QUANTUM_FRAMES) {
        size_t next = NP2_AUDIO86_QUANTUM_FRAMES;
        for (i = state->next_event; i < plan_count; ++i) {
            if (plan[i].frame_timestamp < state->rendered_frames) {
                return -1;
            }
            if (plan[i].frame_timestamp < state->rendered_frames +
                                             (NP2_AUDIO86_QUANTUM_FRAMES - offset)) {
                next = (size_t)(plan[i].frame_timestamp -
                                state->rendered_frames) + offset;
                break;
            }
        }
        if (next > offset && np2audio86_render_span(
                                  state, state->mix_scratch + offset * 2U,
                                  next - offset, result) != 0) {
            return -1;
        }
        offset = next;
        while (state->next_event < plan_count &&
               plan[state->next_event].frame_timestamp == state->rendered_frames) {
            if (plan[state->next_event].opcode !=
                NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
                if (np2audio86_render_apply_event(
                        state, &plan[state->next_event]) != 0) {
                    return -1;
                }
            }
            ++state->next_event;
        }
    }
    if (np2opngen_pcm_canonicalize_s16le(
            state->mix_scratch, NP2_AUDIO86_QUANTUM_FRAMES,
            NP2_AUDIO86_CHANNELS, canonical, canonical_bytes, &stats) != 0) {
        return -1;
    }
    if (stats.s32_abs_peak > result->mix_peak_abs) {
        result->mix_peak_abs = stats.s32_abs_peak;
    }
    result->clamped_samples += stats.clip_samples;
    return 0;
}

void np2audio86_fixture_hash_source(struct np2audio86_fixture_result *result,
                                    const uint8_t *source)
{
    result->source_crc32 = np2_crc32_iso_hdlc(source,
                                              NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES);
    {
        np2_sha256_context sha;
        np2_sha256_init(&sha);
        np2_sha256_update(&sha, source, NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES);
        np2_sha256_final(&sha, result->source_sha256);
    }
}

int np2audio86_fixture_render(struct np2audio86_fixture_result *result)
{
    struct audio86_state state;
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U];
    np2_sha256_context pcm_sha;
    uint32_t pcm_crc = np2_crc32_iso_hdlc_init();
    uint32_t quantum;

    if (result == NULL) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    memset(&state, 0, sizeof(state));
    configure_fm(&state.fm);
    configure_psg(&state.psg);
    configure_rhythm(&state);
    configure_pcm86(&state.pcm86, NULL);
    np2audio86_fixture_hash_control(result);
    np2audio86_fixture_hash_source(result, state.pcm86.source);
    np2_sha256_init(&pcm_sha);

    for (quantum = 0U; quantum < NP2_AUDIO86_QUANTA; ++quantum) {
        SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
        size_t offset = 0U;
        memset(mix, 0, sizeof(mix));
        while (offset < NP2_AUDIO86_QUANTUM_FRAMES) {
            size_t next = NP2_AUDIO86_QUANTUM_FRAMES;
            size_t i;
            for (i = state.next_event;
                 i < sizeof(control_events) / sizeof(control_events[0]); ++i) {
                const uint64_t frame = control_events[i].frame;
                if (frame < state.rendered_frames) {
                    result->sequence_error = 1U;
                    return -1;
                }
                if (frame < state.rendered_frames +
                                (NP2_AUDIO86_QUANTUM_FRAMES - offset)) {
                    next = (size_t)(frame - state.rendered_frames) + offset;
                    break;
                }
            }
            if (next > offset && render_span(&state, mix + offset * 2U,
                                              next - offset, result, 1) != 0) {
                return -1;
            }
            offset = next;
            while (state.next_event < sizeof(control_events) /
                                             sizeof(control_events[0]) &&
                   control_events[state.next_event].frame ==
                       state.rendered_frames) {
                if (apply_event(&state, &control_events[state.next_event]) != 0) {
                    result->sequence_error = 1U;
                    return -1;
                }
                ++state.next_event;
            }
            if (next == NP2_AUDIO86_QUANTUM_FRAMES && offset == next) {
                break;
            }
        }
        {
            struct np2opngen_pcm_stats stats;
            if (np2opngen_pcm_canonicalize_s16le(
                    mix, NP2_AUDIO86_QUANTUM_FRAMES, NP2_AUDIO86_CHANNELS,
                    canonical, sizeof(canonical), &stats) != 0) {
                return -1;
            }
            if (stats.s32_abs_peak > result->mix_peak_abs) {
                result->mix_peak_abs = stats.s32_abs_peak;
            }
            result->clamped_samples += stats.clip_samples;
            pcm_crc = np2_crc32_iso_hdlc_update(pcm_crc, canonical,
                                                sizeof(canonical));
            np2_sha256_update(&pcm_sha, canonical, sizeof(canonical));
        }
    }
    result->pcm_crc32 = np2_crc32_iso_hdlc_finish(pcm_crc);
    np2_sha256_final(&pcm_sha, result->pcm_sha256);
    result->frames = NP2_AUDIO86_DURATION_FRAMES;
    result->bytes = NP2_AUDIO86_PCM_BYTES;
    result->quanta = NP2_AUDIO86_QUANTA;
    result->pcm86_bytes_supplied = state.pcm86.supplied;
    result->pcm86_bytes_consumed = state.pcm86.supplied -
                                   (uint64_t)state.pcm86.pcm.realbuf;
    result->pcm86_refills = state.pcm86.refills;
    result->pcm86_fifo_min = state.pcm86.fifo_min;
    result->pcm86_fifo_max = state.pcm86.fifo_max;
    result->pcm86_fifo_underrun = state.pcm86.underrun;
    if (state.next_event != sizeof(control_events) /
                                sizeof(control_events[0])) {
        result->sequence_error = 1U;
    }
    return 0;
}

static int digest_equal(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, NP2_SHA256_DIGEST_SIZE) == 0;
}

int np2audio86_fixture_control_matches_golden(
    const struct np2audio86_fixture_result *result)
{
    return result != NULL &&
           result->control_events == NP2_AUDIO86_GOLDEN_CONTROL_EVENTS &&
           result->mid_quantum_events == NP2_AUDIO86_GOLDEN_MID_QUANTUM_EVENTS &&
           result->control_crc32 == NP2_AUDIO86_GOLDEN_CONTROL_CRC32 &&
           digest_equal(result->control_sha256,
                        np2audio86_golden_control_sha256);
}

int np2audio86_fixture_source_matches_golden(
    const struct np2audio86_fixture_result *result)
{
    return result != NULL &&
           result->source_crc32 == NP2_AUDIO86_GOLDEN_SOURCE_CRC32 &&
           digest_equal(result->source_sha256, np2audio86_golden_source_sha256);
}

int np2audio86_fixture_matches_golden(
    const struct np2audio86_fixture_result *result)
{
    if (result == NULL || result->frames != NP2_AUDIO86_DURATION_FRAMES ||
        result->bytes != NP2_AUDIO86_PCM_BYTES ||
        result->quanta != NP2_AUDIO86_QUANTA ||
        result->control_events != NP2_AUDIO86_GOLDEN_CONTROL_EVENTS ||
        result->mid_quantum_events != NP2_AUDIO86_GOLDEN_MID_QUANTUM_EVENTS ||
        result->control_crc32 != NP2_AUDIO86_GOLDEN_CONTROL_CRC32 ||
        result->source_crc32 != NP2_AUDIO86_GOLDEN_SOURCE_CRC32 ||
        result->pcm_crc32 != NP2_AUDIO86_GOLDEN_PCM_CRC32 ||
        !digest_equal(result->control_sha256, np2audio86_golden_control_sha256) ||
        !digest_equal(result->source_sha256, np2audio86_golden_source_sha256) ||
        !digest_equal(result->pcm_sha256, np2audio86_golden_pcm_sha256)) {
        return 0;
    }
    return 1;
}

static void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    unsigned i;
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

void np2audio86_fixture_print_result(
    const struct np2audio86_fixture_result *result)
{
    printf("AUDIO86_SYNC_CONFIG rate=%u quantum_frames=%u duration_frames=%u quanta=%u\n",
           NP2_AUDIO86_RATE_HZ, NP2_AUDIO86_QUANTUM_FRAMES,
           NP2_AUDIO86_DURATION_FRAMES, NP2_AUDIO86_QUANTA);
    printf("AUDIO86_SYNC_LOAD fm_channels=%u fm_operators=%u psg_channels=%u psg_noise=1 psg_envelope=1 rhythm_tracks=%u pcm86_source_rate=%u pcm86_channels=%u pcm86_bits=%u pcm86_dactrl=0x30\n",
           AUDIO86_FM_CHANNELS, AUDIO86_FM_OPERATORS, AUDIO86_PSG_CHANNELS,
           AUDIO86_RHYTHM_TRACKS, NP2_AUDIO86_PCM86_SOURCE_RATE_HZ,
           NP2_AUDIO86_PCM86_SOURCE_CHANNELS, NP2_AUDIO86_PCM86_SOURCE_BITS);
    printf("AUDIO86_SYNC_CONTROL_IDENTITY events=%u crc32=0x%08" PRIx32 " sha256=",
           result->control_events, result->control_crc32);
    print_sha256(result->control_sha256);
    printf(" mid_quantum_events=%u\n", result->mid_quantum_events);
    printf("AUDIO86_SYNC_PCM86_SOURCE_IDENTITY period_frames=%u bytes=%u crc32=0x%08" PRIx32 " sha256=",
           NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES,
           NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES, result->source_crc32);
    print_sha256(result->source_sha256);
    printf("\n");
    printf("AUDIO86_SYNC_PCM86_FEED supplied=%" PRIu64 " consumed=%" PRIu64 " refills=%u fifo_min=%" PRId32 " fifo_max=%" PRId32 "\n",
           result->pcm86_bytes_supplied, result->pcm86_bytes_consumed,
           result->pcm86_refills, result->pcm86_fifo_min,
           result->pcm86_fifo_max);
    printf("AUDIO86_SYNC_ACTIVITY fm=%s psg=%s rhythm=%s pcm86=%s\n",
           result->fm_contribution ? "PASS" : "FAIL",
           result->psg_contribution ? "PASS" : "FAIL",
           result->rhythm_contribution ? "PASS" : "FAIL",
           result->pcm86_contribution ? "PASS" : "FAIL");
    printf("AUDIO86_SYNC_PCM_IDENTITY frames=%" PRIu64 " bytes=%" PRIu64 " quanta=%" PRIu64 " crc32=0x%08" PRIx32 " sha256=",
           result->frames, result->bytes, result->quanta, result->pcm_crc32);
    print_sha256(result->pcm_sha256);
    printf("\n");
    printf("AUDIO86_SYNC_AMPLITUDE peak_abs=%" PRIu64 " clamped_samples=%" PRIu64 "\n",
           result->mix_peak_abs, result->clamped_samples);
    printf("AUDIO86_SYNC_SAFETY pcm86_fifo_underrun=%u sequence_error=%u arithmetic_error=%u\n",
           result->pcm86_fifo_underrun, result->sequence_error,
           result->arithmetic_error);
}

void np2audio86_event_ring_init(struct np2audio86_event_ring *ring)
{
    if (ring == NULL) {
        return;
    }
    memset(ring->slots, 0, sizeof(ring->slots));
    atomic_init(&ring->head, 0U);
    atomic_init(&ring->tail, 0U);
}

uint32_t np2audio86_event_ring_occupancy(
    const struct np2audio86_event_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL) {
        return 0U;
    }
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

int np2audio86_event_ring_peek(
    const struct np2audio86_event_ring *ring,
    const struct np2audio86_event **event)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || event == NULL) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    *event = NULL;
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (head == tail) {
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    }
    *event = &ring->slots[tail & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)];
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_event_ring_consume(struct np2audio86_event_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (head == tail) {
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    }
    atomic_store_explicit(&ring->tail, tail + 1U, memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_event_ring_enqueue(struct np2audio86_event_ring *ring,
                                   const struct np2audio86_event *event)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || event == NULL) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (head - tail == NP2_AUDIO86_ASYNC_EVENT_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_FULL;
    }
    ring->slots[head & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)] = *event;
#if defined(NP2AUDIO86_GUEST_TEST) && \
    defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (np2audio86_guest_async_hardening_cutpoint(
            NP2_AUDIO86_ASYNC_CP_EVENT_SLOT_BEFORE_HEAD, head, tail,
            0U, 0U, event->sequence) != 0) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if ((head & (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)) ==
        NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U) {
        np2audio86_guest_async_hardening_event_wrap(head);
    }
#endif
    atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_event_ring_dequeue(struct np2audio86_event_ring *ring,
                                   struct np2audio86_event *event)
{
    const struct np2audio86_event *slot;
    int status;
    if (event == NULL) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    status = np2audio86_event_ring_peek(ring, &slot);
    if (status != NP2_AUDIO86_TRANSPORT_OK) {
        return status;
    }
    *event = *slot;
    return np2audio86_event_ring_consume(ring);
}

void np2audio86_byte_ring_init(struct np2audio86_byte_ring *ring)
{
    if (ring == NULL) {
        return;
    }
    memset(ring->bytes, 0, sizeof(ring->bytes));
    atomic_init(&ring->head, 0U);
    atomic_init(&ring->tail, 0U);
}

uint32_t np2audio86_byte_ring_occupancy(
    const struct np2audio86_byte_ring *ring)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL) {
        return 0U;
    }
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

int np2audio86_byte_ring_copy(const struct np2audio86_byte_ring *ring,
                              uint8_t *bytes, size_t count)
{
    uint32_t head;
    uint32_t tail;
    size_t first;
    if (ring == NULL || (count != 0U && bytes == NULL) ||
        count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (count > head - tail) {
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    }
    first = NP2_AUDIO86_ASYNC_BYTE_CAPACITY -
            (tail & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U));
    if (first > count) {
        first = count;
    }
    if (first != 0U) {
        memcpy(bytes, ring->bytes +
                         (tail & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)),
               first);
    }
    if (count > first) {
        memcpy(bytes + first, ring->bytes, count - first);
    }
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_consume(struct np2audio86_byte_ring *ring,
                                 size_t count)
{
    uint32_t head;
    uint32_t tail;
    if (ring == NULL || count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    head = atomic_load_explicit(&ring->head, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (count > head - tail) {
        return NP2_AUDIO86_TRANSPORT_EMPTY;
    }
    atomic_store_explicit(&ring->tail, tail + (uint32_t)count,
                          memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_push(struct np2audio86_byte_ring *ring,
                              const uint8_t *bytes, size_t count)
{
    uint32_t head;
    uint32_t tail;
    size_t first;
    if (ring == NULL || (count != 0U && bytes == NULL) ||
        count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_ARGUMENT;
    }
    head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head - tail > NP2_AUDIO86_ASYNC_BYTE_CAPACITY) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
    if (count > NP2_AUDIO86_ASYNC_BYTE_CAPACITY - (head - tail)) {
        return NP2_AUDIO86_TRANSPORT_FULL;
    }
    first = NP2_AUDIO86_ASYNC_BYTE_CAPACITY -
            (head & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U));
    if (first > count) {
        first = count;
    }
    if (first != 0U) {
        memcpy(ring->bytes + (head & (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)),
               bytes, first);
    }
    if (count > first) {
        memcpy(ring->bytes, bytes + first, count - first);
    }
#if defined(NP2AUDIO86_GUEST_TEST) && \
    defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
    if (np2audio86_guest_async_hardening_cutpoint(
            NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_HEAD, 0U, 0U, head, tail,
            (uint64_t)count) != 0) {
        return NP2_AUDIO86_TRANSPORT_INVARIANT;
    }
#endif
    atomic_store_explicit(&ring->head, head + (uint32_t)count,
                          memory_order_release);
    return NP2_AUDIO86_TRANSPORT_OK;
}

int np2audio86_byte_ring_pop(struct np2audio86_byte_ring *ring,
                             uint8_t *bytes, size_t count)
{
    int status = np2audio86_byte_ring_copy(ring, bytes, count);
    if (status != NP2_AUDIO86_TRANSPORT_OK) {
        return status;
    }
    return np2audio86_byte_ring_consume(ring, count);
}

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
int np2audio86_async_test_byte_copy(
    const struct np2audio86_byte_ring *ring, uint8_t *bytes, size_t count)
{
    return np2audio86_byte_ring_copy(ring, bytes, count);
}

int np2audio86_async_test_byte_consume(struct np2audio86_byte_ring *ring,
                                       size_t count)
{
    return np2audio86_byte_ring_consume(ring, count);
}
#endif

/* These are the exact long (38-quantum) gaps observed by the unchanged
 * synchronous feed_pcm86() implementation.  The remaining gaps are 37
 * quanta.  Keeping this as a compact immutable fixture plan avoids a second
 * PCM86/resampler model in the producer. */
static const uint16_t async_long_gap_event_numbers[] = {
    5U, 10U, 16U, 21U, 27U, 32U, 38U, 43U, 49U, 54U,
    59U, 65U, 70U, 76U, 81U, 87U, 92U, 98U, 103U, 109U,
    114U, 119U, 125U, 130U, 136U, 141U, 147U, 152U, 158U, 163U,
    168U, 174U, 179U, 185U, 190U, 196U, 201U, 207U, 212U, 217U,
    223U, 228U, 234U, 239U, 245U, 250U, 256U, 261U, 267U, 272U,
    277U, 283U, 288U, 294U, 299U, 305U, 310U, 316U, 321U,
};

static int async_is_long_gap(unsigned event_number)
{
    size_t i;
    for (i = 0U; i < sizeof(async_long_gap_event_numbers) /
                         sizeof(async_long_gap_event_numbers[0]); ++i) {
        if (async_long_gap_event_numbers[i] == event_number) {
            return 1;
        }
    }
    return 0;
}

static uint64_t async_refill_frame(unsigned run_index)
{
    uint64_t quantum = 0U;
    unsigned i;
    if (run_index == 0U) {
        return 0U;
    }
    if (run_index == 1U) {
        return 33U * NP2_AUDIO86_QUANTUM_FRAMES;
    }
    quantum = 33U;
    for (i = 2U; i <= run_index; ++i) {
        quantum += async_is_long_gap(i + 1U) ? 38U : 37U;
    }
    return quantum * NP2_AUDIO86_QUANTUM_FRAMES;
}

static uint32_t async_pack_key(uint8_t channel, uint8_t value)
{
    return (uint32_t)channel | ((uint32_t)value << 8U);
}

static uint32_t async_pack_psg(uint8_t reg, uint8_t value)
{
    return (uint32_t)reg | ((uint32_t)value << 8U);
}

int np2audio86_async_build_plan(struct np2audio86_event *plan, size_t *count)
{
    size_t control = 0U;
    unsigned refill = 0U;
    uint64_t sequence = 0U;
    size_t output = 0U;
    const size_t control_count = sizeof(control_events) /
                                 sizeof(control_events[0]);
    if (plan == NULL || count == NULL) {
        return -1;
    }
    while (control < control_count || refill < 323U) {
        uint64_t control_frame = control < control_count
                                     ? control_events[control].frame
                                     : UINT64_MAX;
        uint64_t refill_frame = refill < 323U
                                    ? async_refill_frame(refill)
                                    : UINT64_MAX;
        uint64_t frame = control_frame < refill_frame ? control_frame
                                                        : refill_frame;
        if (frame >= NP2_AUDIO86_DURATION_FRAMES || output >=
                                                   NP2_AUDIO86_ASYNC_MAX_EVENTS) {
            return -1;
        }
        while (control < control_count &&
               control_events[control].frame == frame) {
            struct np2audio86_event *event = &plan[output++];
            event->frame_timestamp = frame;
            event->sequence = sequence++;
            if (control_events[control].opcode == AUDIO86_EVENT_PSG_WRITE) {
                event->opcode = NP2_AUDIO86_EVENT_PSG_REGISTER;
                event->payload = async_pack_psg(
                    (uint8_t)control_events[control].auxiliary,
                    (uint8_t)control_events[control].value);
            } else {
                event->opcode = NP2_AUDIO86_EVENT_FM_KEY;
                event->payload = async_pack_key(
                    control_events[control].target,
                    (uint8_t)control_events[control].value);
            }
            ++control;
        }
        while (refill < 323U && async_refill_frame(refill) == frame) {
            struct np2audio86_event *event = &plan[output++];
            event->frame_timestamp = frame;
            event->sequence = sequence++;
            event->opcode = NP2_AUDIO86_EVENT_PCM86_DATA_RUN;
            event->payload = AUDIO86_PCM86_REFILL_BYTES;
            ++refill;
        }
    }
    if (control != control_count || refill != 323U || output != 333U ||
        sequence != output) {
        return -1;
    }
    *count = output;
    return 0;
}

int np2audio86_async_validate_plan(const struct np2audio86_event *plan,
                                   size_t count)
{
    size_t i;
    uint64_t bytes = 0U;
    if (plan == NULL || count != NP2_AUDIO86_ASYNC_MAX_EVENTS) {
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        const struct np2audio86_event *event = &plan[i];
        if (event->sequence != (uint64_t)i ||
            event->frame_timestamp >= NP2_AUDIO86_DURATION_FRAMES ||
            (i != 0U && event->frame_timestamp < plan[i - 1U].frame_timestamp)) {
            return -1;
        }
        if (event->opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN) {
            if (event->payload == 0U ||
                event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
                (event->payload & 3U) != 0U ||
                bytes > UINT64_MAX - event->payload) {
                return -1;
            }
            bytes += event->payload;
        } else if (event->opcode != NP2_AUDIO86_EVENT_FM_KEY &&
                   event->opcode != NP2_AUDIO86_EVENT_PSG_REGISTER) {
            return -1;
        }
    }
    return bytes == UINT64_C(10584064) ? 0 : -1;
}

#ifdef NP2_AUDIO86_ASYNC_HOST

struct np2audio86_async_context {
    enum np2audio86_async_mode mode;
    struct np2audio86_event_ring events;
    struct np2audio86_byte_ring pcm_bytes;
    struct np2audio86_event plan[NP2_AUDIO86_ASYNC_MAX_EVENTS];
    size_t plan_count;
    uint8_t producer_source[NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES];
    uint8_t worker_run[NP2_AUDIO86_ASYNC_MAX_DATA_RUN];
    struct audio86_state worker_state;
    struct np2audio86_async_result *result;
    _Atomic uint32_t first_error;
    _Atomic uint64_t committed_through_frame;
    _Atomic bool producer_done;
    _Atomic bool producer_prefill_ready;
    _Atomic bool worker_done;
    _Atomic uint32_t published_event_count;
    uint64_t producer_last_watermark;
    uint64_t worker_last_watermark;
    uint32_t producer_event_index;
    uint64_t producer_source_frame;
    uint32_t producer_data_runs;
    uint32_t worker_next_sequence;
    bool worker_started;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    bool worker_error_attempted;
#endif
    uint32_t transport_crc32;
    np2_sha256_context transport_sha;
    uint64_t transport_count;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    struct np2audio86_async_test_control *test_control;
#endif
};

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)

void np2audio86_async_test_control_init(
    struct np2audio86_async_test_control *control)
{
    if (control == NULL) {
        return;
    }
    memset(control, 0, sizeof(*control));
    atomic_init(&control->gate_reached, false);
    atomic_init(&control->gate_release, false);
    atomic_init(&control->fault_injected, false);
}

static int async_test_fault(const struct np2audio86_async_context *context,
                            enum np2audio86_async_test_fault fault)
{
    return context != NULL && context->test_control != NULL &&
           context->test_control->fault == (uint32_t)fault;
}

static int async_test_take_fault(struct np2audio86_async_context *context,
                                 enum np2audio86_async_test_fault fault)
{
    bool expected = false;
    if (!async_test_fault(context, fault)) {
        return 0;
    }
    return atomic_compare_exchange_strong_explicit(
               &context->test_control->fault_injected, &expected, true,
               memory_order_acq_rel, memory_order_acquire)
               ? 1
               : 0;
}

static int async_test_at_target(const struct np2audio86_async_context *context,
                                size_t event_index)
{
    return context != NULL && context->test_control != NULL &&
           event_index == context->test_control->target_event;
}

static void async_test_gate(struct np2audio86_async_context *context,
                            enum np2audio86_async_test_gate gate,
                            size_t event_index)
{
    bool expected = false;
    struct np2audio86_async_test_control *control;
    if (context == NULL || context->test_control == NULL ||
        context->test_control->gate != (uint32_t)gate ||
        (context->test_control->target_event != UINT32_MAX &&
         event_index != context->test_control->target_event)) {
        return;
    }
    control = context->test_control;
    if (!atomic_compare_exchange_strong_explicit(
            &control->gate_reached, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }
    while (!atomic_load_explicit(&control->gate_release,
                                 memory_order_acquire)) {
        sched_yield();
    }
}

static int async_test_yield_enabled(
    const struct np2audio86_async_context *context, uint32_t flag)
{
    return context != NULL && context->test_control != NULL &&
           (context->test_control->yield_flags & flag) != 0U;
}

static void async_test_mark_injected(struct np2audio86_async_context *context)
{
    if (context != NULL && context->test_control != NULL) {
        atomic_store_explicit(&context->test_control->fault_injected, true,
                              memory_order_release);
    }
}

#endif

static void async_fail(struct np2audio86_async_context *context,
                       enum np2audio86_async_error error)
{
    uint32_t expected = NP2_AUDIO86_ASYNC_ERROR_NONE;
    if (context == NULL || error == NP2_AUDIO86_ASYNC_ERROR_NONE) {
        return;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &context->first_error, &expected, (uint32_t)error,
        memory_order_acq_rel, memory_order_acquire);
}

static int async_error(const struct np2audio86_async_context *context)
{
    return context == NULL
               ? NP2_AUDIO86_ASYNC_ERROR_ARGUMENT
               : (int)atomic_load_explicit(&context->first_error,
                                           memory_order_acquire);
}

static void async_yield(struct np2audio86_async_context *context, bool producer)
{
    if (producer) {
        ++context->result->producer_yield_count;
    } else {
        ++context->result->worker_yield_count;
    }
    sched_yield();
}

static int async_should_yield(const struct np2audio86_async_context *context,
                              bool producer, uint64_t counter)
{
    switch (context->mode) {
    case NP2_AUDIO86_ASYNC_PRODUCER_FAST_WORKER_YIELD:
        return !producer;
    case NP2_AUDIO86_ASYNC_PRODUCER_YIELD_WORKER_FAST:
        return producer;
    case NP2_AUDIO86_ASYNC_DETERMINISTIC_ALTERNATING:
        return producer ? (counter % 3U == 0U) : (counter % 5U == 0U);
    case NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE:
        return !producer;
    }
    return 0;
}

static void async_update_fifo_stats(struct audio86_pcm86_feed *feed)
{
    if (feed->pcm.realbuf < feed->fifo_min) {
        feed->fifo_min = feed->pcm.realbuf;
    }
    if (feed->pcm.realbuf > feed->fifo_max) {
        feed->fifo_max = feed->pcm.realbuf;
    }
}

static int async_copy_run(struct np2audio86_async_context *context,
                          uint32_t count)
{
    struct audio86_pcm86_feed *feed = &context->worker_state.pcm86;
    int transport_status;
    uint32_t remaining;
    uint32_t destination;
    uint32_t source_offset = 0U;
    if (count == 0U || count > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
        (count & 3U) != 0U || feed->pcm.realbuf < 0 ||
        feed->pcm.realbuf > PCM86_BUFSIZE - (SINT32)count) {
        return -1;
    }
    transport_status = np2audio86_byte_ring_copy(
        &context->pcm_bytes, context->worker_run, count);
    if (transport_status != NP2_AUDIO86_TRANSPORT_OK) {
        return transport_status == NP2_AUDIO86_TRANSPORT_EMPTY ? -2 : -3;
    }
    remaining = count;
    destination = feed->pcm.wrtpos & PCM86_BUFMSK;
    while (remaining != 0U) {
        uint32_t destination_part = PCM86_BUFSIZE - destination;
        uint32_t part = remaining < destination_part ? remaining
                                                       : destination_part;
        memcpy(feed->pcm.buffer + destination,
               context->worker_run + source_offset, part);
        destination = (destination + part) & PCM86_BUFMSK;
        source_offset += part;
        remaining -= part;
    }
    feed->pcm.wrtpos = (feed->pcm.wrtpos + count) & PCM86_BUFMSK;
    feed->pcm.realbuf += (SINT32)count;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    if (async_test_yield_enabled(context,
                                 NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_COPY)) {
        async_yield(context, false);
    }
    async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_BYTE_COPY,
                    context->worker_next_sequence);
#endif
    transport_status = np2audio86_byte_ring_consume(&context->pcm_bytes, count);
    if (transport_status != NP2_AUDIO86_TRANSPORT_OK) {
        return -3;
    }
    feed->supplied += count;
    ++feed->refills;
    async_update_fifo_stats(feed);
    context->result->pcm86_byte_pop_bytes += count;
    ++context->result->pcm86_data_run_count;
    return 0;
}

static void async_serialize_event(const struct np2audio86_event *event,
                                  uint8_t record[24])
{
    memset(record, 0, 24U);
    put_le64(record, event->frame_timestamp);
    put_le64(record + 8U, event->sequence);
    put_le32(record + 16U, event->opcode);
    put_le32(record + 20U, event->payload);
}

static void async_trace_event(struct np2audio86_async_context *context,
                              const struct np2audio86_event *event)
{
    uint8_t record[24];
    async_serialize_event(event, record);
    context->transport_crc32 = np2_crc32_iso_hdlc_update(
        context->transport_crc32, record, sizeof(record));
    np2_sha256_update(&context->transport_sha, record, sizeof(record));
    ++context->transport_count;
}

static int async_apply_event(struct np2audio86_async_context *context,
                             const struct np2audio86_event *event)
{
    uint8_t channel;
    uint8_t value;
    if (event == NULL || event->sequence != context->worker_next_sequence) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_SEQUENCE);
        return -1;
    }
    if (event->frame_timestamp >= NP2_AUDIO86_DURATION_FRAMES ||
        event->frame_timestamp != context->worker_state.rendered_frames) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP);
        return -1;
    }
    switch (event->opcode) {
    case NP2_AUDIO86_EVENT_FM_KEY:
        channel = (uint8_t)(event->payload & 0xffU);
        value = (uint8_t)((event->payload >> 8U) & 0xffU);
        if (channel > 5U || (event->payload >> 16U) != 0U) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PAYLOAD);
            return -1;
        }
        opngen_keyon(&context->worker_state.fm, channel, value);
        break;
    case NP2_AUDIO86_EVENT_PSG_REGISTER:
        channel = (uint8_t)(event->payload & 0xffU);
        value = (uint8_t)((event->payload >> 8U) & 0xffU);
        if (channel > 13U || (event->payload >> 16U) != 0U) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PAYLOAD);
            return -1;
        }
        psggen_setreg(&context->worker_state.psg, channel, value);
        break;
    case NP2_AUDIO86_EVENT_PCM86_DATA_RUN:
        {
            int copy_status = 0;
            if (event->payload == 0U ||
                event->payload > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
                (event->payload & 3U) != 0U) {
                copy_status = -1;
            } else if (context->worker_state.pcm86.pcm.realbuf >= 4096) {
                copy_status = -4;
            } else {
                copy_status = async_copy_run(context, event->payload);
            }
            if (copy_status != 0) {
                async_fail(context,
                           event->payload == 0U ||
                                   event->payload >
                                       NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
                                   (event->payload & 3U) != 0U
                               ? NP2_AUDIO86_ASYNC_ERROR_DATA_LENGTH
                               : copy_status == -2
                                     ? NP2_AUDIO86_ASYNC_ERROR_DATA_AVAILABILITY
                                     : copy_status == -3
                                           ? NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT
                                           : NP2_AUDIO86_ASYNC_ERROR_PCM86_UNDERRUN);
                return -1;
            }
        }
        break;
    default:
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_OPCODE);
        return -1;
    }
    async_trace_event(context, event);
    ++context->worker_next_sequence;
    return 0;
}

static int async_render_span(struct np2audio86_async_context *context,
                             SINT32 *mix, size_t frames,
                             struct np2audio86_fixture_result *result)
{
    struct audio86_pcm86_feed *feed = &context->worker_state.pcm86;
    if (feed->pcm.realbuf < 4096) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PCM86_UNDERRUN);
        return -1;
    }
    async_update_fifo_stats(feed);
    return render_span(&context->worker_state, mix, frames, result, 0);
}

static int async_enqueue_event(struct np2audio86_async_context *context,
                               const struct np2audio86_event *event)
{
    for (;;) {
        int status;
        if (async_error(context) != NP2_AUDIO86_ASYNC_ERROR_NONE) {
            return -1;
        }
        status = np2audio86_event_ring_enqueue(&context->events, event);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            ++context->result->event_push_count;
            {
                uint32_t occupancy = np2audio86_event_ring_occupancy(
                    &context->events);
                if (occupancy > context->result->event_high_water) {
                    context->result->event_high_water = occupancy;
                }
            }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (async_test_yield_enabled(
                    context, NP2_AUDIO86_TEST_YIELD_AFTER_EVENT_PUBLICATION)) {
                async_yield(context, true);
            }
            async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_EVENT_PUBLICATION,
                            context->producer_event_index);
#endif
            return 0;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
            return -1;
        }
        ++context->result->event_full_wait_count;
        async_yield(context, true);
    }
}

static int async_enqueue_bytes(struct np2audio86_async_context *context,
                               const uint8_t *bytes, uint32_t count)
{
    for (;;) {
        int status;
        if (async_error(context) != NP2_AUDIO86_ASYNC_ERROR_NONE) {
            return -1;
        }
        status = np2audio86_byte_ring_push(&context->pcm_bytes, bytes, count);
        if (status == NP2_AUDIO86_TRANSPORT_OK) {
            context->result->pcm86_byte_push_bytes += count;
            {
                uint32_t occupancy = np2audio86_byte_ring_occupancy(
                    &context->pcm_bytes);
                if (occupancy > context->result->pcm86_byte_high_water) {
                    context->result->pcm86_byte_high_water = occupancy;
                }
            }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (async_test_yield_enabled(
                    context, NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_PUBLICATION)) {
                async_yield(context, true);
            }
            async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_BYTE_PUBLICATION,
                            context->producer_event_index);
#endif
            return 0;
        }
        if (status != NP2_AUDIO86_TRANSPORT_FULL) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
            return -1;
        }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
        async_test_gate(context, NP2_AUDIO86_TEST_GATE_PRODUCER_BYTE_FULL,
                        context->producer_event_index);
#endif
        ++context->result->pcm86_byte_full_wait_count;
        async_yield(context, true);
    }
}

static int async_publish_watermark(struct np2audio86_async_context *context,
                                   uint64_t next)
{
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    const uint64_t publication = context->result->watermark_publish_count;
    if (async_test_fault(context,
                         NP2_AUDIO86_TEST_FAULT_WATERMARK_REGRESSION) &&
        publication == (uint64_t)context->test_control->target_event) {
        next = context->producer_last_watermark == 0U
                   ? 0U
                   : context->producer_last_watermark - 240U;
        async_test_mark_injected(context);
    }
    if (async_test_fault(context,
                         NP2_AUDIO86_TEST_FAULT_WATERMARK_OVER_END) &&
        publication == (uint64_t)context->test_control->target_event) {
        next = NP2_AUDIO86_DURATION_FRAMES + 1U;
        async_test_mark_injected(context);
    }
#endif
    if (next < context->producer_last_watermark ||
        next > NP2_AUDIO86_DURATION_FRAMES) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WATERMARK);
        return -1;
    }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    if (async_test_yield_enabled(context,
                                 NP2_AUDIO86_TEST_YIELD_BEFORE_WATERMARK)) {
        async_yield(context, true);
    }
    async_test_gate(context, NP2_AUDIO86_TEST_GATE_BEFORE_WATERMARK,
                    (size_t)publication);
#endif
    atomic_store_explicit(&context->committed_through_frame, next,
                          memory_order_release);
    context->producer_last_watermark = next;
    ++context->result->watermark_publish_count;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    if (async_test_yield_enabled(context,
                                 NP2_AUDIO86_TEST_YIELD_AFTER_WATERMARK)) {
        async_yield(context, true);
    }
    async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_WATERMARK,
                    (size_t)publication);
#endif
    return 0;
}

static void *async_producer_thread(void *opaque)
{
    struct np2audio86_async_context *context = opaque;
    size_t i = 0U;
    uint64_t yield_counter = 0U;
    uint32_t published_count = 0U;
    if (context == NULL) {
        return NULL;
    }
    build_pcm86_source(context->producer_source);
    for (i = 0U; i < context->plan_count; ) {
        const uint64_t frame = context->plan[i].frame_timestamp;
        do {
            struct np2audio86_event event = context->plan[i];
            const int is_data = event.opcode == NP2_AUDIO86_EVENT_PCM86_DATA_RUN;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            const int is_cut = is_data &&
                               (async_test_fault(
                                    context, NP2_AUDIO86_TEST_FAULT_CUT_BEFORE_BYTES) ||
                                async_test_fault(
                                    context, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_BYTES) ||
                                async_test_fault(
                                    context, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_EVENT) ||
                                async_test_fault(
                                    context, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_WATERMARK));
            if (async_test_fault(context,
                                NP2_AUDIO86_TEST_FAULT_DUPLICATE_SEQUENCE) &&
                async_test_at_target(context, i)) {
                event.sequence = i == 0U ? 1U : context->plan[i - 1U].sequence;
                async_test_mark_injected(context);
            } else if (async_test_fault(
                           context, NP2_AUDIO86_TEST_FAULT_SKIPPED_SEQUENCE) &&
                       async_test_at_target(context, i)) {
                event.sequence += 1U;
                async_test_mark_injected(context);
            } else if (async_test_fault(
                           context, NP2_AUDIO86_TEST_FAULT_TIMESTAMP_BEHIND) &&
                       async_test_at_target(context, i)) {
                event.frame_timestamp = 0U;
                async_test_mark_injected(context);
            } else if (async_test_fault(
                           context, NP2_AUDIO86_TEST_FAULT_TIMESTAMP_END) &&
                       async_test_at_target(context, i)) {
                event.frame_timestamp = NP2_AUDIO86_DURATION_FRAMES;
                async_test_mark_injected(context);
            } else if (async_test_fault(
                           context, NP2_AUDIO86_TEST_FAULT_INVALID_OPCODE) &&
                       async_test_at_target(context, i)) {
                event.opcode = 0x7fU;
                async_test_mark_injected(context);
            } else if (async_test_fault(
                           context, NP2_AUDIO86_TEST_FAULT_RESERVED_OPCODE) &&
                       async_test_at_target(context, i)) {
                event.opcode = NP2_AUDIO86_EVENT_BARRIER_RESERVED;
                async_test_mark_injected(context);
            } else if (is_data && async_test_at_target(context, i) &&
                       async_test_fault(context, NP2_AUDIO86_TEST_FAULT_DATA_ZERO)) {
                event.payload = 0U;
                async_test_mark_injected(context);
            } else if (is_data && async_test_at_target(context, i) &&
                       async_test_fault(context, NP2_AUDIO86_TEST_FAULT_DATA_UNALIGNED)) {
                event.payload = 2U;
                async_test_mark_injected(context);
            } else if (is_data && async_test_at_target(context, i) &&
                       async_test_fault(context, NP2_AUDIO86_TEST_FAULT_DATA_OVERSIZE)) {
                event.payload = NP2_AUDIO86_ASYNC_MAX_DATA_RUN + 4U;
                async_test_mark_injected(context);
            }
            if (is_data && async_test_fault(
                               context, NP2_AUDIO86_TEST_FAULT_CUT_BEFORE_BYTES)) {
                async_test_gate(context, NP2_AUDIO86_TEST_GATE_BEFORE_WATERMARK, i);
                async_test_mark_injected(context);
                async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
                return NULL;
            }
#endif
            context->producer_event_index = (uint32_t)i;
            if (is_data &&
                event.payload != 0U && event.payload <= NP2_AUDIO86_ASYNC_MAX_DATA_RUN &&
                (event.payload & 3U) == 0U) {
                uint32_t byte_count = event.payload;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
                if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_WORKER) &&
                    async_test_at_target(context, i)) {
                    async_test_gate(context,
                                    NP2_AUDIO86_TEST_GATE_PRODUCER_BYTE_FULL,
                                    i);
                }
                if (async_test_fault(
                        context, NP2_AUDIO86_TEST_FAULT_DATA_UNAVAILABLE) &&
                    async_test_at_target(context, i)) {
                    byte_count -= 4U;
                    async_test_mark_injected(context);
                }
#endif
                if (async_enqueue_bytes(context, context->producer_source,
                                        byte_count) != 0) {
                    return NULL;
                }
                ++context->producer_data_runs;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
                if (is_cut && async_test_fault(
                                  context, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_BYTES)) {
                    async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_BYTE_PUBLICATION,
                                    i);
                    async_test_mark_injected(context);
                    async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
                    return NULL;
                }
#endif
            }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_WITHHOLD_FINAL) &&
                async_test_at_target(context, i)) {
                async_test_mark_injected(context);
                ++i;
                continue;
            }
            if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_INCOMPLETE_GROUP) &&
                async_test_at_target(context, i)) {
                async_test_mark_injected(context);
                ++i;
                continue;
            }
            if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_WATERMARK_PAST_EVENT) &&
                async_test_at_target(context, i)) {
                async_test_mark_injected(context);
                ++i;
                continue;
            }
#endif
            if (async_enqueue_event(context, &event) != 0) {
                return NULL;
            }
            ++published_count;
            atomic_store_explicit(&context->published_event_count,
                                  published_count, memory_order_release);
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (is_cut && async_test_fault(
                              context, NP2_AUDIO86_TEST_FAULT_CUT_AFTER_EVENT)) {
                async_test_gate(context, NP2_AUDIO86_TEST_GATE_AFTER_EVENT_PUBLICATION,
                                i);
                async_test_mark_injected(context);
                async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
                return NULL;
            }
#endif
            ++i;
            ++yield_counter;
            if (context->mode == NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE &&
                context->producer_data_runs == 2U) {
                atomic_store_explicit(&context->producer_prefill_ready, true,
                                      memory_order_release);
            }
            if (async_should_yield(context, true, yield_counter)) {
                async_yield(context, true);
            }
        } while (i < context->plan_count &&
                 context->plan[i].frame_timestamp == frame);
        {
            const uint64_t next = i < context->plan_count
                                      ? context->plan[i].frame_timestamp
                                      : NP2_AUDIO86_DURATION_FRAMES;
            if (async_publish_watermark(context, next) != 0) {
                return NULL;
            }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (async_test_fault(context,
                                 NP2_AUDIO86_TEST_FAULT_CUT_AFTER_WATERMARK) &&
                next != 0U) {
                async_test_mark_injected(context);
                async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
                return NULL;
            }
            if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_DONE_EARLY) &&
                context->result->watermark_publish_count == 1U) {
                async_test_mark_injected(context);
                atomic_store_explicit(&context->producer_done, true,
                                      memory_order_release);
                return NULL;
            }
#endif
        }
    }
    if (async_error(context) == NP2_AUDIO86_ASYNC_ERROR_NONE) {
        atomic_store_explicit(&context->producer_done, true,
                              memory_order_release);
    }
    return NULL;
}

static int async_worker_wait(struct np2audio86_async_context *context)
{
    ++context->result->event_empty_wait_count;
    ++context->result->worker_wait_watermark_count;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    if (async_test_yield_enabled(context,
                                 NP2_AUDIO86_TEST_YIELD_WATERMARK_WAIT)) {
        async_yield(context, false);
    }
    async_test_gate(context, NP2_AUDIO86_TEST_GATE_WORKER_WATERMARK_WAIT,
                    UINT32_MAX);
#endif
    async_yield(context, false);
    return async_error(context) == NP2_AUDIO86_ASYNC_ERROR_NONE ? 0 : -1;
}

static uint32_t async_plan_prefix_count(
    const struct np2audio86_async_context *context, uint64_t frame)
{
    uint32_t count = 0U;
    while (count < context->plan_count &&
           context->plan[count].frame_timestamp < frame) {
        ++count;
    }
    return count;
}

static int async_validate_watermark(
    struct np2audio86_async_context *context, uint64_t watermark)
{
    const uint32_t published = atomic_load_explicit(
        &context->published_event_count, memory_order_acquire);
    const uint32_t required = async_plan_prefix_count(context, watermark);
    if (watermark < context->worker_last_watermark ||
        watermark > NP2_AUDIO86_DURATION_FRAMES) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WATERMARK);
        return -1;
    }
    if (published < required) {
        async_fail(context, watermark == NP2_AUDIO86_DURATION_FRAMES
                               ? NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION
                               : NP2_AUDIO86_ASYNC_ERROR_WATERMARK);
        return -1;
    }
    context->worker_last_watermark = watermark;
    return 0;
}

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
static int async_test_worker_fault(struct np2audio86_async_context *context)
{
    if (async_test_fault(context, NP2_AUDIO86_TEST_FAULT_WORKER) &&
        atomic_load_explicit(&context->test_control->gate_reached,
                             memory_order_acquire) &&
        async_test_take_fault(context, NP2_AUDIO86_TEST_FAULT_WORKER)) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
        return -1;
    }
    if (async_test_fault(
            context, NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY) &&
        async_test_take_fault(context,
                              NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY)) {
        async_test_mark_injected(context);
        context->worker_error_attempted = true;
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
    }
    return 0;
}
#endif

static int async_worker_apply_at_cursor(
    struct np2audio86_async_context *context)
{
    for (;;) {
        const struct np2audio86_event *event = NULL;
        int status = np2audio86_event_ring_peek(&context->events, &event);
        if (status == NP2_AUDIO86_TRANSPORT_EMPTY) {
            return 0;
        }
        if (status != NP2_AUDIO86_TRANSPORT_OK || event == NULL) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
            return -1;
        }
        if (event->frame_timestamp >= NP2_AUDIO86_DURATION_FRAMES ||
            event->frame_timestamp < context->worker_state.rendered_frames) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP);
            return -1;
        }
        if (event->frame_timestamp != context->worker_state.rendered_frames) {
            return 0;
        }
        if (async_apply_event(context, event) != 0) {
            return -1;
        }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
        if (async_test_yield_enabled(context,
                                     NP2_AUDIO86_TEST_YIELD_BEFORE_EVENT_TAIL)) {
            async_yield(context, false);
        }
        async_test_gate(context, NP2_AUDIO86_TEST_GATE_BEFORE_EVENT_TAIL,
                        (size_t)event->sequence);
#endif
        if (
            np2audio86_event_ring_consume(&context->events) !=
                NP2_AUDIO86_TRANSPORT_OK) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
            return -1;
        }
        ++context->result->event_pop_count;
    }
}

static void *async_worker_thread(void *opaque)
{
    struct np2audio86_async_context *context = opaque;
    struct np2audio86_fixture_result *result;
    np2_sha256_context pcm_sha;
    uint32_t pcm_crc;
    uint32_t quantum;
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U];
    if (context == NULL) {
        return NULL;
    }
    result = &context->result->oracle;
    memset(result, 0, sizeof(*result));
    configure_fm(&context->worker_state.fm);
    configure_psg(&context->worker_state.psg);
    configure_rhythm(&context->worker_state);
    configure_pcm86(&context->worker_state.pcm86, NULL);
    np2audio86_fixture_hash_control(result);
    np2audio86_fixture_hash_source(result, context->worker_state.pcm86.source);
    context->transport_crc32 = np2_crc32_iso_hdlc_init();
    np2_sha256_init(&context->transport_sha);
    pcm_crc = np2_crc32_iso_hdlc_init();
    np2_sha256_init(&pcm_sha);
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    if (async_test_worker_fault(context) != 0) {
        return NULL;
    }
#endif
    for (quantum = 0U; quantum < NP2_AUDIO86_QUANTA; ++quantum) {
        SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
        size_t offset = 0U;
        memset(mix, 0, sizeof(mix));
        while (offset < NP2_AUDIO86_QUANTUM_FRAMES) {
            uint64_t watermark;
            uint64_t next_frame;
            const struct np2audio86_event *event = NULL;
            int peek_status;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
            if (async_test_worker_fault(context) != 0) {
                return NULL;
            }
#endif
            if (async_error(context) != NP2_AUDIO86_ASYNC_ERROR_NONE) {
                return NULL;
            }
            if (async_worker_apply_at_cursor(context) != 0) {
                return NULL;
            }
            watermark = atomic_load_explicit(&context->committed_through_frame,
                                             memory_order_acquire);
            if (async_validate_watermark(context, watermark) != 0) {
                return NULL;
            }
            if (context->worker_state.rendered_frames >= watermark) {
                if (atomic_load_explicit(&context->producer_done,
                                         memory_order_acquire)) {
                    /* producer_done follows the final watermark release.
                     * Re-acquire that authoritative value before deciding
                     * completion; watermark above predates the done acquire. */
                    watermark = atomic_load_explicit(
                        &context->committed_through_frame,
                        memory_order_acquire);
                    if (async_validate_watermark(context, watermark) != 0) {
                        return NULL;
                    }
                    if (context->worker_state.rendered_frames < watermark) {
                        continue;
                    }
                    if (watermark < NP2_AUDIO86_DURATION_FRAMES) {
                        async_fail(context,
                                   NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION);
                        return NULL;
                    }
                }
                if (async_worker_wait(context) != 0) {
                    return NULL;
                }
                continue;
            }
            next_frame = (uint64_t)(quantum + 1U) * NP2_AUDIO86_QUANTUM_FRAMES;
            if (next_frame > watermark) {
                next_frame = watermark;
            }
            peek_status = np2audio86_event_ring_peek(&context->events, &event);
            if (peek_status == NP2_AUDIO86_TRANSPORT_OK && event != NULL) {
                if (event->frame_timestamp >= NP2_AUDIO86_DURATION_FRAMES ||
                    event->frame_timestamp < context->worker_state.rendered_frames) {
                    async_fail(context, NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP);
                    return NULL;
                }
                if (event->frame_timestamp < next_frame) {
                    next_frame = event->frame_timestamp;
                }
            } else if (peek_status != NP2_AUDIO86_TRANSPORT_EMPTY) {
                async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
                return NULL;
            }
            if (next_frame == context->worker_state.rendered_frames) {
                if (async_worker_apply_at_cursor(context) != 0) {
                    return NULL;
                }
                continue;
            }
            if (async_render_span(context, mix + offset * 2U,
                                  (size_t)(next_frame -
                                           context->worker_state.rendered_frames),
                                  result) != 0) {
                return NULL;
            }
            offset = (size_t)(context->worker_state.rendered_frames -
                              (uint64_t)quantum * NP2_AUDIO86_QUANTUM_FRAMES);
            if (async_should_yield(context, false,
                                   context->worker_state.rendered_frames)) {
                async_yield(context, false);
            }
        }
        {
            struct np2opngen_pcm_stats stats;
            if (np2opngen_pcm_canonicalize_s16le(
                    mix, NP2_AUDIO86_QUANTUM_FRAMES, NP2_AUDIO86_CHANNELS,
                    canonical, sizeof(canonical), &stats) != 0) {
                async_fail(context, NP2_AUDIO86_ASYNC_ERROR_CANONICAL);
                return NULL;
            }
            if (stats.s32_abs_peak > result->mix_peak_abs) {
                result->mix_peak_abs = stats.s32_abs_peak;
            }
            result->clamped_samples += stats.clip_samples;
            pcm_crc = np2_crc32_iso_hdlc_update(pcm_crc, canonical,
                                                sizeof(canonical));
            np2_sha256_update(&pcm_sha, canonical, sizeof(canonical));
        }
    }
    result->pcm_crc32 = np2_crc32_iso_hdlc_finish(pcm_crc);
    np2_sha256_final(&pcm_sha, result->pcm_sha256);
    result->frames = NP2_AUDIO86_DURATION_FRAMES;
    result->bytes = NP2_AUDIO86_PCM_BYTES;
    result->quanta = NP2_AUDIO86_QUANTA;
    result->pcm86_bytes_supplied = context->worker_state.pcm86.supplied;
    result->pcm86_bytes_consumed = context->worker_state.pcm86.supplied -
                                   (uint64_t)context->worker_state.pcm86.pcm.realbuf;
    result->pcm86_refills = context->worker_state.pcm86.refills;
    result->pcm86_fifo_min = context->worker_state.pcm86.fifo_min;
    result->pcm86_fifo_max = context->worker_state.pcm86.fifo_max;
    result->pcm86_fifo_underrun = context->worker_state.pcm86.underrun;
    if (context->worker_next_sequence != context->plan_count ||
        atomic_load_explicit(&context->committed_through_frame,
                             memory_order_acquire) != NP2_AUDIO86_DURATION_FRAMES ||
        !atomic_load_explicit(&context->producer_done, memory_order_acquire) ||
        np2audio86_event_ring_occupancy(&context->events) != 0U ||
        np2audio86_byte_ring_occupancy(&context->pcm_bytes) != 0U ||
        context->worker_state.rendered_frames != NP2_AUDIO86_DURATION_FRAMES) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION);
        return NULL;
    }
    np2_sha256_final(&context->transport_sha,
                     context->result->transport_event_sha256);
    context->result->transport_event_count = context->transport_count;
    context->result->transport_event_crc32 =
        np2_crc32_iso_hdlc_finish(context->transport_crc32);
    atomic_store_explicit(&context->worker_done, true, memory_order_release);
    return NULL;
}

static int async_oracle_equal(const struct np2audio86_fixture_result *actual,
                              const struct np2audio86_fixture_result *expected)
{
    if (actual == NULL || expected == NULL ||
        actual->frames != expected->frames || actual->bytes != expected->bytes ||
        actual->quanta != expected->quanta ||
        actual->pcm_crc32 != expected->pcm_crc32 ||
        memcmp(actual->pcm_sha256, expected->pcm_sha256,
               NP2_SHA256_DIGEST_SIZE) != 0 ||
        actual->control_events != expected->control_events ||
        actual->mid_quantum_events != expected->mid_quantum_events ||
        actual->control_crc32 != expected->control_crc32 ||
        memcmp(actual->control_sha256, expected->control_sha256,
               NP2_SHA256_DIGEST_SIZE) != 0 ||
        actual->source_crc32 != expected->source_crc32 ||
        memcmp(actual->source_sha256, expected->source_sha256,
               NP2_SHA256_DIGEST_SIZE) != 0 ||
        actual->pcm86_bytes_supplied != expected->pcm86_bytes_supplied ||
        actual->pcm86_bytes_consumed != expected->pcm86_bytes_consumed ||
        actual->pcm86_refills != expected->pcm86_refills ||
        actual->pcm86_fifo_min != expected->pcm86_fifo_min ||
        actual->pcm86_fifo_max != expected->pcm86_fifo_max ||
        actual->mix_peak_abs != expected->mix_peak_abs ||
        actual->clamped_samples != expected->clamped_samples ||
        actual->fm_contribution != expected->fm_contribution ||
        actual->psg_contribution != expected->psg_contribution ||
        actual->rhythm_contribution != expected->rhythm_contribution ||
        actual->pcm86_contribution != expected->pcm86_contribution ||
        actual->pcm86_fifo_underrun != 0U || actual->sequence_error != 0U ||
        actual->arithmetic_error != 0U) {
        return 0;
    }
    return 1;
}

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
static int async_test_pthread_create(
    struct np2audio86_async_context *context, pthread_t *thread,
    void *(*entry)(void *), int producer)
{
    if (producer &&
        async_test_take_fault(context, NP2_AUDIO86_TEST_FAULT_PRODUCER_CREATE)) {
        return EAGAIN;
    }
    if (!producer &&
        async_test_take_fault(context, NP2_AUDIO86_TEST_FAULT_WORKER_CREATE)) {
        return EAGAIN;
    }
    return pthread_create(thread, NULL, entry, context);
}

static void async_test_observe(
    const struct np2audio86_async_context *context,
    struct np2audio86_async_test_observer *observer,
    uint32_t expected_error, uint8_t producer_created,
    uint8_t worker_created, uint8_t producer_reaped, uint8_t worker_reaped)
{
    if (observer == NULL) {
        return;
    }
    memset(observer, 0, sizeof(*observer));
    observer->expected_error = expected_error;
    observer->observed_error = (uint32_t)async_error(context);
    observer->injected = context->test_control != NULL &&
                         atomic_load_explicit(
                             &context->test_control->fault_injected,
                             memory_order_acquire);
    observer->detected = observer->injected &&
                         observer->observed_error == expected_error;
    observer->producer_created = producer_created;
    observer->worker_created = worker_created;
    observer->producer_reaped = producer_reaped;
    observer->worker_reaped = worker_reaped;
    observer->workload_success = context->result->passed;
    observer->peer_unblocked = observer->detected &&
                               (!producer_created || producer_reaped) &&
                               (!worker_created || worker_reaped);
    observer->later_error_attempted = context->worker_error_attempted ? 1U : 0U;
    observer->producer_terminal = producer_created
                                      ? atomic_load_explicit(
                                            &context->producer_done,
                                            memory_order_acquire)
                                            ? NP2_AUDIO86_TEST_TERMINAL_COMPLETED
                                            : NP2_AUDIO86_TEST_TERMINAL_ABORTED
                                      : NP2_AUDIO86_TEST_TERMINAL_NOT_STARTED;
    observer->worker_terminal = worker_created
                                    ? atomic_load_explicit(
                                          &context->worker_done,
                                          memory_order_acquire)
                                          ? NP2_AUDIO86_TEST_TERMINAL_COMPLETED
                                          : NP2_AUDIO86_TEST_TERMINAL_ABORTED
                                    : NP2_AUDIO86_TEST_TERMINAL_NOT_STARTED;
    observer->event_residual = np2audio86_event_ring_occupancy(&context->events);
    observer->byte_residual = np2audio86_byte_ring_occupancy(&context->pcm_bytes);
}
#else
static int async_test_pthread_create(
    struct np2audio86_async_context *context, pthread_t *thread,
    void *(*entry)(void *), int producer)
{
    (void)producer;
    return pthread_create(thread, NULL, entry, context);
}
#endif

const char *np2audio86_async_mode_name(enum np2audio86_async_mode mode)
{
    switch (mode) {
    case NP2_AUDIO86_ASYNC_PRODUCER_FAST_WORKER_YIELD:
        return "PRODUCER_FAST_WORKER_YIELD";
    case NP2_AUDIO86_ASYNC_PRODUCER_YIELD_WORKER_FAST:
        return "PRODUCER_YIELD_WORKER_FAST";
    case NP2_AUDIO86_ASYNC_DETERMINISTIC_ALTERNATING:
        return "DETERMINISTIC_ALTERNATING_YIELDS";
    case NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE:
        return "BYTE_TRANSPORT_PRESSURE";
    }
    return "INVALID";
}

const char *np2audio86_async_error_name(uint32_t error)
{
    switch (error) {
    case NP2_AUDIO86_ASYNC_ERROR_NONE: return "NONE";
    case NP2_AUDIO86_ASYNC_ERROR_ARGUMENT: return "ARGUMENT";
    case NP2_AUDIO86_ASYNC_ERROR_PLAN: return "PLAN";
    case NP2_AUDIO86_ASYNC_ERROR_SEQUENCE: return "SEQUENCE";
    case NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP: return "TIMESTAMP";
    case NP2_AUDIO86_ASYNC_ERROR_OPCODE: return "OPCODE";
    case NP2_AUDIO86_ASYNC_ERROR_PAYLOAD: return "PAYLOAD";
    case NP2_AUDIO86_ASYNC_ERROR_BYTE_RING: return "BYTE_RING";
    case NP2_AUDIO86_ASYNC_ERROR_PCM86_UNDERRUN: return "PCM86_UNDERRUN";
    case NP2_AUDIO86_ASYNC_ERROR_ARITHMETIC: return "ARITHMETIC";
    case NP2_AUDIO86_ASYNC_ERROR_CANONICAL: return "CANONICAL";
    case NP2_AUDIO86_ASYNC_ERROR_PRODUCER: return "PRODUCER";
    case NP2_AUDIO86_ASYNC_ERROR_WORKER: return "WORKER";
    case NP2_AUDIO86_ASYNC_ERROR_COMPLETION: return "COMPLETION";
    case NP2_AUDIO86_ASYNC_ERROR_LIVENESS: return "LIVENESS";
    case NP2_AUDIO86_ASYNC_ERROR_DATA_LENGTH: return "DATA_LENGTH";
    case NP2_AUDIO86_ASYNC_ERROR_DATA_AVAILABILITY: return "DATA_AVAILABILITY";
    case NP2_AUDIO86_ASYNC_ERROR_WATERMARK: return "WATERMARK";
    case NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION:
        return "PREMATURE_COMPLETION";
    case NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT:
        return "TRANSPORT_INVARIANT";
    case NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH:
        return "ORACLE_MISMATCH";
    }
    return "UNKNOWN";
}

static int np2audio86_async_run_internal(
    enum np2audio86_async_mode mode, struct np2audio86_async_result *result
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    , struct np2audio86_async_test_control *test_control,
    struct np2audio86_async_test_observer *observer
#endif
)
{
    struct np2audio86_fixture_result expected;
    struct np2audio86_async_context *context;
    pthread_t producer;
    pthread_t worker;
    bool producer_created = false;
    bool worker_created = false;
    uint8_t producer_reaped = 0U;
    uint8_t worker_reaped = 0U;
    bool allow_worker_after_error = false;
    if (result == NULL || mode > NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->mode = (uint32_t)mode;
    if (np2audio86_fixture_render(&expected) != 0 ||
        !np2audio86_fixture_matches_golden(&expected)) {
        result->first_error = NP2_AUDIO86_ASYNC_ERROR_PLAN;
        return -1;
    }
    context = calloc(1U, sizeof(*context));
    if (context == NULL) {
        result->first_error = NP2_AUDIO86_ASYNC_ERROR_ARGUMENT;
        return -1;
    }
    context->mode = mode;
    context->result = result;
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    context->test_control = test_control;
#endif
    atomic_init(&context->first_error, NP2_AUDIO86_ASYNC_ERROR_NONE);
    atomic_init(&context->committed_through_frame, 0U);
    atomic_init(&context->producer_done, false);
    atomic_init(&context->producer_prefill_ready, false);
    atomic_init(&context->worker_done, false);
    atomic_init(&context->published_event_count, 0U);
    np2audio86_event_ring_init(&context->events);
    np2audio86_byte_ring_init(&context->pcm_bytes);
    if (np2audio86_async_build_plan(context->plan, &context->plan_count) != 0 ||
        np2audio86_async_validate_plan(context->plan, context->plan_count) != 0) {
        async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PLAN);
        result->first_error = NP2_AUDIO86_ASYNC_ERROR_PLAN;
        free(context);
        return -1;
    }
    if (mode == NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE) {
        if (async_test_pthread_create(context, &producer,
                                      async_producer_thread, 1) != 0) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
        } else {
            producer_created = true;
        }
        while (!atomic_load_explicit(&context->producer_prefill_ready,
                                     memory_order_acquire) &&
               async_error(context) == NP2_AUDIO86_ASYNC_ERROR_NONE) {
            sched_yield();
        }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
        if (async_test_fault(
                context, NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY) &&
            async_error(context) == NP2_AUDIO86_ASYNC_ERROR_NONE) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
        }
        allow_worker_after_error =
            test_control != NULL &&
            test_control->fault ==
                NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY;
#endif
        if ((async_error(context) == NP2_AUDIO86_ASYNC_ERROR_NONE ||
             allow_worker_after_error) &&
            async_test_pthread_create(context, &worker, async_worker_thread, 0) ==
                0) {
            worker_created = true;
        } else {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
        }
    } else {
        if (async_test_pthread_create(context, &worker,
                                      async_worker_thread, 0) != 0) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_WORKER);
        } else {
            worker_created = true;
        }
        if (async_test_pthread_create(context, &producer,
                                      async_producer_thread, 1) != 0) {
            async_fail(context, NP2_AUDIO86_ASYNC_ERROR_PRODUCER);
        } else {
            producer_created = true;
        }
    }
    if (producer_created) {
        producer_reaped = pthread_join(producer, NULL) == 0 ? 1U : 0U;
    }
    if (worker_created) {
        worker_reaped = pthread_join(worker, NULL) == 0 ? 1U : 0U;
    }
    result->first_error = atomic_load_explicit(&context->first_error,
                                               memory_order_acquire);
    result->passed = result->first_error == NP2_AUDIO86_ASYNC_ERROR_NONE &&
                     atomic_load_explicit(&context->worker_done,
                                          memory_order_acquire) &&
                     async_oracle_equal(&result->oracle, &expected) &&
                     result->event_push_count == result->event_pop_count &&
                     result->event_push_count == context->plan_count &&
                     result->pcm86_data_run_count == 323U &&
                     result->pcm86_byte_push_bytes ==
                         result->pcm86_byte_pop_bytes &&
                     np2audio86_event_ring_occupancy(&context->events) == 0U &&
                     np2audio86_byte_ring_occupancy(&context->pcm_bytes) == 0U;
    if (result->first_error == NP2_AUDIO86_ASYNC_ERROR_NONE && !result->passed) {
        result->first_error = async_oracle_equal(&result->oracle, &expected)
                                  ? NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION
                                  : NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH;
    }
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
    async_test_observe(context, observer,
                       test_control == NULL
                           ? NP2_AUDIO86_ASYNC_ERROR_NONE
                           : test_control->fault ==
                                     NP2_AUDIO86_TEST_FAULT_DATA_ZERO ||
                                 test_control->fault ==
                                     NP2_AUDIO86_TEST_FAULT_DATA_UNALIGNED ||
                                 test_control->fault ==
                                     NP2_AUDIO86_TEST_FAULT_DATA_OVERSIZE
                             ? NP2_AUDIO86_ASYNC_ERROR_DATA_LENGTH
                             : test_control->fault ==
                                       NP2_AUDIO86_TEST_FAULT_DATA_UNAVAILABLE
                                 ? NP2_AUDIO86_ASYNC_ERROR_DATA_AVAILABILITY
                                 : test_control->fault ==
                                           NP2_AUDIO86_TEST_FAULT_WATERMARK_REGRESSION ||
                                       test_control->fault ==
                                           NP2_AUDIO86_TEST_FAULT_WATERMARK_OVER_END ||
                                       test_control->fault ==
                                           NP2_AUDIO86_TEST_FAULT_WATERMARK_PAST_EVENT ||
                                       test_control->fault ==
                                           NP2_AUDIO86_TEST_FAULT_INCOMPLETE_GROUP
                                     ? NP2_AUDIO86_ASYNC_ERROR_WATERMARK
                                     : test_control->fault ==
                                               NP2_AUDIO86_TEST_FAULT_DONE_EARLY ||
                                           test_control->fault ==
                                               NP2_AUDIO86_TEST_FAULT_WITHHOLD_FINAL
                                         ? NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION
                                         : test_control->fault ==
                                                   NP2_AUDIO86_TEST_FAULT_WORKER
                                             ? NP2_AUDIO86_ASYNC_ERROR_WORKER
                                             : test_control->fault ==
                                                       NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY
                                                 ? NP2_AUDIO86_ASYNC_ERROR_PRODUCER
                                                 : test_control->fault ==
                                                           NP2_AUDIO86_TEST_FAULT_PRODUCER_CREATE
                                                     ? NP2_AUDIO86_ASYNC_ERROR_PRODUCER
                                                     : test_control->fault ==
                                                               NP2_AUDIO86_TEST_FAULT_WORKER_CREATE
                                                         ? NP2_AUDIO86_ASYNC_ERROR_WORKER
                                                         : test_control->fault ==
                                                                       NP2_AUDIO86_TEST_FAULT_CUT_BEFORE_BYTES ||
                                                                   test_control->fault ==
                                                                       NP2_AUDIO86_TEST_FAULT_CUT_AFTER_BYTES ||
                                                                   test_control->fault ==
                                                                       NP2_AUDIO86_TEST_FAULT_CUT_AFTER_EVENT ||
                                                                   test_control->fault ==
                                                                       NP2_AUDIO86_TEST_FAULT_CUT_AFTER_WATERMARK
                                                               ? NP2_AUDIO86_ASYNC_ERROR_PRODUCER
                                                               : test_control->fault ==
                                                                         NP2_AUDIO86_TEST_FAULT_DUPLICATE_SEQUENCE ||
                                                                     test_control->fault ==
                                                                         NP2_AUDIO86_TEST_FAULT_SKIPPED_SEQUENCE
                                                                 ? NP2_AUDIO86_ASYNC_ERROR_SEQUENCE
                                                                 : test_control->fault ==
                                                                           NP2_AUDIO86_TEST_FAULT_TIMESTAMP_BEHIND ||
                                                                       test_control->fault ==
                                                                           NP2_AUDIO86_TEST_FAULT_TIMESTAMP_END
                                                                     ? NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP
                                                                     : test_control->fault ==
                                                                               NP2_AUDIO86_TEST_FAULT_INVALID_OPCODE ||
                                                                           test_control->fault ==
                                                                               NP2_AUDIO86_TEST_FAULT_RESERVED_OPCODE
                                                                       ? NP2_AUDIO86_ASYNC_ERROR_OPCODE
                                                                       : NP2_AUDIO86_ASYNC_ERROR_NONE,
                       producer_created ? 1U : 0U, worker_created ? 1U : 0U,
                       producer_reaped, worker_reaped);
#else
    (void)producer_reaped;
    (void)worker_reaped;
#endif
    free(context);
    return result->passed ? 0 : -1;
}

int np2audio86_async_run(enum np2audio86_async_mode mode,
                         struct np2audio86_async_result *result)
{
    return np2audio86_async_run_internal(mode, result
#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
                                         , NULL, NULL
#endif
    );
}

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)
int np2audio86_async_run_with_test_control(
    enum np2audio86_async_mode mode,
    struct np2audio86_async_test_control *control,
    struct np2audio86_async_test_observer *observer,
    struct np2audio86_async_result *result)
{
    if (control == NULL || observer == NULL) {
        return -1;
    }
    return np2audio86_async_run_internal(mode, result, control, observer);
}

int np2audio86_async_test_prevalidate(unsigned case_id)
{
    struct np2audio86_event plan[NP2_AUDIO86_ASYNC_MAX_EVENTS];
    size_t count = 0U;
    if (np2audio86_async_build_plan(plan, &count) != 0) {
        return -1;
    }
    switch (case_id) {
    case 1U:
        plan[1U].sequence = plan[0U].sequence;
        if (np2audio86_async_validate_plan(plan, count) == 0 ||
            np2audio86_async_build_plan(plan, &count) != 0) {
            return -1;
        }
        plan[1U].sequence += 1U;
        return np2audio86_async_validate_plan(plan, count) != 0 ? 0 : -1;
    case 2U:
        plan[8U].frame_timestamp = plan[7U].frame_timestamp - 1U;
        if (np2audio86_async_validate_plan(plan, count) == 0) {
            return -1;
        }
        if (np2audio86_async_build_plan(plan, &count) != 0) {
            return -1;
        }
        plan[count - 1U].frame_timestamp = NP2_AUDIO86_DURATION_FRAMES;
        return np2audio86_async_validate_plan(plan, count) != 0 ? 0 : -1;
    case 3U:
        plan[6U].payload = 0U;
        return np2audio86_async_validate_plan(plan, count) != 0 ? 0 : -1;
    case 4U:
        plan[6U].payload = 2U;
        return np2audio86_async_validate_plan(plan, count) != 0 ? 0 : -1;
    case 5U:
        plan[6U].payload = NP2_AUDIO86_ASYNC_MAX_DATA_RUN + 4U;
        return np2audio86_async_validate_plan(plan, count) != 0 ? 0 : -1;
    default:
        return -1;
    }
}
#endif

#else

const char *np2audio86_async_mode_name(enum np2audio86_async_mode mode)
{
    (void)mode;
    return "HOST_ASYNC_DISABLED";
}

const char *np2audio86_async_error_name(uint32_t error)
{
    (void)error;
    return "HOST_ASYNC_DISABLED";
}

int np2audio86_async_run(enum np2audio86_async_mode mode,
                         struct np2audio86_async_result *result)
{
    (void)mode;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->first_error = 1U;
    }
    return -1;
}

#endif
