#include <assert.h>
#include <string.h>

#include <np2opngen_synth_event.h>

struct recorder {
    unsigned render_calls;
    unsigned apply_calls;
    uint64_t cursors[8];
    uint64_t frames[8];
    uint64_t sequences[8];
};

struct order_recorder {
    uint8_t a4;
    uint16_t observed_frequency;
};

static int record_render(void *context, uint64_t cursor, uint64_t frames)
{
    struct recorder *recorder = (struct recorder *)context;
    assert(recorder->render_calls < 8U);
    recorder->cursors[recorder->render_calls] = cursor;
    recorder->frames[recorder->render_calls] = frames;
    ++recorder->render_calls;
    return 0;
}

static int record_apply(void *context,
                        const struct np2opngen_synth_event *event)
{
    struct recorder *recorder = (struct recorder *)context;
    assert(recorder->apply_calls < 8U);
    recorder->sequences[recorder->apply_calls] = event->sequence;
    ++recorder->apply_calls;
    return 0;
}

static int order_apply(void *context,
                       const struct np2opngen_synth_event *event)
{
    struct order_recorder *recorder = (struct order_recorder *)context;
    if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE &&
        event->payload.register_write.reg == 0xa4U) {
        recorder->a4 = event->payload.register_write.value;
    } else if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE &&
               event->payload.register_write.reg == 0xa0U) {
        recorder->observed_frequency =
            (uint16_t)(((uint16_t)recorder->a4 << 8) |
                       event->payload.register_write.value);
    }
    return 0;
}

static int order_render(void *context, uint64_t cursor, uint64_t frames)
{
    (void)context;
    (void)cursor;
    (void)frames;
    return 0;
}

static struct np2opngen_synth_event reg_event(uint64_t timestamp,
                                              uint64_t sequence,
                                              uint16_t reg, uint8_t value)
{
    struct np2opngen_synth_event event = {
        timestamp,
        sequence,
        NP2_SYNTH_EVENT_REGISTER_WRITE,
        { .register_write = { .chbase = 0U, .reg = reg, .value = value } },
    };
    return event;
}

static struct np2opngen_synth_event key_event(uint64_t timestamp,
                                              uint64_t sequence,
                                              uint8_t channel, uint8_t value)
{
    struct np2opngen_synth_event event = {
        timestamp,
        sequence,
        NP2_SYNTH_EVENT_KEY_EVENT,
        { .key_event = { channel, value, 0U } },
    };
    return event;
}

static void expect_invalid(const char *name,
                           const struct np2opngen_synth_event *events,
                           size_t count, uint64_t end_frame,
                           uint64_t first_sequence, uint64_t frame_capacity)
{
    struct recorder recorder = {0};
    struct np2opngen_synth_event_observer observer;
    int status = np2opngen_synth_event_interpret(
        events, count, end_frame, first_sequence, frame_capacity,
        record_render, record_apply, &recorder, &observer);
    assert(status != NP2_SYNTH_EVENT_STATUS_OK && name != 0);
    assert(recorder.render_calls == 0U);
    assert(recorder.apply_calls == 0U);
}

static void test_invalid_lists(void)
{
    struct np2opngen_synth_event events[2] = {
        reg_event(2U, 0U, 0x30U, 1U),
        key_event(3U, 1U, 0U, 0xf0U),
    };

    events[1].sample_timestamp = 1U;
    expect_invalid("descending timestamp", events, 2U, 4U, 0U, 4U);
    events[1].sample_timestamp = 3U;
    events[1].sequence = 0U;
    expect_invalid("duplicate sequence", events, 2U, 4U, 0U, 4U);
    events[1].sequence = UINT64_C(2);
    expect_invalid("sequence gap", events, 2U, 4U, 0U, 4U);
    events[1].sequence = 0U;
    events[0].sequence = 1U;
    expect_invalid("decreasing sequence", events, 2U, 4U, 0U, 4U);
    events[0].sequence = 0U;
    events[1].type = 99U;
    expect_invalid("invalid type", events, 2U, 4U, 0U, 4U);
    events[1] = key_event(3U, 1U, 6U, 0xf0U);
    expect_invalid("invalid key channel", events, 2U, 4U, 0U, 4U);
    events[0].payload.register_write.chbase = 4U;
    events[1] = key_event(3U, 1U, 0U, 0xf0U);
    expect_invalid("invalid chbase", events, 2U, 4U, 0U, 4U);
    events[0] = reg_event(2U, 0U, 0x33U, 1U);
    expect_invalid("invalid register", events, 2U, 4U, 0U, 4U);
    events[0] = reg_event(2U, 0U, 0x30U, 1U);
    events[1].payload.key_event.reserved = 1U;
    expect_invalid("reserved payload", events, 2U, 4U, 0U, 4U);
    events[1] = key_event(5U, 1U, 0U, 0xf0U);
    expect_invalid("timestamp beyond end", events, 2U, 4U, 0U, 4U);
    events[1] = key_event(3U, 1U, 0U, 0xf0U);
    expect_invalid("frame capacity", events, 2U, 5U, 0U, 4U);
    expect_invalid("sequence overflow", events, 2U, 4U, UINT64_MAX, 4U);
}

