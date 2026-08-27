#include "np2opngen_s98.h"

#include <limits.h>
#include <string.h>

#define S98_HEADER_BYTES 0x20U
#define S98_DEVICE_BYTES 0x10U
#define S98_DEVICE_YM2203 2U

static uint32_t get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static int set_error(struct np2opngen_s98_parser *parser,
                     enum np2opngen_s98_result_category category,
                     enum np2opngen_s98_error error)
{
    parser->result_category = category;
    parser->error = error;
    return -1;
}

static int range_valid(size_t size, uint32_t offset, size_t bytes)
{
    return (size_t)offset <= size && bytes <= size - (size_t)offset;
}

/* Compute floor(a * b / divisor) without a wider-than-uint64_t integer.
 * The S98 denominator is a LE32 value, so the bounded remainder operations
 * below cannot overflow. */
static int mul_div_floor(uint64_t a, uint64_t b, uint32_t divisor,
                         uint64_t *result)
{
    uint64_t quotient = 0U;
    uint64_t remainder = 0U;
    uint64_t b_quotient;
    uint64_t b_remainder;
    int bit;

    if (divisor == 0U || result == 0) {
        return -1;
    }
    b_quotient = b / (uint64_t)divisor;
    b_remainder = b % (uint64_t)divisor;
    for (bit = 63; bit >= 0; --bit) {
        if (quotient > UINT64_MAX / 2U) {
            return -1;
        }
        quotient *= 2U;
        remainder *= 2U;
        if (remainder >= (uint64_t)divisor) {
            remainder -= (uint64_t)divisor;
            if (quotient == UINT64_MAX) {
                return -1;
            }
            ++quotient;
        }
        if ((a & (UINT64_C(1) << (unsigned)bit)) != 0U) {
            if (quotient > UINT64_MAX - b_quotient) {
                return -1;
            }
            quotient += b_quotient;
            remainder += b_remainder;
            if (remainder >= (uint64_t)divisor) {
                remainder -= (uint64_t)divisor;
                if (quotient == UINT64_MAX) {
                    return -1;
                }
                ++quotient;
            }
        }
    }
    *result = quotient;
    return 0;
}

static int mapped_frame(const struct np2opngen_s98_parser *parser,
                        uint64_t sync_count, uint64_t *frame)
{
    uint64_t scale;
    scale = (uint64_t)parser->metadata.effective_timer_numerator * 48000U;
    return mul_div_floor(sync_count, scale,
                         parser->metadata.effective_timer_denominator, frame);
}

static int decode_varint(const uint8_t *data, size_t size, size_t *cursor,
                         uint64_t *value)
{
    uint64_t decoded = 0U;
    unsigned shift = 0U;

    while (*cursor < size) {
        const uint8_t byte = data[(*cursor)++];
        const uint64_t payload = (uint64_t)(byte & 0x7fU);
        if (shift > 63U || (shift == 63U && payload > 1U)) {
            return -1;
        }
        decoded |= payload << shift;
        if ((byte & 0x80U) == 0U) {
            *value = decoded;
            return 0;
        }
        shift += 7U;
    }
    return -1;
}

static int preflight_dump(struct np2opngen_s98_parser *parser)
{
    size_t cursor = (size_t)parser->metadata.data_offset;
    int loop_on_boundary = parser->metadata.loop_offset == 0U;

    while (cursor < parser->size) {
        const size_t command_offset = cursor;
        const uint8_t command = parser->data[cursor++];
        uint64_t ignored_value;

        if (parser->metadata.loop_offset != 0U &&
            command_offset == (size_t)parser->metadata.loop_offset) {
            loop_on_boundary = 1;
        }
        if (command == 0x00U || command == 0x01U) {
            if (parser->size - cursor < 2U) {
                return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                                 NP2_OPNGEN_S98_ERROR_TRUNCATED);
            }
            cursor += 2U;
        } else if (command == 0xffU) {
            continue;
        } else if (command == 0xfeU) {
            if (decode_varint(parser->data, parser->size, &cursor,
                              &ignored_value) != 0 ||
                ignored_value > UINT64_MAX - 2U) {
                return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                                 NP2_OPNGEN_S98_ERROR_VARINT);
            }
        } else if (command == 0xfdU) {
            parser->dump_end = cursor;
            break;
        } else {
            return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                             NP2_OPNGEN_S98_ERROR_COMMAND);
        }
    }
    if (parser->dump_end == 0U) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_COMMAND);
    }
    if (parser->metadata.tag_offset != 0U) {
        const size_t tag_offset = (size_t)parser->metadata.tag_offset;
        if (tag_offset < parser->dump_end ||
            parser->size - tag_offset < 5U ||
            memcmp(parser->data + tag_offset, "[S98]", 5U) != 0) {
            return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                             NP2_OPNGEN_S98_ERROR_TAG);
        }
    }
    if (parser->metadata.loop_offset != 0U) {
        const size_t loop_offset = (size_t)parser->metadata.loop_offset;
        if (loop_offset < (size_t)parser->metadata.data_offset ||
            loop_offset >= parser->dump_end || !loop_on_boundary) {
            return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                             NP2_OPNGEN_S98_ERROR_LOOP);
        }
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         NP2_OPNGEN_S98_ERROR_LOOP);
    }
    return 0;
}

