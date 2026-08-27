#ifndef NP2_OPNGEN_SYNTH_EVENT_H
#define NP2_OPNGEN_SYNTH_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The event protocol deliberately contains only the two Phase-2 primitives. */
enum synth_event_type {
    SYNTH_EVENT_REGISTER_WRITE = 1,
    SYNTH_EVENT_KEY_EVENT = 2,
};

#define NP2_SYNTH_EVENT_REGISTER_WRITE SYNTH_EVENT_REGISTER_WRITE
#define NP2_SYNTH_EVENT_KEY_EVENT SYNTH_EVENT_KEY_EVENT

struct np2opngen_synth_event_register_write {
    uint8_t chbase;
    uint16_t reg;
    uint8_t value;
};

struct np2opngen_synth_event_key_event {
    uint8_t channel;
    uint8_t value;
    uint16_t reserved;
};

struct np2opngen_synth_event {
    uint64_t sample_timestamp;
    uint64_t sequence;
    uint32_t type;
    union {
        struct np2opngen_synth_event_register_write register_write;
        struct np2opngen_synth_event_key_event key_event;
    } payload;
};

enum np2opngen_synth_event_status {
    NP2_SYNTH_EVENT_STATUS_OK = 0,
    NP2_SYNTH_EVENT_STATUS_ARGUMENT,
    NP2_SYNTH_EVENT_STATUS_FRAME_RANGE,
    NP2_SYNTH_EVENT_STATUS_TIMESTAMP,
    NP2_SYNTH_EVENT_STATUS_SEQUENCE,
    NP2_SYNTH_EVENT_STATUS_TYPE,
    NP2_SYNTH_EVENT_STATUS_PAYLOAD,
    NP2_SYNTH_EVENT_STATUS_REGISTER,
    NP2_SYNTH_EVENT_STATUS_KEY,
    NP2_SYNTH_EVENT_STATUS_OVERFLOW,
    NP2_SYNTH_EVENT_STATUS_CALLBACK,
};

struct np2opngen_synth_event_observer {
    uint64_t applied_count;
    uint64_t last_sequence;
    uint64_t expected_sequence;
    bool has_last_sequence;
    bool order_valid;
};

typedef int (*np2opngen_synth_event_render_fn)(void *context,
                                                uint64_t cursor,
                                                uint64_t frame_count);
typedef int (*np2opngen_synth_event_apply_fn)(
    void *context, const struct np2opngen_synth_event *event);

int np2opngen_synth_event_validate(
    const struct np2opngen_synth_event *events, size_t count,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t frame_capacity);

int np2opngen_synth_event_interpret(
    const struct np2opngen_synth_event *events, size_t count,
    uint64_t end_frame, uint64_t expected_first_sequence,
    uint64_t frame_capacity, np2opngen_synth_event_render_fn render,
    np2opngen_synth_event_apply_fn apply, void *context,
    struct np2opngen_synth_event_observer *observer);

int np2opngen_synth_event_trace(
    const struct np2opngen_synth_event *events, size_t count,
    uint32_t *crc32, uint8_t digest[32]);

const char *np2opngen_synth_event_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_SYNTH_EVENT_H */
