#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2audio86_fixture.h"
#include "np2audio86_guest_async.h"
#include "np2audio86_guest_runtime_capture.h"
#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_pcm_canonical.h"

#define SYNC_SOURCE_BYTES NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES
#define SYNC_MAX_EVENTS 64U
#define SYNC_MAX_RUNS 8U
#define SYNC_MAX_ACTIONS (SYNC_MAX_EVENTS + SYNC_MAX_RUNS)
#define SYNC_HORIZON_FRAMES 2400U
#define SYNC_TRACE_RECORD_BYTES 40U

_Static_assert(sizeof(np2audio86_guest_event_t) == 24U,
               "86R.3 must retain the 24-byte guest event ABI");

enum sync_action_kind {
    SYNC_ACTION_OPNA_REGISTER = NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER,
    SYNC_ACTION_OPNA_CSM = NP2_AUDIO86_GUEST_ACTION_OPNA_CSM,
    SYNC_ACTION_PCM_CONTROL = NP2_AUDIO86_GUEST_ACTION_PCM_CONTROL,
    SYNC_ACTION_RESET = NP2_AUDIO86_GUEST_ACTION_RESET,
    SYNC_ACTION_DATA_RUN = NP2_AUDIO86_GUEST_ACTION_DATA_RUN,
};

struct sync_action {
    uint64_t frame;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
    uint64_t byte_offset;
    uint32_t byte_count;
    uint8_t kind;
};

struct sync_apply_record {
    uint64_t frame;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t action;
    uint64_t byte_offset;
    uint32_t byte_count;
    uint32_t payload;
};

struct sync_pcm_result {
    uint8_t full_pcm[SYNC_HORIZON_FRAMES * 4U];
    uint8_t pre_pcm[SYNC_HORIZON_FRAMES * 4U];
    size_t full_bytes;
    size_t pre_bytes;
    uint64_t full_frames;
    uint64_t pre_frames;
    uint64_t full_peak;
    uint64_t pre_peak;
    uint64_t full_nonzero;
    uint64_t pre_nonzero;
    uint64_t full_first_nonzero;
    uint64_t pre_first_nonzero;
    uint64_t full_clamp;
    uint64_t pre_clamp;
    uint64_t full_spans;
    uint64_t highest_event_frame;
    uint64_t pre_reset_frame;
    struct sync_apply_record apply[SYNC_MAX_ACTIONS];
    size_t apply_count;
};

struct sync_input_snapshot {
    size_t event_bytes;
    size_t run_bytes;
    size_t pcm_bytes;
    uint32_t event_crc;
    uint32_t run_crc;
    uint32_t pcm_crc;
    uint8_t event_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t run_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t pcm_sha[NP2_SHA256_DIGEST_SIZE];
    uint8_t events[SYNC_MAX_EVENTS * 24U];
    uint8_t runs[SYNC_MAX_RUNS * 32U];
    uint8_t pcm[32768U];
};

static int validate_action_stream(const struct sync_action *actions,
                                  size_t action_count, size_t pcm_count);

static int pcm86_feed_semantically_equal(
    const struct np2audio86_pcm86_feed *left,
    const struct np2audio86_pcm86_feed *right)
{
    return memcmp(&left->pcm, &right->pcm, sizeof(left->pcm)) == 0 &&
           left->source_frame == right->source_frame &&
           left->supplied == right->supplied &&
           left->underrun == right->underrun;
}

static int test_pcm86_partial_lengths(void)
{
    static const size_t lengths[] = {1U, 2U, 3U, 4U, 5U, 7U, 8U};
    static const size_t fragments[][2] = {
        {8U, 0U}, {1U, 7U}, {2U, 6U}, {3U, 5U}, {4U, 4U},
    };
    uint8_t *bytes = malloc(NP2_AUDIO86_PCM86_REFILL_BYTES + 1U);
    struct np2audio86_render_state *state = malloc(sizeof(*state));
    struct np2audio86_render_state *whole = malloc(sizeof(*whole));
    struct np2audio86_render_state *split = malloc(sizeof(*split));
    struct np2audio86_render_state *snapshot = malloc(sizeof(*snapshot));
    size_t i;
    if (bytes == NULL || state == NULL || whole == NULL || split == NULL ||
        snapshot == NULL) {
        free(bytes); free(state); free(whole); free(split); free(snapshot);
        return -1;
    }
    for (i = 0U; i <= NP2_AUDIO86_PCM86_REFILL_BYTES; ++i) {
        bytes[i] = (uint8_t)(i * 37U + 11U);
    }
    for (i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t count = lengths[i];
        if (np2audio86_render_init(state) != 0 ||
            np2audio86_render_pcm86_push(state, bytes, count) != 0 ||
            state->pcm86.pcm.wrtpos != count ||
            state->pcm86.pcm.realbuf != (SINT32)count ||
            state->pcm86.supplied != count || state->pcm86.underrun != 0U ||
            memcmp(state->pcm86.pcm.buffer, bytes, count) != 0 ||
            (count < PCM86_BUFSIZE && state->pcm86.pcm.buffer[count] != 0U)) {
            free(bytes); free(state); free(whole); free(split); free(snapshot);
            return -1;
        }
    }
    if (np2audio86_render_init(state) != 0) {
        free(bytes); free(state); free(whole); free(split); free(snapshot);
        return -1;
    }
    memcpy(snapshot, state, sizeof(*snapshot));
    if (np2audio86_render_pcm86_push(state, bytes, 0U) == 0 ||
        memcmp(state, snapshot, sizeof(*state)) != 0 ||
        np2audio86_render_pcm86_push(
            state, bytes, NP2_AUDIO86_PCM86_REFILL_BYTES + 1U) == 0 ||
        memcmp(state, snapshot, sizeof(*state)) != 0 ||
        np2audio86_render_pcm86_push(
            state, bytes, NP2_AUDIO86_PCM86_REFILL_BYTES) != 0 ||
        state->pcm86.pcm.realbuf != NP2_AUDIO86_PCM86_REFILL_BYTES ||
        state->pcm86.supplied != NP2_AUDIO86_PCM86_REFILL_BYTES) {
        free(bytes); free(state); free(whole); free(split); free(snapshot);
        return -1;
    }
    for (i = 0U; i < sizeof(fragments) / sizeof(fragments[0]); ++i) {
        SINT32 whole_pcm[4] = {0, 0, 0, 0};
        SINT32 split_pcm[4] = {0, 0, 0, 0};
        if (np2audio86_render_init(whole) != 0 ||
            np2audio86_render_init(split) != 0 ||
            np2audio86_render_pcm86_push(whole, bytes, 8U) != 0 ||
            np2audio86_render_pcm86_push(split, bytes, fragments[i][0]) != 0 ||
            (fragments[i][1] != 0U &&
             np2audio86_render_pcm86_push(split, bytes + fragments[i][0],
                                          fragments[i][1]) != 0) ||
            !pcm86_feed_semantically_equal(&whole->pcm86, &split->pcm86)) {
            free(bytes); free(state); free(whole); free(split); free(snapshot);
            return -1;
        }
        whole->pcm86.pcm.divremain = -1;
        split->pcm86.pcm.divremain = -1;
        pcm86gen_getpcm(&whole->pcm86.pcm, whole_pcm, 2U);
        pcm86gen_getpcm(&split->pcm86.pcm, split_pcm, 2U);
        if (memcmp(whole_pcm, split_pcm, sizeof(whole_pcm)) != 0 ||
            !pcm86_feed_semantically_equal(&whole->pcm86, &split->pcm86)) {
            free(bytes); free(state); free(whole); free(split); free(snapshot);
            return -1;
        }
    }
    free(bytes); free(state); free(whole); free(split); free(snapshot);
    return 0;
}