int np2opngen_s98_parser_init(struct np2opngen_s98_parser *parser,
                               const uint8_t *data, size_t size)
{
    uint32_t compression;
    uint32_t device_table_end;
    size_t device_table_bytes;
    uint32_t device_pan;
    uint32_t device_reserved;

    if (parser == 0 || data == 0) {
        return -1;
    }
    memset(parser, 0, sizeof(*parser));
    parser->data = data;
    parser->size = size;
    parser->metadata.s98_version = NP2_OPNGEN_S98_VERSION;
    parser->metadata.effective_opngen_clock_hz =
        NP2_OPNGEN_S98_EFFECTIVE_OPNGEN_CLOCK_HZ;
    parser->result_category = NP2_OPNGEN_S98_RESULT_PASS;

    if (size < S98_HEADER_BYTES) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_HEADER);
    }
    if (memcmp(data, "S983", 4U) != 0) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_MAGIC);
    }
    parser->metadata.raw_timer_numerator = get_le32(data + 0x04U);
    parser->metadata.raw_timer_denominator = get_le32(data + 0x08U);
    compression = get_le32(data + 0x0cU);
    parser->metadata.tag_offset = get_le32(data + 0x10U);
    parser->metadata.data_offset = get_le32(data + 0x14U);
    parser->metadata.loop_offset = get_le32(data + 0x18U);
    parser->metadata.device_count = get_le32(data + 0x1cU);
    parser->metadata.effective_timer_numerator =
        parser->metadata.raw_timer_numerator == 0U
            ? 10U
            : parser->metadata.raw_timer_numerator;
    parser->metadata.effective_timer_denominator =
        parser->metadata.raw_timer_denominator == 0U
            ? 1000U
            : parser->metadata.raw_timer_denominator;

    if (compression != 0U) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         NP2_OPNGEN_S98_ERROR_HEADER);
    }
    if (parser->metadata.device_count >
        (UINT32_MAX - S98_HEADER_BYTES) / S98_DEVICE_BYTES) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_OFFSET);
    }
    device_table_end = S98_HEADER_BYTES +
                       parser->metadata.device_count * S98_DEVICE_BYTES;
    device_table_bytes =
        (size_t)parser->metadata.device_count * S98_DEVICE_BYTES;
    if (!range_valid(size, S98_HEADER_BYTES, device_table_bytes) ||
        parser->metadata.data_offset < device_table_end ||
        !range_valid(size, parser->metadata.data_offset, 1U) ||
        (parser->metadata.tag_offset != 0U &&
         !range_valid(size, parser->metadata.tag_offset, 1U)) ||
        (parser->metadata.loop_offset != 0U &&
         !range_valid(size, parser->metadata.loop_offset, 1U))) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_OFFSET);
    }
    if (parser->metadata.device_count != 1U) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         NP2_OPNGEN_S98_ERROR_DEVICE);
    }
    parser->metadata.device_type = get_le32(data + 0x20U);
    parser->metadata.declared_device_clock_hz = get_le32(data + 0x24U);
    device_pan = get_le32(data + 0x28U);
    device_reserved = get_le32(data + 0x2cU);
    if (parser->metadata.device_type != S98_DEVICE_YM2203) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         NP2_OPNGEN_S98_ERROR_DEVICE);
    }
    if (parser->metadata.declared_device_clock_hz == 0U) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_CLOCK);
    }
    if (device_pan != 0U || device_reserved != 0U) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         device_pan != 0U ? NP2_OPNGEN_S98_ERROR_PAN
                                          : NP2_OPNGEN_S98_ERROR_DEVICE);
    }
    if (parser->metadata.declared_device_clock_hz == 3993600U) {
        parser->metadata.clock_policy = NP2_OPNGEN_S98_CLOCK_EXACT_NP2;
    } else if (parser->metadata.declared_device_clock_hz == 4000000U) {
        parser->metadata.clock_policy =
            NP2_OPNGEN_S98_CLOCK_WORKLOAD_CLOCK_MISMATCH;
    } else {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                         NP2_OPNGEN_S98_ERROR_CLOCK);
    }
    np2opngen_synth_event_trace_init(&parser->event_trace);
    if (preflight_dump(parser) != 0) {
        return -1;
    }
    parser->cursor = (size_t)parser->metadata.data_offset;
    return 0;
}

