#include "np2opngen_synth_event.h"

#include <limits.h>
#include <string.h>

#include "np2_crc32.h"
#include "np2_sha256.h"

static int register_supported(uint8_t chbase, uint16_t reg)
{
    uint16_t group;
    if (chbase > 3U || (reg & 3U) == 3U) {
        return 0;
    }
    group = reg < 0xa0U ? (uint16_t)(reg & 0xf0U)
                        : (uint16_t)(reg & 0xfcU);
    switch (group) {
    case 0x30U:
    case 0x40U:
    case 0x50U:
    case 0x60U:
    case 0x70U:
    case 0x80U:
        return 1;
    case 0x90U:
        return reg < 0xa0U;
    case 0xa0U:
    case 0xa4U:
    case 0xa8U:
    case 0xacU:
    case 0xb0U:
    case 0xb4U:
        return 1;
    default:
        return 0;
    }
}

static int validate_payload(const struct np2opngen_synth_event *event)
{
    if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE) {
        return register_supported(event->payload.register_write.chbase,
                                   event->payload.register_write.reg)
                   ? NP2_SYNTH_EVENT_STATUS_OK
                   : NP2_SYNTH_EVENT_STATUS_REGISTER;
    }
    if (event->type == NP2_SYNTH_EVENT_KEY_EVENT) {
        if (event->payload.key_event.channel > 5U) {
            return NP2_SYNTH_EVENT_STATUS_KEY;
        }
        return event->payload.key_event.reserved == 0U
                   ? NP2_SYNTH_EVENT_STATUS_OK
                   : NP2_SYNTH_EVENT_STATUS_PAYLOAD;
    }
    return NP2_SYNTH_EVENT_STATUS_TYPE;
}

int np2opngen_synth_event_validate(
    const struct np2opngen_synth_event *events, size_t count,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t frame_capacity)
{
    size_t i;
    uint64_t previous_timestamp = 0U;
    if ((events == 0 && count != 0U) || end_frame > frame_capacity) {
        return events == 0 && count != 0U ? NP2_SYNTH_EVENT_STATUS_ARGUMENT
                                          : NP2_SYNTH_EVENT_STATUS_FRAME_RANGE;
    }
    for (i = 0; i < count; ++i) {
        const struct np2opngen_synth_event *event = &events[i];
        const uint64_t index = (uint64_t)i;
        int status;
        if (i != 0U && event->sample_timestamp < previous_timestamp) {
            return NP2_SYNTH_EVENT_STATUS_TIMESTAMP;
        }
        if (event->sample_timestamp > end_frame) {
            return NP2_SYNTH_EVENT_STATUS_TIMESTAMP;
        }
        if (index > UINT64_MAX - expected_first_sequence) {
            return NP2_SYNTH_EVENT_STATUS_OVERFLOW;
        }
        if (event->sequence != expected_first_sequence + index) {
            return NP2_SYNTH_EVENT_STATUS_SEQUENCE;
        }
        status = validate_payload(event);
        if (status != NP2_SYNTH_EVENT_STATUS_OK) {
            return status;
        }
        previous_timestamp = event->sample_timestamp;
    }
    return NP2_SYNTH_EVENT_STATUS_OK;
}

int np2opngen_synth_event_interpret(
    const struct np2opngen_synth_event *events, size_t count,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t frame_capacity, np2opngen_synth_event_render_fn render,
    np2opngen_synth_event_apply_fn apply, void *context,
    struct np2opngen_synth_event_observer *observer)
{
    uint64_t cursor = 0U;
    size_t i;
    int status;
    if (render == 0 || (count != 0U && apply == 0)) {
        return NP2_SYNTH_EVENT_STATUS_ARGUMENT;
    }
    status = np2opngen_synth_event_validate(
        events, count, end_frame, expected_first_sequence, frame_capacity);
    if (status != NP2_SYNTH_EVENT_STATUS_OK) {
        return status;
    }
    if (observer != 0) {
        memset(observer, 0, sizeof(*observer));
        observer->expected_sequence = expected_first_sequence;
        observer->order_valid = true;
    }
    for (i = 0; i < count; ++i) {
        const struct np2opngen_synth_event *event = &events[i];
        status = render(context, cursor, event->sample_timestamp - cursor);
        if (status != 0) {
            return NP2_SYNTH_EVENT_STATUS_CALLBACK;
        }
        if (apply(context, event) != 0) {
            return NP2_SYNTH_EVENT_STATUS_CALLBACK;
        }
        if (observer != 0) {
            if (event->sequence != observer->expected_sequence) {
                observer->order_valid = false;
            }
            observer->applied_count++;
            observer->last_sequence = event->sequence;
            observer->expected_sequence =
                event->sequence == UINT64_MAX ? UINT64_MAX
                                              : event->sequence + 1U;
            observer->has_last_sequence = true;
        }
        cursor = event->sample_timestamp;
    }
    if (render(context, cursor, end_frame - cursor) != 0) {
        return NP2_SYNTH_EVENT_STATUS_CALLBACK;
    }
    if (observer != 0 && count == 0U) {
        observer->expected_sequence = expected_first_sequence;
    }
    return NP2_SYNTH_EVENT_STATUS_OK;
}

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void put_le64(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8U; ++i) {
        out[i] = (uint8_t)(value & 0xffU);
        value >>= 8;
    }
}