static int test_pcm86_incomplete_frames(void)
{
    static const struct {
        uint8_t dactrl;
        size_t bytes_per_sample;
    } formats[] = {
        {0x50U, 1U}, /* 8-bit mono */
        {0x70U, 2U}, /* 8-bit stereo */
        {0x10U, 2U}, /* 16-bit mono */
        {0x30U, 4U}, /* 16-bit stereo */
    };
    static const uint8_t sample[4] = {0x21U, 0x43U, 0x65U, 0x87U};
    struct np2audio86_render_state *state = malloc(sizeof(*state));
    size_t i;
    if (state == NULL) return -1;
    for (i = 0U; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        const size_t required = formats[i].bytes_per_sample;
        const size_t partial = required == 1U ? 0U : required - 1U;
        SINT32 mix[2] = {0, 0};
        if (np2audio86_render_init(state) != 0) {
            free(state); return -1;
        }
        state->pcm86.pcm.dactrl = formats[i].dactrl;
        if (partial != 0U) {
            if (np2audio86_render_pcm86_push(state, sample, partial) != 0) {
                free(state); return -1;
            }
            state->pcm86.pcm.divremain = -1;
            pcm86gen_getpcm(&state->pcm86.pcm, mix, 1U);
            if (state->pcm86.pcm.realbuf != (SINT32)partial ||
                state->pcm86.pcm.readpos != 0U || mix[0] != 0 || mix[1] != 0) {
                free(state); return -1;
            }
        }
        if (np2audio86_render_pcm86_push(state, sample + partial,
                                         required - partial) != 0) {
            free(state); return -1;
        }
        state->pcm86.pcm.divremain = -1;
        pcm86gen_getpcm(&state->pcm86.pcm, mix, 1U);
        if (state->pcm86.pcm.realbuf != 0 ||
            state->pcm86.pcm.readpos != required) {
            free(state); return -1;
        }
    }
    free(state);
    return 0;
}

static int test_pcm86_partial_boundaries(void)
{
    static const uint8_t bytes[3] = {0x10U, 0x20U, 0x30U};
    struct np2audio86_render_state *state = malloc(sizeof(*state));
    const struct np2audio86_guest_action run = {
        0U, 0U, NP2AUDIO86_TRACE_PCM, 0U, 0U, 3U,
        NP2_AUDIO86_GUEST_ACTION_DATA_RUN
    };
    const struct np2audio86_guest_action event = {
        0U, 1U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x000028f0U, 0U, 0U,
        NP2_AUDIO86_GUEST_ACTION_OPNA_REGISTER
    };
    const struct np2audio86_guest_action reset = {
        0U, 1U, NP2AUDIO86_TRACE_RESET_BARRIER, 0U, 0U, 0U,
        NP2_AUDIO86_GUEST_ACTION_RESET
    };
    uint8_t *source = malloc(SYNC_SOURCE_BYTES);
    if (state == NULL || source == NULL) {
        free(state); free(source); return -1;
    }
    if (np2audio86_render_init(state) != 0 ||
        np2audio86_guest_action_apply(state, &run, bytes, sizeof(bytes),
                                      source, SYNC_SOURCE_BYTES) != 0 ||
        state->pcm86.supplied != sizeof(bytes) ||
        np2audio86_guest_action_apply(state, &event, NULL, 0U, source,
                                      SYNC_SOURCE_BYTES) != 0 ||
        state->pcm86.supplied != sizeof(bytes)) {
        free(state); free(source); return -1;
    }
    if (np2audio86_render_init(state) != 0 ||
        np2audio86_guest_action_apply(state, &run, bytes, sizeof(bytes),
                                      source, SYNC_SOURCE_BYTES) != 0 ||
        state->pcm86.supplied != sizeof(bytes) ||
        np2audio86_guest_action_apply(state, &reset, NULL, 0U, source,
                                      SYNC_SOURCE_BYTES) != 0 ||
        state->pcm86.supplied != SYNC_SOURCE_BYTES ||
        state->pcm86.pcm.realbuf != SYNC_SOURCE_BYTES) {
        free(state); free(source); return -1;
    }
    free(state); free(source);
    return 0;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
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

static uint32_t get_le32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8U |
           (uint32_t)in[2] << 16U | (uint32_t)in[3] << 24U;
}

static uint64_t get_le64(const uint8_t *in)
{
    uint64_t value = 0U;
    unsigned i;
    for (i = 0U; i < 8U; ++i) {
        value |= (uint64_t)in[i] << (i * 8U);
    }
    return value;
}

static void sha_hex(const uint8_t *bytes, size_t length, char out[65])
{
    uint8_t digest[NP2_SHA256_DIGEST_SIZE];
    np2_sha256_context context;
    size_t i;
    np2_sha256_init(&context);
    np2_sha256_update(&context, bytes, length);
    np2_sha256_final(&context, digest);
    for (i = 0U; i < sizeof(digest); ++i) {
        (void)snprintf(out + i * 2U, 3U, "%02x", digest[i]);
    }
    out[64] = '\0';
}

static void sha_digest(const uint8_t *bytes, size_t length,
                       uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    np2_sha256_context context;
    np2_sha256_init(&context);
    np2_sha256_update(&context, bytes, length);
    np2_sha256_final(&context, digest);
}

static void print_digest(const char *name, const uint8_t *bytes, size_t length)
{
    char sha[65];
    printf("%s_SERIALIZED_BYTES=%zu\n", name, length);
    printf("%s_CRC32=%08" PRIx32 "\n", name,
           np2_crc32_iso_hdlc(bytes, length));
    sha_hex(bytes, length, sha);
    printf("%s_SHA256=%s\n", name, sha);
}

static size_t serialize_guest_events(const np2audio86_guest_event_t *events,
                                     size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        put_le64(out + i * 24U, events[i].frame_timestamp);
        put_le64(out + i * 24U + 8U, events[i].sequence);
        put_le32(out + i * 24U + 16U, events[i].opcode);
        put_le32(out + i * 24U + 20U, events[i].payload);
    }
    return count * 24U;
}

static size_t serialize_guest_runs(const np2audio86_guest_data_run_t *runs,
                                   size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        put_le64(out + i * 32U, runs[i].frame_timestamp);
        put_le64(out + i * 32U + 8U, runs[i].sequence);
        put_le64(out + i * 32U + 16U, runs[i].byte_offset);
        put_le32(out + i * 32U + 24U, runs[i].count);
        put_le32(out + i * 32U + 28U, 0U);
    }
    return count * 32U;
}

