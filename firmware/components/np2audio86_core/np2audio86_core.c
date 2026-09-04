#include "np2audio86_core.h"

#include <limits.h>
#include <string.h>

enum {
    OPN_INIT_UNINITIALIZED = 0U,
    OPN_INIT_IN_PROGRESS = 1U,
    OPN_INIT_READY = 2U,
    CORE_GUEST_OPNA_REGISTER = 1U,
    CORE_GUEST_OPNA_CSM = 2U,
    CORE_GUEST_PCM_CONTROL = 3U,
    CORE_GUEST_RESET_BARRIER = 0x80000000U,
    CORE_GUEST_PCM_DATA = 3U,
};

static _Atomic uint32_t g_process_init_state;
#if defined(NP2AUDIO86_GUEST_TEST)
static _Atomic uint32_t g_opngen_initialize_call_count;
static _Atomic uint32_t g_opngen_initialize_fail_next;
#endif

/* pcm86g.c calls this device-layer hook before consuming.  The core owns a
 * bounded FIFO directly, so there is no external refill callback here. */
void SOUNDCALL pcm86gen_checkbuf(PCM86 pcm86, UINT count)
{
    (void)pcm86;
    (void)count;
}

#if defined(NP2AUDIO86_GUEST_TEST)
void np2audio86_test_opngen_initialize_reset(void)
{
    atomic_store_explicit(&g_process_init_state, OPN_INIT_UNINITIALIZED,
                          memory_order_release);
    atomic_store_explicit(&g_opngen_initialize_call_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&g_opngen_initialize_fail_next, 0U,
                          memory_order_release);
}

uint32_t np2audio86_test_opngen_initialize_call_count(void)
{
    return atomic_load_explicit(&g_opngen_initialize_call_count,
                                memory_order_acquire);
}

void np2audio86_test_opngen_initialize_fail_next(void)
{
    atomic_store_explicit(&g_opngen_initialize_fail_next, 1U,
                          memory_order_release);
}
#endif

int np2audio86_core_process_initialize(void)
{
    uint32_t expected = OPN_INIT_UNINITIALIZED;
    uint32_t state = atomic_load_explicit(&g_process_init_state,
                                          memory_order_acquire);
    if (state == OPN_INIT_READY)
        return 0;
    if (atomic_compare_exchange_strong_explicit(
            &g_process_init_state, &expected, OPN_INIT_IN_PROGRESS,
            memory_order_acq_rel, memory_order_acquire)) {
#if defined(NP2AUDIO86_GUEST_TEST)
        if (atomic_exchange_explicit(&g_opngen_initialize_fail_next, 0U,
                                     memory_order_acq_rel) != 0U) {
            atomic_store_explicit(&g_process_init_state,
                                  OPN_INIT_UNINITIALIZED,
                                  memory_order_release);
            return -1;
        }
#endif
        opngen_initialize(NP2_AUDIO86_RATE_HZ);
        psggen_initialize(NP2_AUDIO86_RATE_HZ);
        opngen_setvol(48U);
        psggen_setvol(24U);
#if defined(NP2AUDIO86_GUEST_TEST)
        atomic_fetch_add_explicit(&g_opngen_initialize_call_count, 1U,
                                  memory_order_relaxed);
#endif
        atomic_store_explicit(&g_process_init_state, OPN_INIT_READY,
                              memory_order_release);
        return 0;
    }
    do {
        state = atomic_load_explicit(&g_process_init_state,
                                     memory_order_acquire);
    } while (state == OPN_INIT_IN_PROGRESS);
    return state == OPN_INIT_READY ? 0 : -1;
}

static void pcm_set_rate(PCM86 pcm, uint8_t rate_index)
{
    static const uint32_t rates[8] = {
        352800U, 264600U, 176400U, 132300U,
        88200U, 66150U, 44010U, 33075U,
    };
    const uint32_t rate = rates[rate_index & 7U];
    pcm->rateval = rate;
    pcm->div = (SINT32)((rate << (PCM86_DIVBIT - 3U)) /
                        NP2_AUDIO86_RATE_HZ);
    pcm->div2 = (SINT32)((NP2_AUDIO86_RATE_HZ << (PCM86_DIVBIT + 3U)) /
                         rate);
}

static void configure_neutral_mutable(struct np2audio86_render_state *state)
{
    opngen_reset(&state->fm);
    opngen_setcfg(&state->fm, 3U, OPN_STEREO | 0x38U);

    psggen_reset(&state->psg);

    memset(&state->rhythm, 0, sizeof(state->rhythm));
    memset(state->rhythm_tracks, 0, sizeof(state->rhythm_tracks));
    memset(state->rhythm_samples, 0, sizeof(state->rhythm_samples));

    memset(&state->pcm86, 0, sizeof(state->pcm86));
    state->pcm86.pcm.fifosize = 0x80;
    state->pcm86.pcm.dactrl = 0x32U;
    state->pcm86.pcm.stepbit = 2U;
    state->pcm86.pcm.stepmask = 3U;
    state->pcm86.pcm.irq = 0xffU;
    state->pcm86.fifo_min = INT_MAX;
    pcm_set_rate(&state->pcm86.pcm, 0U);
}