static int add_syncs(struct np2opngen_s98_parser *parser, uint64_t syncs)
{
    if (syncs > UINT64_MAX - parser->sync_count) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_TIMING);
    }
    parser->sync_count += syncs;
    return 0;
}

static int emit_register_or_key(struct np2opngen_s98_parser *parser,
                                uint8_t reg, uint8_t value,
                                struct np2opngen_synth_event *event_out)
{
    uint64_t timestamp;
    int is_register = 0;

    if (parser->metadata.source_write_count == UINT64_MAX) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_SEQUENCE);
    }
    ++parser->metadata.source_write_count;
    if (parser->next_sequence == UINT64_MAX ||
        mapped_frame(parser, parser->sync_count, &timestamp) != 0) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         parser->next_sequence == UINT64_MAX
                             ? NP2_OPNGEN_S98_ERROR_SEQUENCE
                             : NP2_OPNGEN_S98_ERROR_TIMING);
    }
    memset(event_out, 0, sizeof(*event_out));
    event_out->sample_timestamp = timestamp;
    event_out->sequence = parser->next_sequence;
    if (reg == 0x28U) {
        if ((value & 0x04U) != 0U || (value & 0x03U) > 2U) {
            return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                             NP2_OPNGEN_S98_ERROR_KEY);
        }
        event_out->type = NP2_SYNTH_EVENT_KEY_EVENT;
        event_out->payload.key_event.channel = value & 0x03U;
        event_out->payload.key_event.value = value & 0xf0U;
    } else {
        if (reg >= 0x30U && reg <= 0x9fU) {
            is_register = (reg & 0x03U) != 0x03U;
        } else if ((reg >= 0xa0U && reg <= 0xa2U) ||
                   (reg >= 0xa4U && reg <= 0xa6U) ||
                   (reg >= 0xb0U && reg <= 0xb2U)) {
            is_register = 1;
        }
        if (!is_register) {
            return set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                             NP2_OPNGEN_S98_ERROR_REGISTER);
        }
        event_out->type = NP2_SYNTH_EVENT_REGISTER_WRITE;
        event_out->payload.register_write.chbase = 0U;
        event_out->payload.register_write.reg = reg;
        event_out->payload.register_write.value = value;
    }
    if (np2opngen_synth_event_trace_update(&parser->event_trace, event_out) !=
        NP2_SYNTH_EVENT_STATUS_OK) {
        return set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                         NP2_OPNGEN_S98_ERROR_TRACE);
    }
    ++parser->metadata.emitted_event_count;
    ++parser->next_sequence;
    parser->last_timestamp = timestamp;
    parser->has_event = 1U;
    return 0;
}