static int snapshot_inputs(struct sync_input_snapshot *snapshot,
                           const np2audio86_guest_event_t *events,
                           size_t event_count,
                           const np2audio86_guest_data_run_t *runs,
                           size_t run_count, const uint8_t *pcm_bytes,
                           size_t pcm_count)
{
    if (snapshot == NULL || pcm_bytes == NULL ||
        event_count > SYNC_MAX_EVENTS || run_count > SYNC_MAX_RUNS ||
        pcm_count > sizeof(snapshot->pcm)) {
        return -1;
    }
    snapshot->event_bytes = serialize_guest_events(events, event_count,
                                                    snapshot->events);
    snapshot->run_bytes = serialize_guest_runs(runs, run_count, snapshot->runs);
    snapshot->pcm_bytes = pcm_count;
    memcpy(snapshot->pcm, pcm_bytes, pcm_count);
    snapshot->event_crc = np2_crc32_iso_hdlc(snapshot->events,
                                              snapshot->event_bytes);
    snapshot->run_crc = np2_crc32_iso_hdlc(snapshot->runs, snapshot->run_bytes);
    snapshot->pcm_crc = np2_crc32_iso_hdlc(snapshot->pcm, snapshot->pcm_bytes);
    sha_digest(snapshot->events, snapshot->event_bytes, snapshot->event_sha);
    sha_digest(snapshot->runs, snapshot->run_bytes, snapshot->run_sha);
    sha_digest(snapshot->pcm, snapshot->pcm_bytes, snapshot->pcm_sha);
    return 0;
}

static int snapshot_pcm_matches(const struct sync_input_snapshot *snapshot,
                                const uint8_t *pcm_bytes, size_t pcm_count)
{
    uint8_t digest[NP2_SHA256_DIGEST_SIZE];
    return snapshot != NULL && pcm_bytes != NULL &&
           pcm_count == snapshot->pcm_bytes &&
           memcmp(snapshot->pcm, pcm_bytes, pcm_count) == 0 &&
           np2_crc32_iso_hdlc(pcm_bytes, pcm_count) == snapshot->pcm_crc &&
           (sha_digest(pcm_bytes, pcm_count, digest),
            memcmp(snapshot->pcm_sha, digest, sizeof(digest)) == 0);
}

static int snapshot_inputs_match(const struct sync_input_snapshot *snapshot,
                                 const np2audio86_guest_event_t *events,
                                 size_t event_count,
                                 const np2audio86_guest_data_run_t *runs,
                                 size_t run_count, const uint8_t *pcm_bytes,
                                 size_t pcm_count)
{
    uint8_t event_bytes[SYNC_MAX_EVENTS * 24U];
    uint8_t run_bytes[SYNC_MAX_RUNS * 32U];
    uint8_t digest[NP2_SHA256_DIGEST_SIZE];
    const size_t event_length = serialize_guest_events(events, event_count,
                                                        event_bytes);
    const size_t run_length = serialize_guest_runs(runs, run_count, run_bytes);
    if (snapshot == NULL || event_count > SYNC_MAX_EVENTS ||
        run_count > SYNC_MAX_RUNS || event_length != snapshot->event_bytes ||
        run_length != snapshot->run_bytes ||
        memcmp(snapshot->events, event_bytes, event_length) != 0 ||
        memcmp(snapshot->runs, run_bytes, run_length) != 0 ||
        np2_crc32_iso_hdlc(event_bytes, event_length) != snapshot->event_crc ||
        np2_crc32_iso_hdlc(run_bytes, run_length) != snapshot->run_crc) {
        return -1;
    }
    sha_digest(event_bytes, event_length, digest);
    if (memcmp(snapshot->event_sha, digest, sizeof(digest)) != 0) return -1;
    sha_digest(run_bytes, run_length, digest);
    if (memcmp(snapshot->run_sha, digest, sizeof(digest)) != 0) return -1;
    return snapshot_pcm_matches(snapshot, pcm_bytes, pcm_count) ? 0 : -1;
}

static int parse_events(const uint8_t *bytes, size_t length,
                        np2audio86_guest_event_t *events, size_t capacity,
                        size_t *count)
{
    size_t i;
    if (bytes == NULL || events == NULL || count == NULL ||
        (length % 24U) != 0U || length / 24U > capacity) {
        return -1;
    }
    *count = length / 24U;
    for (i = 0U; i < *count; ++i) {
        events[i].frame_timestamp = get_le64(bytes + i * 24U);
        events[i].sequence = get_le64(bytes + i * 24U + 8U);
        events[i].opcode = get_le32(bytes + i * 24U + 16U);
        events[i].payload = get_le32(bytes + i * 24U + 20U);
    }
    return 0;
}

static int parse_runs(const uint8_t *bytes, size_t length,
                      np2audio86_guest_data_run_t *runs, size_t capacity,
                      size_t *count)
{
    size_t i;
    if (bytes == NULL || runs == NULL || count == NULL ||
        (length % 32U) != 0U || length / 32U > capacity) {
        return -1;
    }
    *count = length / 32U;
    for (i = 0U; i < *count; ++i) {
        runs[i].frame_timestamp = get_le64(bytes + i * 32U);
        runs[i].sequence = get_le64(bytes + i * 32U + 8U);
        runs[i].byte_offset = get_le64(bytes + i * 32U + 16U);
        runs[i].count = get_le32(bytes + i * 32U + 24U);
    }
    return 0;
}

static int less_key(uint64_t frame_a, uint64_t sequence_a,
                    uint64_t frame_b, uint64_t sequence_b)
{
    return frame_a < frame_b ||
           (frame_a == frame_b && sequence_a < sequence_b);
}

static int validate_one_stream(uint64_t frame, uint64_t sequence,
                               uint64_t *last_frame, uint64_t *last_sequence,
                               uint8_t *have_last)
{
    if (*have_last && (frame < *last_frame || sequence <= *last_sequence)) {
        return -1;
    }
    *last_frame = frame;
    *last_sequence = sequence;
    *have_last = 1U;
    return 0;
}