int np2audio86_core_render_init(struct np2audio86_render_state *state)
{
    if (state == NULL || np2audio86_core_process_initialize() != 0)
        return -1;
    memset(state, 0, sizeof(*state));
    configure_neutral_mutable(state);
    return 0;
}

int np2audio86_core_render_reset(struct np2audio86_render_state *state)
{
    if (state == NULL ||
        atomic_load_explicit(&g_process_init_state, memory_order_acquire) !=
            OPN_INIT_READY)
        return -1;
    memset(state, 0, sizeof(*state));
    configure_neutral_mutable(state);
    return 0;
}

void np2audio86_core_render_destroy(struct np2audio86_render_state *state)
{
    if (state != NULL)
        memset(state, 0, sizeof(*state));
}

void np2audio86_core_render_set_profile_clock(
    struct np2audio86_render_state *state,
    uint64_t (*now_us)(void *opaque), void *opaque)
{
    if (state == NULL)
        return;
#if defined(NP2_AUDIO86_PROFILE)
    state->profile_now_us = now_us;
    state->profile_clock_opaque = opaque;
#else
    (void)now_us;
    (void)opaque;
#endif
}

static void rhythm_trigger(struct np2audio86_render_state *state,
                           uint8_t value)
{
    unsigned track;
    for (track = 0U; track < NP2_AUDIO86_RHYTHM_TRACKS; ++track) {
        if ((value & (uint8_t)(1U << track)) != 0U) {
            PMIXTRK *track_state = &state->rhythm.trk[track];
            if (track_state->data.sample == NULL ||
                track_state->data.samples == 0U)
                continue;
            track_state->pcm = track_state->data.sample;
            track_state->remain = track_state->data.samples;
            state->rhythm.hdr.playing |= UINT32_C(1) << track;
        }
    }
}

int np2audio86_core_render_apply_opna_register(
    struct np2audio86_render_state *state, uint16_t address, uint8_t value)
{
    const uint8_t bank = address >= 0x100U ? 3U : 0U;
    const uint8_t reg = (uint8_t)(address & 0xffU);
    if (state == NULL)
        return -1;
    if (reg < 0x10U && address < 0x100U) {
        psggen_setreg(&state->psg, reg, value);
    } else if (reg == 0x10U && address < 0x100U) {
        rhythm_trigger(state, value);
    } else if (reg == 0x28U && address < 0x100U) {
        opngen_keyon(&state->fm, value & 7U, value & 0xf0U);
    } else if (reg >= 0x30U) {
        opngen_setreg(&state->fm, bank, reg, value);
    }
    return 0;
}

int np2audio86_core_render_apply_opna_csm(
    struct np2audio86_render_state *state)
{
    if (state == NULL)
        return -1;
    opngen_csm(&state->fm);
    return 0;
}

int np2audio86_core_render_apply_pcm86_control(
    struct np2audio86_render_state *state, uint8_t register_index,
    uint8_t value)
{
    PCM86 pcm;
    uint8_t old;
    if (state == NULL)
        return -1;
    pcm = &state->pcm86.pcm;
    switch (register_index) {
    case 0x00U:
        break;
    case 0x06U:
        if ((value & 0xe0U) == 0xa0U) {
            const uint8_t vol5 = (uint8_t)((~value) & 15U);
            pcm->volume = 64 * vol5;
        }
        break;
    case 0x08U:
        old = pcm->fifo;
        if ((value & 8U) != 0U && (old & 8U) == 0U) {
            pcm->wrtpos = 0U;
            pcm->readpos = 0U;
            pcm->realbuf = 0;
        }
        if (((old ^ value) & 7U) != 0U)
            pcm_set_rate(pcm, value);
        pcm->fifo = value;
        break;
    case 0x0aU:
        if ((pcm->fifo & 0x20U) == 0U && (value & 15U) != 15U) {
            pcm->dactrl = value;
            pcm_set_rate(pcm, pcm->fifo);
        }
        break;
    default:
        return -1;
    }
    return 0;
}

int np2audio86_core_render_pcm86_push(
    struct np2audio86_render_state *state, const uint8_t *bytes,
    size_t count)
{
    struct np2audio86_pcm86_feed *feed;
    uint32_t destination;
    size_t first;
    if (state == NULL || bytes == NULL || count == 0U ||
        count > NP2_AUDIO86_PCM86_REFILL_BYTES)
        return -1;
    feed = &state->pcm86;
    if (feed->pcm.realbuf < 0 ||
        feed->pcm.realbuf > PCM86_BUFSIZE - (SINT32)count) {
        feed->underrun = 1U;
        return -1;
    }
    destination = feed->pcm.wrtpos & PCM86_BUFMSK;
    first = PCM86_BUFSIZE - destination;
    if (first > count)
        first = count;
    memcpy(feed->pcm.buffer + destination, bytes, first);
    if (count > first)
        memcpy(feed->pcm.buffer, bytes + first, count - first);
    feed->pcm.wrtpos = (feed->pcm.wrtpos + (UINT32)count) & PCM86_BUFMSK;
    feed->pcm.realbuf += (SINT32)count;
    feed->supplied += count;
    ++feed->refills;
    if (feed->pcm.realbuf < feed->fifo_min)
        feed->fifo_min = feed->pcm.realbuf;
    if (feed->pcm.realbuf > feed->fifo_max)
        feed->fifo_max = feed->pcm.realbuf;
    return 0;
}