static int serialize_event(const struct np2opngen_synth_event *event,
                           uint8_t record[24])
{
    if (event == 0 || record == 0) {
        return NP2_SYNTH_EVENT_STATUS_ARGUMENT;
    }
    memset(record, 0, 24U);
    put_le64(record, event->sample_timestamp);
    put_le64(record + 8U, event->sequence);
    put_le32(record + 16U, event->type);
    if (event->type == NP2_SYNTH_EVENT_REGISTER_WRITE) {
        record[20] = event->payload.register_write.chbase;
        put_le16(record + 21U, event->payload.register_write.reg);
        record[23] = event->payload.register_write.value;
    } else if (event->type == NP2_SYNTH_EVENT_KEY_EVENT) {
        record[20] = event->payload.key_event.channel;
        record[21] = event->payload.key_event.value;
    } else {
        return NP2_SYNTH_EVENT_STATUS_TYPE;
    }
    return NP2_SYNTH_EVENT_STATUS_OK;
}

void np2opngen_synth_event_trace_init(
    struct np2opngen_synth_event_trace_state *state)
{
    if (state == 0) {
        return;
    }
    state->count = 0U;
    state->running_crc32 = np2_crc32_iso_hdlc_init();
    np2_sha256_init(&state->sha256);
}

int np2opngen_synth_event_trace_update(
    struct np2opngen_synth_event_trace_state *state,
    const struct np2opngen_synth_event *event)
{
    uint8_t record[24];
    int status;
    if (state == 0 || event == 0) {
        return NP2_SYNTH_EVENT_STATUS_ARGUMENT;
    }
    if (state->count == UINT64_MAX) {
        return NP2_SYNTH_EVENT_STATUS_OVERFLOW;
    }
    status = serialize_event(event, record);
    if (status != NP2_SYNTH_EVENT_STATUS_OK) {
        return status;
    }
    state->running_crc32 = np2_crc32_iso_hdlc_update(
        state->running_crc32, record, sizeof(record));
    np2_sha256_update(&state->sha256, record, sizeof(record));
    ++state->count;
    return NP2_SYNTH_EVENT_STATUS_OK;
}

int np2opngen_synth_event_trace_finish(
    struct np2opngen_synth_event_trace_state *state, uint64_t *count,
    uint32_t *crc32, uint8_t digest[32])
{
    if (state == 0 || count == 0 || crc32 == 0 || digest == 0) {
        return NP2_SYNTH_EVENT_STATUS_ARGUMENT;
    }
    *count = state->count;
    *crc32 = np2_crc32_iso_hdlc_finish(state->running_crc32);
    np2_sha256_final(&state->sha256, digest);
    return NP2_SYNTH_EVENT_STATUS_OK;
}

int np2opngen_synth_event_trace(
    const struct np2opngen_synth_event *events, size_t count,
    uint32_t *crc32, uint8_t digest[32])
{
    struct np2opngen_synth_event_trace_state state;
    uint64_t traced_count;
    int status;
    size_t i;
    if ((events == 0 && count != 0U) || crc32 == 0 || digest == 0) {
        return NP2_SYNTH_EVENT_STATUS_ARGUMENT;
    }
    np2opngen_synth_event_trace_init(&state);
    for (i = 0; i < count; ++i) {
        status = np2opngen_synth_event_trace_update(&state, &events[i]);
        if (status != NP2_SYNTH_EVENT_STATUS_OK) {
            return status;
        }
    }
    return np2opngen_synth_event_trace_finish(&state, &traced_count, crc32,
                                              digest);
}

const char *np2opngen_synth_event_status_name(int status)
{
    switch (status) {
    case NP2_SYNTH_EVENT_STATUS_OK:
        return "ok";
    case NP2_SYNTH_EVENT_STATUS_ARGUMENT:
        return "argument";
    case NP2_SYNTH_EVENT_STATUS_FRAME_RANGE:
        return "frame_range";
    case NP2_SYNTH_EVENT_STATUS_TIMESTAMP:
        return "timestamp";
    case NP2_SYNTH_EVENT_STATUS_SEQUENCE:
        return "sequence";
    case NP2_SYNTH_EVENT_STATUS_TYPE:
        return "type";
    case NP2_SYNTH_EVENT_STATUS_PAYLOAD:
        return "payload";
    case NP2_SYNTH_EVENT_STATUS_REGISTER:
        return "register";
    case NP2_SYNTH_EVENT_STATUS_KEY:
        return "key";
    case NP2_SYNTH_EVENT_STATUS_OVERFLOW:
        return "overflow";
    case NP2_SYNTH_EVENT_STATUS_CALLBACK:
        return "callback";
    default:
        return "unknown";
    }
}