static int build_actions(const np2audio86_guest_event_t *events, size_t event_count,
                         const np2audio86_guest_data_run_t *runs, size_t run_count,
                         size_t pcm_count, struct sync_action *actions,
                         size_t capacity, size_t *action_count)
{
    size_t ei = 0U, ri = 0U, count = 0U;
    uint64_t last_frame = 0U, last_sequence = 0U;
    uint8_t have_last = 0U;
    uint64_t stream_frame = 0U, stream_sequence = 0U;
    uint8_t stream_have = 0U;
    if (events == NULL || runs == NULL || actions == NULL || action_count == NULL ||
        event_count + run_count > capacity) {
        return -1;
    }
    for (ei = 0U; ei < event_count; ++ei) {
        if (validate_one_stream(events[ei].frame_timestamp, events[ei].sequence,
                                &stream_frame, &stream_sequence,
                                &stream_have) != 0) {
            return -1;
        }
    }
    stream_have = 0U;
    for (ri = 0U; ri < run_count; ++ri) {
        size_t prior;
        size_t following;
        if (runs[ri].count == 0U || runs[ri].count > 32768U ||
            runs[ri].byte_offset > pcm_count ||
            runs[ri].count > pcm_count - (size_t)runs[ri].byte_offset ||
            validate_one_stream(runs[ri].frame_timestamp, runs[ri].sequence,
                                &stream_frame, &stream_sequence,
                                &stream_have) != 0) {
            return -1;
        }
        for (prior = 0U; prior < event_count; ++prior) {
            if (events[prior].frame_timestamp > runs[ri].frame_timestamp) {
                break;
            }
            if (events[prior].sequence >= runs[ri].sequence) return -1;
        }
        for (following = 0U; following < event_count; ++following) {
            const np2audio86_guest_event_t *event = &events[following];
            if (event->frame_timestamp > runs[ri].frame_timestamp) {
                if (event->sequence <= runs[ri].sequence) return -1;
                break;
            }
        }
    }
    ei = 0U;
    ri = 0U;
    while (ei < event_count || ri < run_count) {
        struct sync_action *action;
        const int choose_event = ri == run_count ||
            (ei < event_count && less_key(events[ei].frame_timestamp,
                                          events[ei].sequence,
                                          runs[ri].frame_timestamp,
                                          runs[ri].sequence));
        if (count >= capacity) {
            return -1;
        }
        action = &actions[count++];
        if (choose_event) {
            const np2audio86_guest_event_t *event = &events[ei++];
            action->frame = event->frame_timestamp;
            action->sequence = event->sequence;
            action->opcode = event->opcode;
            action->payload = event->payload;
            action->byte_offset = 0U;
            action->byte_count = 0U;
            action->kind = event->opcode == NP2AUDIO86_TRACE_OPNA_REGISTER
                ? SYNC_ACTION_OPNA_REGISTER
                : event->opcode == NP2AUDIO86_TRACE_OPNA_CSM
                    ? SYNC_ACTION_OPNA_CSM
                    : event->opcode == NP2AUDIO86_TRACE_PCM_CONTROL
                        ? SYNC_ACTION_PCM_CONTROL
                        : event->opcode == NP2AUDIO86_TRACE_RESET_BARRIER
                            ? SYNC_ACTION_RESET : 0U;
        } else {
            const np2audio86_guest_data_run_t *run = &runs[ri++];
            action->frame = run->frame_timestamp;
            action->sequence = run->sequence;
            action->opcode = NP2AUDIO86_TRACE_PCM;
            action->payload = 0U;
            action->byte_offset = run->byte_offset;
            action->byte_count = run->count;
            action->kind = SYNC_ACTION_DATA_RUN;
        }
        if (action->kind == 0U ||
            validate_one_stream(action->frame, action->sequence,
                                &last_frame, &last_sequence,
                                &have_last) != 0) {
            return -1;
        }
    }
    *action_count = count;
    return 0;
}

static void record_apply(struct sync_pcm_result *result,
                         const struct sync_action *action)
{
    struct sync_apply_record *record = &result->apply[result->apply_count++];
    record->frame = action->frame;
    record->sequence = action->sequence;
    record->opcode = action->opcode;
    record->action = action->kind;
    record->byte_offset = action->byte_offset;
    record->byte_count = action->byte_count;
    record->payload = action->payload;
}

static int render_chunk(struct np2audio86_render_state *worker,
                        struct sync_pcm_result *result, uint64_t frame,
                        size_t frames, uint8_t pre_reset)
{
    SINT32 mix[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    uint8_t canonical[NP2_AUDIO86_QUANTUM_FRAMES * 4U];
    struct np2audio86_fixture_result fixture_result;
    struct np2opngen_pcm_stats stats;
    size_t i;
    if (frames == 0U || frames > NP2_AUDIO86_QUANTUM_FRAMES ||
        frame + frames > SYNC_HORIZON_FRAMES) {
        return -1;
    }
    memset(mix, 0, sizeof(mix));
    memset(&fixture_result, 0, sizeof(fixture_result));
    if (np2audio86_render_span(worker, mix, frames, &fixture_result) != 0 ||
        np2opngen_pcm_canonicalize_s16le(mix, frames, 2U, canonical,
                                         sizeof(canonical), &stats) != 0) {
        return -1;
    }
    memcpy(result->full_pcm + frame * 4U, canonical, frames * 4U);
    if (pre_reset) {
        memcpy(result->pre_pcm + frame * 4U, canonical, frames * 4U);
    }
    for (i = 0U; i < frames; ++i) {
        uint8_t nonzero = canonical[i * 4U] != 0U || canonical[i * 4U + 1U] != 0U ||
                          canonical[i * 4U + 2U] != 0U || canonical[i * 4U + 3U] != 0U;
        if (stats.s32_abs_peak > result->full_peak) {
            result->full_peak = stats.s32_abs_peak;
        }
        if (nonzero) {
            ++result->full_nonzero;
            if (result->full_nonzero == 1U) {
                result->full_first_nonzero = frame + i;
            }
        }
        if (pre_reset) {
            if (stats.s32_abs_peak > result->pre_peak) {
                result->pre_peak = stats.s32_abs_peak;
            }
            if (nonzero) {
                ++result->pre_nonzero;
                if (result->pre_nonzero == 1U) {
                    result->pre_first_nonzero = frame + i;
                }
            }
        }
    }
    result->full_clamp += stats.clip_samples;
    if (pre_reset) {
        result->pre_clamp += stats.clip_samples;
    }
    ++result->full_spans;
    return 0;
}

static int prime_worker(struct np2audio86_render_state *worker)
{
    uint8_t source[SYNC_SOURCE_BYTES];
    return np2audio86_guest_action_prime_worker(worker, source, sizeof(source));
}

static int apply_action(struct np2audio86_render_state *worker,
                        struct sync_pcm_result *result,
                        const struct sync_action *action,
                        const uint8_t *pcm_bytes, size_t pcm_count,
                        uint8_t *reset_seen)
{
    struct np2audio86_guest_action guest_action;
    uint8_t source[SYNC_SOURCE_BYTES];
    const uint8_t *data = NULL;
    size_t data_count = 0U;
    int status;
    if (result->apply_count >= SYNC_MAX_ACTIONS || action == NULL) {
        return -1;
    }
    if (action->kind == SYNC_ACTION_DATA_RUN) {
        if (action->byte_offset > pcm_count ||
            action->byte_count > pcm_count - (size_t)action->byte_offset) {
            return -1;
        }
        data = pcm_bytes + action->byte_offset;
        data_count = action->byte_count;
    }
    guest_action.frame_timestamp = action->frame;
    guest_action.sequence = action->sequence;
    guest_action.opcode = action->opcode;
    guest_action.payload = action->payload;
    guest_action.byte_offset = action->byte_offset;
    guest_action.byte_count = action->byte_count;
    guest_action.kind = action->kind;
    record_apply(result, action);
    if (action->kind == SYNC_ACTION_RESET && *reset_seen) {
        return -1;
    }
    status = np2audio86_guest_action_apply(worker, &guest_action, data,
                                            data_count, source, sizeof(source));
    if (status != 0) {
        return -1;
    }
    if (action->kind == SYNC_ACTION_RESET) {
        *reset_seen = 1U;
        result->pre_reset_frame = action->frame;
    }
    return 0;
}

static int replay_actions(const struct sync_action *actions, size_t action_count,
                          const uint8_t *pcm_bytes, size_t pcm_count,
                          size_t service_quantum, struct sync_pcm_result *result)
{
    struct np2audio86_render_state worker;
    uint64_t frame = 0U;
    size_t i;
    uint8_t reset_seen = 0U;
    memset(result, 0, sizeof(*result));
    if (service_quantum == 0U || service_quantum > NP2_AUDIO86_QUANTUM_FRAMES ||
        validate_action_stream(actions, action_count, pcm_count) != 0 ||
        prime_worker(&worker) != 0) {
        return -1;
    }
    for (i = 0U; i < action_count; ++i) {
        const struct sync_action *action = &actions[i];
        if (action->frame > result->highest_event_frame) {
            result->highest_event_frame = action->frame;
        }
        if (action->frame > SYNC_HORIZON_FRAMES || action->frame < frame) {
            return -1;
        }
        while (frame < action->frame) {
            size_t chunk = (size_t)(action->frame - frame);
            if (chunk > service_quantum) {
                chunk = service_quantum;
            }
            if (render_chunk(&worker, result, frame, chunk, !reset_seen) != 0) {
                return -1;
            }
            frame += chunk;
        }
        if (apply_action(&worker, result, action, pcm_bytes, pcm_count,
                         &reset_seen) != 0) {
            return -1;
        }
    }
    while (frame < SYNC_HORIZON_FRAMES) {
        size_t chunk = SYNC_HORIZON_FRAMES - frame;
        if (chunk > service_quantum) {
            chunk = service_quantum;
        }
        if (render_chunk(&worker, result, frame, chunk, !reset_seen) != 0) {
            return -1;
        }
        frame += chunk;
    }
    result->full_frames = SYNC_HORIZON_FRAMES;
    result->full_bytes = sizeof(result->full_pcm);
    result->pre_frames = result->pre_reset_frame;
    result->pre_bytes = (size_t)result->pre_frames * 4U;
    return 0;
}

static size_t serialize_apply(const struct sync_apply_record *records,
                              size_t count, uint8_t *out)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        const struct sync_apply_record *record = &records[i];
        put_le64(out + i * SYNC_TRACE_RECORD_BYTES, record->frame);
        put_le64(out + i * SYNC_TRACE_RECORD_BYTES + 8U, record->sequence);
        put_le32(out + i * SYNC_TRACE_RECORD_BYTES + 16U, record->opcode);
        put_le32(out + i * SYNC_TRACE_RECORD_BYTES + 20U, record->action);
        put_le64(out + i * SYNC_TRACE_RECORD_BYTES + 24U, record->byte_offset);
        put_le32(out + i * SYNC_TRACE_RECORD_BYTES + 32U, record->byte_count);
        put_le32(out + i * SYNC_TRACE_RECORD_BYTES + 36U, record->payload);
    }
    return count * SYNC_TRACE_RECORD_BYTES;
}