int np2opngen_s98_parser_next(struct np2opngen_s98_parser *parser,
                               struct np2opngen_synth_event *event_out)
{
    if (parser == 0 || event_out == 0 ||
        parser->result_category != NP2_OPNGEN_S98_RESULT_PASS) {
        return NP2_OPNGEN_S98_NEXT_ERROR;
    }
    if (parser->ended != 0U) {
        return NP2_OPNGEN_S98_NEXT_END;
    }
    /* dump_end is the byte immediately after the preflight-validated FD. */
    while (parser->cursor + 1U < parser->dump_end) {
        const uint8_t command = parser->data[parser->cursor++];
        if (command == 0x00U) {
            const uint8_t reg = parser->data[parser->cursor++];
            const uint8_t value = parser->data[parser->cursor++];
            if (emit_register_or_key(parser, reg, value, event_out) != 0) {
                return NP2_OPNGEN_S98_NEXT_ERROR;
            }
            return NP2_OPNGEN_S98_NEXT_EVENT;
        }
        if (command == 0x01U) {
            set_error(parser, NP2_OPNGEN_S98_RESULT_UNSUPPORTED,
                      NP2_OPNGEN_S98_ERROR_COMMAND);
            return NP2_OPNGEN_S98_NEXT_ERROR;
        }
        if (command == 0xffU) {
            if (add_syncs(parser, 1U) != 0) {
                return NP2_OPNGEN_S98_NEXT_ERROR;
            }
            continue;
        }
        if (command == 0xfeU) {
            uint64_t raw_wait;
            if (decode_varint(parser->data, parser->dump_end, &parser->cursor,
                              &raw_wait) != 0 || raw_wait > UINT64_MAX - 2U ||
                add_syncs(parser, raw_wait + 2U) != 0) {
                if (parser->result_category == NP2_OPNGEN_S98_RESULT_PASS) {
                    set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                              NP2_OPNGEN_S98_ERROR_VARINT);
                }
                return NP2_OPNGEN_S98_NEXT_ERROR;
            }
            continue;
        }
        set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                  NP2_OPNGEN_S98_ERROR_COMMAND);
        return NP2_OPNGEN_S98_NEXT_ERROR;
    }
    if (mapped_frame(parser, parser->sync_count, &parser->metadata.end_frame) !=
            0 ||
        (parser->has_event != 0U &&
         parser->metadata.end_frame < parser->last_timestamp)) {
        set_error(parser, NP2_OPNGEN_S98_RESULT_MALFORMED,
                  NP2_OPNGEN_S98_ERROR_TIMING);
        return NP2_OPNGEN_S98_NEXT_ERROR;
    }
    parser->metadata.final_sync_count = parser->sync_count;
    parser->ended = 1U;
    return NP2_OPNGEN_S98_NEXT_END;
}

int np2opngen_s98_parser_event_trace_finish(
    struct np2opngen_s98_parser *parser, uint64_t *count, uint32_t *crc32,
    uint8_t digest[32])
{
    if (parser == 0 || parser->result_category != NP2_OPNGEN_S98_RESULT_PASS ||
        parser->ended == 0U) {
        return -1;
    }
    return np2opngen_synth_event_trace_finish(&parser->event_trace, count,
                                              crc32, digest);
}

const char *np2opngen_s98_result_category_name(
    enum np2opngen_s98_result_category category)
{
    switch (category) {
    case NP2_OPNGEN_S98_RESULT_PASS:
        return "PASS";
    case NP2_OPNGEN_S98_RESULT_MALFORMED:
        return "MALFORMED";
    case NP2_OPNGEN_S98_RESULT_UNSUPPORTED:
        return "UNSUPPORTED";
    default:
        return "UNKNOWN";
    }
}

const char *np2opngen_s98_error_name(enum np2opngen_s98_error error)
{
    switch (error) {
    case NP2_OPNGEN_S98_ERROR_NONE: return "none";
    case NP2_OPNGEN_S98_ERROR_ARGUMENT: return "argument";
    case NP2_OPNGEN_S98_ERROR_HEADER: return "header";
    case NP2_OPNGEN_S98_ERROR_MAGIC: return "magic";
    case NP2_OPNGEN_S98_ERROR_OFFSET: return "offset";
    case NP2_OPNGEN_S98_ERROR_DEVICE: return "device";
    case NP2_OPNGEN_S98_ERROR_CLOCK: return "clock";
    case NP2_OPNGEN_S98_ERROR_PAN: return "pan";
    case NP2_OPNGEN_S98_ERROR_LOOP: return "loop";
    case NP2_OPNGEN_S98_ERROR_TAG: return "tag";
    case NP2_OPNGEN_S98_ERROR_COMMAND: return "command";
    case NP2_OPNGEN_S98_ERROR_TRUNCATED: return "truncated";
    case NP2_OPNGEN_S98_ERROR_VARINT: return "varint";
    case NP2_OPNGEN_S98_ERROR_REGISTER: return "register";
    case NP2_OPNGEN_S98_ERROR_KEY: return "key";
    case NP2_OPNGEN_S98_ERROR_TIMING: return "timing";
    case NP2_OPNGEN_S98_ERROR_SEQUENCE: return "sequence";
    case NP2_OPNGEN_S98_ERROR_TRACE: return "trace";
    default: return "unknown";
    }
}

const char *np2opngen_s98_clock_policy_name(
    enum np2opngen_s98_clock_policy policy)
{
    switch (policy) {
    case NP2_OPNGEN_S98_CLOCK_EXACT_NP2:
        return "EXACT_NP2";
    case NP2_OPNGEN_S98_CLOCK_WORKLOAD_CLOCK_MISMATCH:
        return "WORKLOAD_CLOCK_MISMATCH";
    default:
        return "UNKNOWN";
    }
}
