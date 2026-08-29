#include "np2audio86_fixture.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define AUDIO86_FM_CHANNELS 6U
#define AUDIO86_FM_OPERATORS 24U
#define AUDIO86_PSG_CHANNELS 3U
#define AUDIO86_RHYTHM_TRACKS 6U
#define AUDIO86_PCM86_FIFO_BYTES PCM86_BUFSIZE
#define AUDIO86_PCM86_REFILL_BYTES 32768U
#define AUDIO86_RHYTHM_MAX_SAMPLES 313U

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

struct audio86_pcm86_feed {
    _PCM86 pcm;
    uint8_t source[NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES];
    uint32_t source_frame;
    uint64_t supplied;
    uint32_t refills;
    int32_t fifo_min;
    int32_t fifo_max;
    uint8_t underrun;
};

struct audio86_pcmmix {
    PMIXHDR hdr;
    PMIXTRK trk[AUDIO86_RHYTHM_TRACKS];
};

struct audio86_state {
    _OPNGEN fm;
    _PSGGEN psg;
    struct audio86_pcmmix rhythm;
    PMIXTRK rhythm_tracks[AUDIO86_RHYTHM_TRACKS];
    SINT16 rhythm_samples[AUDIO86_RHYTHM_TRACKS][AUDIO86_RHYTHM_MAX_SAMPLES];
    struct audio86_pcm86_feed pcm86;
    size_t next_event;
    uint64_t rendered_frames;
};

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

static void hash_control(struct np2audio86_fixture_result *result)
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

static void configure_pcm86(struct audio86_pcm86_feed *feed)
{
    memset(feed, 0, sizeof(*feed));
    build_pcm86_source(feed->source);
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
                       struct np2audio86_fixture_result *result)
{
    const size_t samples = frames * NP2_AUDIO86_CHANNELS;
    SINT32 fm[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 psg[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 rhythm[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 pcm86[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    memset(fm, 0, sizeof(fm));
    memset(psg, 0, sizeof(psg));
    memset(rhythm, 0, sizeof(rhythm));
    memset(pcm86, 0, sizeof(pcm86));
    feed_pcm86(&state->pcm86, 4096);
    opngen_getpcm(&state->fm, fm, (UINT)frames);
    psggen_getpcm(&state->psg, psg, (UINT)frames);
    pcmmix_getpcm((PCMMIX)&state->rhythm, rhythm, (UINT)frames);
    pcm86gen_getpcm(&state->pcm86.pcm, pcm86, (UINT)frames);
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

static void hash_source(struct np2audio86_fixture_result *result,
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
    configure_pcm86(&state.pcm86);
    hash_control(result);
    hash_source(result, state.pcm86.source);
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
                                              next - offset, result) != 0) {
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