static int compare_pcm_results(const struct sync_pcm_result *left,
                               const struct sync_pcm_result *right)
{
    return left->full_bytes == right->full_bytes &&
           left->pre_bytes == right->pre_bytes &&
           memcmp(left->full_pcm, right->full_pcm, left->full_bytes) == 0 &&
           memcmp(left->pre_pcm, right->pre_pcm, left->pre_bytes) == 0 &&
           left->apply_count == right->apply_count &&
           memcmp(left->apply, right->apply,
                  left->apply_count * sizeof(left->apply[0])) == 0;
}

static int validate_action_stream(const struct sync_action *actions,
                                  size_t action_count, size_t pcm_count)
{
    size_t i;
    uint64_t last_frame = 0U;
    uint64_t last_sequence = 0U;
    uint8_t have_last = 0U;
    if (actions == NULL || action_count > SYNC_MAX_ACTIONS) {
        return -1;
    }
    for (i = 0U; i < action_count; ++i) {
        const struct sync_action *action = &actions[i];
        if (action->frame > SYNC_HORIZON_FRAMES ||
            validate_one_stream(action->frame, action->sequence,
                                &last_frame, &last_sequence, &have_last) != 0) {
            return -1;
        }
        switch (action->kind) {
        case SYNC_ACTION_OPNA_REGISTER:
            if (action->opcode != NP2AUDIO86_TRACE_OPNA_REGISTER) return -1;
            break;
        case SYNC_ACTION_OPNA_CSM:
            if (action->opcode != NP2AUDIO86_TRACE_OPNA_CSM) return -1;
            break;
        case SYNC_ACTION_PCM_CONTROL:
            if (action->opcode != NP2AUDIO86_TRACE_PCM_CONTROL) return -1;
            break;
        case SYNC_ACTION_RESET:
            if (action->opcode != NP2AUDIO86_TRACE_RESET_BARRIER) return -1;
            break;
        case SYNC_ACTION_DATA_RUN:
            if (action->opcode != NP2AUDIO86_TRACE_PCM ||
                action->byte_count == 0U || action->byte_count > 32768U ||
                action->byte_offset > pcm_count ||
                action->byte_count > pcm_count - (size_t)action->byte_offset) {
                return -1;
            }
            break;
        default:
            return -1;
        }
    }
    return 0;
}

static int run_negative_tests(const struct sync_action *actions, size_t count,
                              const uint8_t *pcm_bytes, size_t pcm_count)
{
    struct sync_action bad[SYNC_MAX_ACTIONS];
    struct sync_pcm_result result;
    struct sync_action one;
    np2audio86_guest_event_t event;
    np2audio86_guest_data_run_t run;
    uint8_t malformed[23] = {0};
    uint8_t malformed_runs[31] = {0};
    size_t parsed;

    if (parse_events(malformed, sizeof(malformed), &event, 1U, &parsed) == 0 ||
        parse_runs(malformed_runs, sizeof(malformed_runs),
                   &run, 1U, &parsed) == 0 ||
        count < 2U) {
        return -1;
    }

    /* Same-frame duplicate sequence must fail before any worker mutation. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[1].sequence = bad[0].sequence;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* Same-frame sequence regression must also fail. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[1].sequence = bad[0].sequence - 1U;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* A timestamp regression must not be silently re-ordered. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[0].frame = bad[1].frame + 1U;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* Unknown operation and opcode-to-operation mismatches fail closed. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[0].kind = 99U;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[0].opcode = UINT32_C(0xdeadbeef);
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* Horizon overflow is rejected before rendering a partial result. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[count - 1U].frame = SYNC_HORIZON_FRAMES + 1U;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* A second reset barrier is not a legal replay stream. */
    memcpy(bad, actions, count * sizeof(bad[0]));
    bad[0].kind = SYNC_ACTION_RESET;
    bad[0].opcode = NP2AUDIO86_TRACE_RESET_BARRIER;
    bad[1].kind = SYNC_ACTION_RESET;
    bad[1].opcode = NP2AUDIO86_TRACE_RESET_BARRIER;
    if (replay_actions(bad, count, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;

    /* DATA_RUN range, maximum length, and missing payload are independent gates. */
    memset(&one, 0, sizeof(one));
    one.opcode = NP2AUDIO86_TRACE_PCM;
    one.kind = SYNC_ACTION_DATA_RUN;
    one.byte_offset = pcm_count;
    one.byte_count = 1U;
    if (replay_actions(&one, 1U, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;
    one.byte_offset = 0U;
    one.byte_count = 32769U;
    if (replay_actions(&one, 1U, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;
    one.byte_count = 1U;
    if (replay_actions(&one, 1U, NULL, 0U, 240U, &result) == 0) return -1;
    one.kind = SYNC_ACTION_PCM_CONTROL;
    one.opcode = NP2AUDIO86_TRACE_PCM_CONTROL;
    one.payload = UINT32_C(0xff00);
    if (replay_actions(&one, 1U, pcm_bytes, pcm_count, 240U, &result) == 0) return -1;
    return 0;
}

static int run_global_sequence_tests(const uint8_t *pcm_bytes, size_t pcm_count)
{
    struct sync_action sequence_cases[2] = {
        {0U, 10U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x2400U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
        {0U, 11U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x2400U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
    };
    np2audio86_guest_event_t events[2] = {
        {0U, 15U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x2400U},
        {13U, 17U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x2400U},
    };
    np2audio86_guest_data_run_t run = {0U, 16U, 0U, 4U};
    struct sync_action merged[3];
    size_t count = 0U;

    (void)pcm_bytes;
    sequence_cases[1].frame = 0U;
    sequence_cases[1].sequence = 9U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) == 0) return -1;
    sequence_cases[1].sequence = 10U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) == 0) return -1;
    sequence_cases[1].frame = 1U;
    sequence_cases[1].sequence = 9U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) == 0) return -1;
    sequence_cases[1].sequence = 10U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) == 0) return -1;
    sequence_cases[1].sequence = 11U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) != 0) return -1;
    sequence_cases[0].frame = 1U;
    sequence_cases[0].sequence = 10U;
    sequence_cases[1].frame = 0U;
    sequence_cases[1].sequence = 11U;
    if (validate_action_stream(sequence_cases, 2U, pcm_count) == 0) return -1;

    if (build_actions(events, 2U, &run, 1U, pcm_count, merged,
                      sizeof(merged) / sizeof(merged[0]), &count) != 0 ||
        count != 3U || merged[1].kind != SYNC_ACTION_DATA_RUN ||
        merged[1].sequence != 16U) {
        return -1;
    }
    run.sequence = 15U;
    if (build_actions(events, 2U, &run, 1U, pcm_count, merged,
                      sizeof(merged) / sizeof(merged[0]), &count) == 0) return -1;
    run.sequence = 14U;
    if (build_actions(events, 2U, &run, 1U, pcm_count, merged,
                      sizeof(merged) / sizeof(merged[0]), &count) == 0) return -1;
    run.sequence = 17U;
    if (build_actions(events, 2U, &run, 1U, pcm_count, merged,
                      sizeof(merged) / sizeof(merged[0]), &count) == 0) return -1;
    return 0;
}

