#ifndef NP2_OPNGEN_S98_H
#define NP2_OPNGEN_S98_H

#include <stddef.h>
#include <stdint.h>

#include "np2opngen_synth_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP2_OPNGEN_S98_VERSION 3U
#define NP2_OPNGEN_S98_EFFECTIVE_OPNGEN_CLOCK_HZ 3993600U

enum np2opngen_s98_result_category {
    NP2_OPNGEN_S98_RESULT_PASS = 0,
    NP2_OPNGEN_S98_RESULT_MALFORMED,
    NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
};

enum np2opngen_s98_error {
    NP2_OPNGEN_S98_ERROR_NONE = 0,
    NP2_OPNGEN_S98_ERROR_ARGUMENT,
    NP2_OPNGEN_S98_ERROR_HEADER,
    NP2_OPNGEN_S98_ERROR_MAGIC,
    NP2_OPNGEN_S98_ERROR_OFFSET,
    NP2_OPNGEN_S98_ERROR_DEVICE,
    NP2_OPNGEN_S98_ERROR_CLOCK,
    NP2_OPNGEN_S98_ERROR_PAN,
    NP2_OPNGEN_S98_ERROR_LOOP,
    NP2_OPNGEN_S98_ERROR_TAG,
    NP2_OPNGEN_S98_ERROR_COMMAND,
    NP2_OPNGEN_S98_ERROR_TRUNCATED,
    NP2_OPNGEN_S98_ERROR_VARINT,
    NP2_OPNGEN_S98_ERROR_REGISTER,
    NP2_OPNGEN_S98_ERROR_KEY,
    NP2_OPNGEN_S98_ERROR_TIMING,
    NP2_OPNGEN_S98_ERROR_SEQUENCE,
    NP2_OPNGEN_S98_ERROR_TRACE,
};

enum np2opngen_s98_clock_policy {
    NP2_OPNGEN_S98_CLOCK_EXACT_NP2 = 0,
    NP2_OPNGEN_S98_CLOCK_WORKLOAD_CLOCK_MISMATCH,
};

enum np2opngen_s98_next_result {
    NP2_OPNGEN_S98_NEXT_EVENT = 1,
    NP2_OPNGEN_S98_NEXT_END = 0,
    NP2_OPNGEN_S98_NEXT_ERROR = -1,
};

struct np2opngen_s98_metadata {
    uint32_t s98_version;
    uint32_t device_count;
    uint32_t device_type;
    uint32_t declared_device_clock_hz;
    uint32_t effective_opngen_clock_hz;
    uint32_t raw_timer_numerator;
    uint32_t raw_timer_denominator;
    uint32_t effective_timer_numerator;
    uint32_t effective_timer_denominator;
    uint32_t data_offset;
    uint32_t tag_offset;
    uint32_t loop_offset;
    uint64_t source_write_count;
    uint64_t emitted_event_count;
    uint64_t ignored_write_count;
    uint64_t final_sync_count;
    uint64_t end_frame;
    enum np2opngen_s98_clock_policy clock_policy;
};

struct np2opngen_s98_parser {
    const uint8_t *data;
    size_t size;
    size_t cursor;
    size_t dump_end;
    uint64_t sync_count;
    uint64_t next_sequence;
    uint64_t last_timestamp;
    struct np2opngen_s98_metadata metadata;
    struct np2opngen_synth_event_trace_state event_trace;
    enum np2opngen_s98_result_category result_category;
    enum np2opngen_s98_error error;
    uint8_t ended;
    uint8_t has_event;
};

int np2opngen_s98_parser_init(struct np2opngen_s98_parser *parser,
                               const uint8_t *data, size_t size);

int np2opngen_s98_parser_next(struct np2opngen_s98_parser *parser,
                               struct np2opngen_synth_event *event_out);

int np2opngen_s98_parser_event_trace_finish(
    struct np2opngen_s98_parser *parser, uint64_t *count, uint32_t *crc32,
    uint8_t digest[32]);

const char *np2opngen_s98_result_category_name(
    enum np2opngen_s98_result_category category);
const char *np2opngen_s98_error_name(enum np2opngen_s98_error error);
const char *np2opngen_s98_clock_policy_name(
    enum np2opngen_s98_clock_policy policy);

#ifdef __cplusplus
}
#endif

#endif /* NP2_OPNGEN_S98_H */