static int nonzero_samples(const SINT32 *samples, size_t count)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        if (samples[i] != 0)
            return 1;
    }
    return 0;
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

int np2audio86_core_render_span(
    struct np2audio86_render_state *state, SINT32 *mix, size_t frames,
    struct np2audio86_core_mix_result *result)
{
    const size_t samples = frames * NP2_AUDIO86_CHANNELS;
    SINT32 *fm;
    SINT32 *psg;
    SINT32 *rhythm;
    SINT32 *pcm86;
#if defined(NP2_AUDIO86_PROFILE)
    uint64_t profile_begin;
#endif
    if (state == NULL || mix == NULL || result == NULL || frames == 0U ||
        frames > NP2_AUDIO86_QUANTUM_FRAMES)
        return -1;
    fm = state->fm_scratch;
    psg = state->psg_scratch;
    rhythm = state->rhythm_scratch;
    pcm86 = state->pcm86_scratch;
    memset(fm, 0, samples * sizeof(*fm));
    memset(psg, 0, samples * sizeof(*psg));
    memset(rhythm, 0, samples * sizeof(*rhythm));
    memset(pcm86, 0, samples * sizeof(*pcm86));
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
    result->fm_contribution |= (uint8_t)nonzero_samples(fm, samples);
    result->psg_contribution |= (uint8_t)nonzero_samples(psg, samples);
    result->rhythm_contribution |= (uint8_t)nonzero_samples(rhythm, samples);
    result->pcm86_contribution |= (uint8_t)nonzero_samples(pcm86, samples);
    if (add_source(mix, fm, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, psg, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, rhythm, samples, &result->arithmetic_error) != 0 ||
        add_source(mix, pcm86, samples, &result->arithmetic_error) != 0)
        return -1;
#if defined(NP2_AUDIO86_PROFILE)
    if (state->profile_now_us != NULL)
        state->profile_mix_us +=
            state->profile_now_us(state->profile_clock_opaque) - profile_begin;
#endif
    state->rendered_frames += frames;
    return 0;
}

int np2audio86_core_guest_action_kind_for_opcode(uint32_t opcode,
                                                  uint8_t *kind)
{
    if (kind == NULL)
        return -1;
    switch (opcode) {
    case CORE_GUEST_OPNA_REGISTER:
        *kind = NP2_AUDIO86_CORE_ACTION_OPNA_REGISTER;
        return 0;
    case CORE_GUEST_OPNA_CSM:
        *kind = NP2_AUDIO86_CORE_ACTION_OPNA_CSM;
        return 0;
    case CORE_GUEST_PCM_CONTROL:
        *kind = NP2_AUDIO86_CORE_ACTION_PCM_CONTROL;
        return 0;
    case CORE_GUEST_RESET_BARRIER:
        *kind = NP2_AUDIO86_CORE_ACTION_RESET;
        return 0;
    default:
        return -1;
    }
}

int np2audio86_core_guest_action_apply(
    struct np2audio86_render_state *state,
    const struct np2audio86_core_guest_action *action,
    const uint8_t *data, size_t data_count)
{
    uint8_t expected_kind;
    if (state == NULL || action == NULL)
        return -1;
    if (action->kind == NP2_AUDIO86_CORE_ACTION_DATA_RUN) {
        if (action->opcode != CORE_GUEST_PCM_DATA ||
            action->byte_count == 0U ||
            action->byte_count > NP2_AUDIO86_ASYNC_MAX_DATA_RUN ||
            data == NULL || data_count != action->byte_count)
            return -1;
        return np2audio86_core_render_pcm86_push(state, data, data_count);
    }
    if (np2audio86_core_guest_action_kind_for_opcode(action->opcode,
                                                      &expected_kind) != 0 ||
        expected_kind != action->kind)
        return -1;
    switch (action->kind) {
    case NP2_AUDIO86_CORE_ACTION_OPNA_REGISTER:
        return np2audio86_core_render_apply_opna_register(
            state, (uint16_t)(action->payload >> 8U),
            (uint8_t)action->payload);
    case NP2_AUDIO86_CORE_ACTION_OPNA_CSM:
        return action->payload == 0U
                   ? np2audio86_core_render_apply_opna_csm(state)
                   : -1;
    case NP2_AUDIO86_CORE_ACTION_PCM_CONTROL:
        return np2audio86_core_render_apply_pcm86_control(
            state, (uint8_t)(action->payload >> 8U),
            (uint8_t)action->payload);
    default:
        return -1;
    }
}