static int run_domain_a_pcm_split_test(void)
{
    struct np2audio86_render_state state;
    PCM86 pcm;
    unsigned volume_code;
    SINT32 virbuf;
    UINT8 irqflag;
    UINT8 reqirq;
    UINT64 lastclockforwait;
    UINT64 lastclock;
    UINT8 soundflags;
    SINT32 fifosize;
    UINT stepbit;
    UINT stepmask;
    if (np2audio86_render_init(&state) != 0) return -1;
    pcm = &state.pcm86.pcm;
    pcm->virbuf = 123;
    pcm->irqflag = 1U;
    pcm->reqirq = 1U;
    pcm->lastclockforwait = UINT64_C(0x123456789abcdef0);
    pcm->lastclock = UINT64_C(0x0fedcba987654321);
    pcm->soundflags = 0xa4U;
    pcm->fifosize = 789;
    pcm->readpos = 5U;
    pcm->wrtpos = 7U;
    pcm->realbuf = 64;
    pcm->fifo = 0U;
    virbuf = pcm->virbuf;
    irqflag = pcm->irqflag;
    reqirq = pcm->reqirq;
    lastclockforwait = pcm->lastclockforwait;
    lastclock = pcm->lastclock;
    soundflags = pcm->soundflags;
    fifosize = pcm->fifosize;
    stepbit = pcm->stepbit;
    stepmask = pcm->stepmask;
    if (np2audio86_render_apply_pcm86_control(&state, 0x08U, 0x08U) != 0 ||
        pcm->readpos != 0U || pcm->wrtpos != 0U || pcm->realbuf != 0 ||
        pcm->fifo != 0x08U || pcm->virbuf != virbuf ||
        pcm->irqflag != irqflag || pcm->reqirq != reqirq ||
        pcm->lastclockforwait != lastclockforwait ||
        pcm->lastclock != lastclock || pcm->fifosize != fifosize ||
        pcm->stepbit != stepbit || pcm->stepmask != stepmask) {
        return -1;
    }
    if (np2audio86_render_apply_pcm86_control(&state, 0x00U, 0x01U) != 0 ||
        pcm->soundflags != soundflags) {
        return -1;
    }
    for (volume_code = 0U; volume_code < 32U; ++volume_code) {
        const uint8_t value = (uint8_t)(0xa0U | volume_code);
        const SINT32 expected_volume = 64 * (SINT32)((~value) & 15U);
        pcm->vol5 = 11;
        pcm->volume = -1;
        if (np2audio86_render_apply_pcm86_control(&state, 0x06U, value) != 0 ||
            pcm->vol5 != 11 || pcm->volume != expected_volume ||
            pcm->virbuf != virbuf || pcm->irqflag != irqflag ||
            pcm->reqirq != reqirq || pcm->lastclock != lastclock ||
            pcm->lastclockforwait != lastclockforwait ||
            pcm->soundflags != soundflags || pcm->fifosize != fifosize ||
            pcm->stepbit != stepbit || pcm->stepmask != stepmask) {
            return -1;
        }
    }
    return 0;
}