static void test_edges(void)
{
    struct recorder recorder;
    struct np2opngen_synth_event_observer observer;
    struct np2opngen_synth_event events[2];
    int status;

    memset(&recorder, 0, sizeof(recorder));
    status = np2opngen_synth_event_interpret(
        0, 0U, 7U, 0U, 7U, record_render, record_apply, &recorder,
        &observer);
    assert(status == NP2_SYNTH_EVENT_STATUS_OK);
    assert(recorder.render_calls == 1U && recorder.cursors[0] == 0U &&
           recorder.frames[0] == 7U && recorder.apply_calls == 0U &&
           observer.expected_sequence == 0U && observer.order_valid);

    memset(&recorder, 0, sizeof(recorder));
    events[0] = reg_event(0U, 0U, 0x30U, 1U);
    status = np2opngen_synth_event_interpret(
        events, 1U, 3U, 0U, 3U, record_render, record_apply, &recorder,
        &observer);
    assert(status == NP2_SYNTH_EVENT_STATUS_OK);
    assert(recorder.render_calls == 2U && recorder.frames[0] == 0U &&
           recorder.frames[1] == 3U && recorder.apply_calls == 1U &&
           observer.last_sequence == 0U && observer.order_valid);

    memset(&recorder, 0, sizeof(recorder));
    events[0] = reg_event(2U, 0U, 0x30U, 1U);
    events[1] = key_event(2U, 1U, 0U, 0xf0U);
    status = np2opngen_synth_event_interpret(
        events, 2U, 5U, 0U, 5U, record_render, record_apply, &recorder,
        &observer);
    assert(status == NP2_SYNTH_EVENT_STATUS_OK);
    assert(recorder.render_calls == 3U && recorder.frames[0] == 2U &&
           recorder.frames[1] == 0U && recorder.frames[2] == 3U &&
           recorder.apply_calls == 2U && recorder.sequences[0] == 0U &&
           recorder.sequences[1] == 1U && observer.applied_count == 2U &&
           observer.order_valid);

    memset(&recorder, 0, sizeof(recorder));
    events[0] = key_event(4U, 0U, 0U, 0x00U);
    status = np2opngen_synth_event_interpret(
        events, 1U, 4U, 0U, 4U, record_render, record_apply, &recorder,
        &observer);
    assert(status == NP2_SYNTH_EVENT_STATUS_OK);
    assert(recorder.render_calls == 2U && recorder.frames[0] == 4U &&
           recorder.frames[1] == 0U && recorder.apply_calls == 1U &&
           observer.last_sequence == 0U && observer.order_valid);
}

static void test_trace(void)
{
    const struct np2opngen_synth_event events[] = {
        reg_event(0x0102030405060708ULL, 9U, 0x00a4U, 0x22U),
        key_event(12U, 10U, 5U, 0xf0U),
    };
    uint8_t first_digest[32];
    uint8_t second_digest[32];
    uint32_t first_crc;
    uint32_t second_crc;
    assert(np2opngen_synth_event_trace(events, 2U, &first_crc, first_digest) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(np2opngen_synth_event_trace(events, 2U, &second_crc,
                                        second_digest) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(first_crc != 0U && first_crc == second_crc &&
           memcmp(first_digest, second_digest, sizeof(first_digest)) == 0);
}

static void test_order_sensitivity(void)
{
    const struct np2opngen_synth_event first[] = {
        reg_event(0U, 0U, 0xa4U, 0x22U),
        reg_event(0U, 1U, 0xa0U, 0x69U),
        key_event(0U, 2U, 0U, 0xf0U),
    };
    struct np2opngen_synth_event reordered[3];
    struct order_recorder first_state = {0};
    struct order_recorder reordered_state = {0};
    struct np2opngen_synth_event_observer observer;

    reordered[0] = first[1];
    reordered[1] = first[0];
    reordered[2] = first[2];
    reordered[0].sequence = 0U;
    reordered[1].sequence = 1U;
    assert(np2opngen_synth_event_interpret(
               first, 3U, 1U, 0U, 1U, order_render, order_apply,
               &first_state, &observer) == NP2_SYNTH_EVENT_STATUS_OK);
    assert(np2opngen_synth_event_interpret(
               reordered, 3U, 1U, 0U, 1U, order_render, order_apply,
               &reordered_state, &observer) == NP2_SYNTH_EVENT_STATUS_OK);
    assert(first_state.observed_frequency == 0x2269U);
    assert(reordered_state.observed_frequency == 0x0069U);
    assert(first_state.observed_frequency != reordered_state.observed_frequency);
}

int main(void)
{
    test_invalid_lists();
    test_edges();
    test_trace();
    test_order_sensitivity();
    return 0;
}