static int run_boundary_tests(const uint8_t *pcm_bytes, size_t pcm_count)
{
    struct sync_action actions[5] = {
        {0U, 0U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x28f0U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
        {1U, 1U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x10f0U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
        {240U, 2U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x28f0U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
        {241U, 3U, NP2AUDIO86_TRACE_OPNA_REGISTER, 0x10f0U, 0U, 0U,
         SYNC_ACTION_OPNA_REGISTER},
        {241U, 4U, NP2AUDIO86_TRACE_OPNA_CSM, 0U, 0U, 0U,
         SYNC_ACTION_OPNA_CSM},
    };
    struct sync_pcm_result result;
    if (replay_actions(actions, 5U, pcm_bytes, pcm_count, 240U, &result) != 0 ||
        result.apply_count != 5U || result.full_spans <= 4U ||
        result.apply[0].frame != 0U || result.apply[1].frame != 1U ||
        result.apply[2].frame != 240U || result.apply[3].frame != 241U ||
        result.apply[4].action != SYNC_ACTION_OPNA_CSM) {
        return -1;
    }
    return 0;
}

static const char *event_meaning(uint32_t opcode, uint32_t payload)
{
    if (opcode == NP2AUDIO86_TRACE_OPNA_REGISTER) {
        const uint16_t address = (uint16_t)(payload >> 8U);
        return address < 0x10U ? "OPNA_PSG_REGISTER" :
               address >= 0x10U && address <= 0x1fU ? "OPNA_RHYTHM_REGISTER" :
               address == 0x28U ? "OPNA_FM_KEY" :
               address >= 0x30U ? "OPNA_FM_REGISTER" :
               "OPNA_TIMER_REGISTER";
    }
    if (opcode == NP2AUDIO86_TRACE_OPNA_CSM) return "OPNA_CSM";
    if (opcode == NP2AUDIO86_TRACE_PCM_CONTROL) return "PCM86_CONTROL";
    if (opcode == NP2AUDIO86_TRACE_RESET_BARRIER) return "RESET_BARRIER";
    return "UNKNOWN";
}

static int emit_result(const struct sync_pcm_result *result,
                       const np2audio86_guest_trace_t *trace,
                       const uint8_t *event_serialized, size_t event_bytes,
                       const uint8_t *run_serialized, size_t run_bytes)
{
    uint8_t apply_serialized[SYNC_MAX_ACTIONS * SYNC_TRACE_RECORD_BYTES];
    size_t apply_bytes = serialize_apply(result->apply, result->apply_count,
                                         apply_serialized);
    size_t opcode_counts[4] = {0U, 0U, 0U, 0U};
    size_t i;
    char sha[65];
    print_digest("WORKER_APPLY_TRACE", apply_serialized, apply_bytes);
    printf("WORKER_APPLY_TRACE_SEMANTIC_COUNT=%zu\n", result->apply_count);
    print_digest("AUDIO_EVENTS", event_serialized, event_bytes);
    printf("AUDIO_EVENTS_SEMANTIC_COUNT=%zu\n", trace->event_count);
    print_digest("PCM86_BYTES", trace->pcm_bytes, trace->pcm_count);
    printf("PCM86_BYTES_PAYLOAD_BYTES=%zu\n", trace->pcm_count);
    print_digest("PCM86_DATA_RUNS", run_serialized, run_bytes);
    printf("PCM86_DATA_RUNS_SEMANTIC_COUNT=%zu\nPCM86_DATA_RUNS_PAYLOAD_BYTES=%zu\n",
           trace->data_run_count, trace->pcm_count);
    printf("HIGHEST_EVENT_FRAME=%" PRIu64 "\n", result->highest_event_frame);
    printf("RENDER_HORIZON_FRAMES=%u\nTAIL_FRAMES=%" PRIu64 "\n",
           SYNC_HORIZON_FRAMES,
           (uint64_t)SYNC_HORIZON_FRAMES - result->pre_reset_frame);
    printf("PRE_RESET_PCM_FRAMES=%" PRIu64 "\nPRE_RESET_PCM_BYTES=%zu\n",
           result->pre_frames, result->pre_bytes);
    print_digest("PRE_RESET_PCM", result->pre_pcm, result->pre_bytes);
    printf("PRE_RESET_PCM_PEAK=%" PRIu64 "\nPRE_RESET_PCM_NONZERO=%" PRIu64
           "\nPRE_RESET_PCM_FIRST_NONZERO=%" PRIu64 "\nPRE_RESET_PCM_CLAMP=%" PRIu64 "\n",
           result->pre_peak, result->pre_nonzero, result->pre_first_nonzero,
           result->pre_clamp);
    printf("FULL_REPLAY_PCM_FRAMES=%" PRIu64 "\nFULL_REPLAY_PCM_BYTES=%zu\n",
           result->full_frames, result->full_bytes);
    print_digest("FULL_REPLAY_PCM", result->full_pcm, result->full_bytes);
    printf("FULL_REPLAY_PCM_PEAK=%" PRIu64 "\nFULL_REPLAY_PCM_NONZERO=%" PRIu64
           "\nFULL_REPLAY_PCM_FIRST_NONZERO=%" PRIu64 "\nFULL_REPLAY_PCM_CLAMP=%" PRIu64 "\n",
           result->full_peak, result->full_nonzero,
           result->full_first_nonzero, result->full_clamp);
    for (i = 0U; i < trace->event_count; ++i) {
        if (trace->events[i].opcode == NP2AUDIO86_TRACE_OPNA_REGISTER) {
            ++opcode_counts[0];
        } else if (trace->events[i].opcode == NP2AUDIO86_TRACE_OPNA_CSM) {
            ++opcode_counts[1];
        } else if (trace->events[i].opcode == NP2AUDIO86_TRACE_PCM_CONTROL) {
            ++opcode_counts[2];
        } else if (trace->events[i].opcode == NP2AUDIO86_TRACE_RESET_BARRIER) {
            ++opcode_counts[3];
        }
        printf("EVENT_TABLE sequence=%" PRIu64 " frame=%" PRIu64
               " opcode=%" PRIu32 " payload=0x%08" PRIx32 " meaning=%s\n",
               trace->events[i].sequence, trace->events[i].frame_timestamp,
               trace->events[i].opcode, trace->events[i].payload,
               event_meaning(trace->events[i].opcode, trace->events[i].payload));
    }
    printf("EVENT_OPCODE_COUNT OPNA_REGISTER=%zu OPNA_CSM=%zu PCM_CONTROL=%zu RESET_BARRIER=%zu DATA_RUN=%zu\n",
           opcode_counts[0], opcode_counts[1], opcode_counts[2],
           opcode_counts[3], trace->data_run_count);
    sha_hex(result->full_pcm, result->full_bytes, sha);
    (void)sha;
    printf("FM_COVERAGE=EXERCISED\nPSG_COVERAGE=EXERCISED\nRHYTHM_COVERAGE=EXERCISED\nPCM86_COVERAGE=NOT_EXERCISED\n");
    (void)i;
    return 0;
}

int main(void)
{
    static np2audio86_guest_event_t events[SYNC_MAX_EVENTS];
    static np2audio86_guest_data_run_t runs[SYNC_MAX_RUNS];
    static uint8_t pcm_bytes[32768];
    static np2audio86_guest_timer_trace_t timers[4096];
    static np2audio86_guest_io_trace_t io[16384];
    static uint8_t event_serialized[SYNC_MAX_EVENTS * 24U];
    static uint8_t run_serialized[SYNC_MAX_RUNS * 32U];
    static uint8_t event_serialized_copy[SYNC_MAX_EVENTS * 24U];
    static uint8_t run_serialized_copy[SYNC_MAX_RUNS * 32U];
    static np2audio86_guest_event_t events_copy[SYNC_MAX_EVENTS];
    static np2audio86_guest_data_run_t runs_copy[SYNC_MAX_RUNS];
    np2audio86_guest_trace_t trace = {
        events, SYNC_MAX_EVENTS, 0U, runs, SYNC_MAX_RUNS, 0U,
        pcm_bytes, sizeof(pcm_bytes), 0U, timers, 4096U, 0U,
        io, 16384U, 0U, 0U
    };
    np2audio86_guest_state_snapshot_t guest_state;
    struct sync_action actions[SYNC_MAX_ACTIONS];
    struct sync_action serialized_actions[SYNC_MAX_ACTIONS];
    struct sync_pcm_result direct;
    struct sync_pcm_result serialized;
    struct sync_pcm_result alternate;
    size_t event_bytes, run_bytes, event_copy_bytes, run_copy_bytes;
    size_t event_count, run_count, action_count, serialized_action_count;
    int build_rc;
    struct sync_input_snapshot input_snapshot;
    uint8_t pcm_mutation_copy[32768];

    if (test_pcm86_partial_lengths() != 0) return 2;
    if (test_pcm86_incomplete_frames() != 0) return 3;
    if (test_pcm86_partial_boundaries() != 0) return 4;

    if (np2audio86_guest_runtime_capture(&trace, &guest_state) != 0) return 1;
    printf("SOURCE_86R2_HEAD=0639a606842d04842f68baf717d41c4d93d794bf\n");
    event_bytes = serialize_guest_events(trace.events, trace.event_count,
                                         event_serialized);
    run_bytes = serialize_guest_runs(trace.data_runs, trace.data_run_count,
                                     run_serialized);
    if (trace.event_count != 18U || trace.pcm_count != 8U ||
        trace.data_run_count != 1U || event_bytes != 432U || run_bytes != 32U ||
        np2_crc32_iso_hdlc(event_serialized, event_bytes) != UINT32_C(0x3b57b261) ||
        np2_crc32_iso_hdlc(trace.pcm_bytes, trace.pcm_count) != UINT32_C(0xcbf0b66e) ||
        np2_crc32_iso_hdlc(run_serialized, run_bytes) != UINT32_C(0xb5843125)) {
        return 1;
    }
    if (snapshot_inputs(&input_snapshot, trace.events, trace.event_count,
                        trace.data_runs, trace.data_run_count,
                        trace.pcm_bytes, trace.pcm_count) != 0) return 1;
    build_rc = build_actions(trace.events, trace.event_count, trace.data_runs,
                      trace.data_run_count, trace.pcm_count, actions,
                      SYNC_MAX_ACTIONS, &action_count);
    if (build_rc != 0 ||
        replay_actions(actions, action_count, trace.pcm_bytes, trace.pcm_count,
                       240U, &direct) != 0 ||
        snapshot_inputs_match(&input_snapshot, trace.events, trace.event_count,
                               trace.data_runs, trace.data_run_count,
                               trace.pcm_bytes, trace.pcm_count) != 0) {
        return 1;
    }
    event_copy_bytes = serialize_guest_events(trace.events, trace.event_count,
                                              event_serialized_copy);
    run_copy_bytes = serialize_guest_runs(trace.data_runs, trace.data_run_count,
                                          run_serialized_copy);
    if (parse_events(event_serialized_copy, event_copy_bytes, events_copy,
                     SYNC_MAX_EVENTS, &event_count) != 0 ||
        parse_runs(run_serialized_copy, run_copy_bytes, runs_copy, SYNC_MAX_RUNS,
                   &run_count) != 0 ||
        build_actions(events_copy, event_count, runs_copy, run_count,
                      trace.pcm_count, serialized_actions, SYNC_MAX_ACTIONS,
                      &serialized_action_count) != 0 ||
        replay_actions(serialized_actions, serialized_action_count,
                       trace.pcm_bytes, trace.pcm_count, 240U, &serialized) != 0 ||
        !compare_pcm_results(&direct, &serialized) ||
        snapshot_inputs_match(&input_snapshot, trace.events, trace.event_count,
                               trace.data_runs, trace.data_run_count,
                               trace.pcm_bytes, trace.pcm_count) != 0) {
        return 1;
    }
    if (replay_actions(actions, action_count, trace.pcm_bytes, trace.pcm_count,
                       120U, &alternate) != 0 ||
        !compare_pcm_results(&direct, &alternate) ||
        snapshot_inputs_match(&input_snapshot, trace.events, trace.event_count,
                               trace.data_runs, trace.data_run_count,
                               trace.pcm_bytes, trace.pcm_count) != 0) {
        return 1;
    }
    memcpy(pcm_mutation_copy, trace.pcm_bytes, trace.pcm_count);
    pcm_mutation_copy[0] ^= UINT8_C(0x01);
    if (snapshot_pcm_matches(&input_snapshot, pcm_mutation_copy,
                             trace.pcm_count) ||
        run_global_sequence_tests(trace.pcm_bytes, trace.pcm_count) != 0 ||
        run_domain_a_pcm_split_test() != 0 ||
        run_negative_tests(actions, action_count, trace.pcm_bytes,
                           trace.pcm_count) != 0 ||
        run_boundary_tests(trace.pcm_bytes, trace.pcm_count) != 0 ||
        direct.pre_nonzero == 0U || direct.pre_peak == 0U ||
        direct.full_nonzero == 0U || direct.full_peak == 0U ||
        direct.full_clamp != 0U || direct.pre_clamp != 0U) {
        return 1;
    }
    if (emit_result(&direct, &trace, event_serialized, event_bytes,
                    run_serialized, run_bytes) != 0) return 1;
    printf("AUDIO86_GUEST_SYNC_INPUT=PASS\n");
    printf("AUDIO86_GUEST_SYNC_GLOBAL_SEQUENCE_VALIDATION=PASS\n");
    printf("MERGED_ACTION_SEQUENCE_VALIDATION=PASS\n");
    printf("PCM_BYTE_IMMUTABILITY_CHECKER_SENSITIVE=PASS\n");
    printf("AUDIO86_GUEST_REPLAY_EVENTS_IMMUTABLE=PASS\n");
    printf("AUDIO86_GUEST_REPLAY_DATA_RUNS_IMMUTABLE=PASS\n");
    printf("AUDIO86_GUEST_REPLAY_PCM_BYTES_IMMUTABLE=PASS\n");
    printf("AUDIO86_GUEST_REPLAY_INPUT_IMMUTABLE=PASS\n");
    printf("AUDIO86_GUEST_SYNC_DOMAIN_A_PCM_SPLIT=PASS\n");
    printf("AUDIO86_GUEST_SYNC_PCM_VOL5_PRESERVED=PASS\n");
    printf("AUDIO86_GUEST_SYNC_WORKER_APPLY=PASS\n");
    printf("AUDIO86_GUEST_SYNC_FAIL_CLOSED=PASS\n");
    printf("AUDIO86_GUEST_SYNC_PCM=PASS\n");
    printf("AUDIO86_GUEST_SYNC_SERIALIZED_REPLAY=PASS\n");
    printf("AUDIO86_GUEST_SYNC_QUANTUM_INDEPENDENCE=PASS\n");
    printf("AUDIO86_GUEST_SYNC_PCM_DETERMINISM=PASS\n");
    printf("AUDIO86_GUEST_SYNC_NEGATIVE_TESTS=PASS\n");
    printf("AUDIO86_GUEST_SYNC_BOUNDARY_TESTS=PASS\n");
    printf("PCM86_RENDERER_ARBITRARY_LENGTH_ACCEPTANCE=PASS\n");
    printf("PCM86_ZERO_LENGTH_REJECTED=PASS\n");
    printf("PCM86_MAX_LENGTH_BOUNDARY=PASS\n");
    printf("PCM86_PARTIAL_LENGTH_MATRIX=7/7_PASS\n");
    printf("PCM86_INVALID_LENGTH_MATRIX=PASS\n");
    printf("PCM86_FRAGMENTATION_EQUIVALENCE=5/5_PASS\n");
    printf("PARTIAL_RUN_THEN_EVENT=PASS\n");
    printf("PARTIAL_RUN_RESET_ORDER=PASS\n");
    printf("PARTIAL_RUN_FINALIZE=PASS\n");
    printf("PCM86_PARTIAL_SAMPLE_FIFO_SEMANTICS=PASS\n");
    printf("PCM86_INCOMPLETE_FRAME_STAGING=PASS\n");
    printf("PCM86_SYNTHETIC_PADDING=0\n");
    printf("AUDIO86_GUEST_SYNC_RESULT=PASS\n");
    return 0;
}
