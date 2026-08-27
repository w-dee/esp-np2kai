#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <np2opngen_s98.h>

#define HEADER_BYTES 0x30U
#define MAX_EVENTS 64U

struct parsed_fixture {
    struct np2opngen_s98_parser parser;
    struct np2opngen_synth_event events[MAX_EVENTS];
    size_t event_count;
    uint64_t trace_count;
    uint32_t trace_crc32;
    uint8_t trace_sha256[32];
};

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8U) & 0xffU);
    out[2] = (uint8_t)((value >> 16U) & 0xffU);
    out[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static size_t build_header(uint8_t *data, size_t capacity, uint32_t numerator,
                           uint32_t denominator, uint32_t device_count,
                           uint32_t device_type, uint32_t clock, uint32_t pan,
                           uint32_t compression, uint32_t tag_offset,
                           uint32_t loop_offset)
{
    assert(capacity >= HEADER_BYTES);
    memset(data, 0, capacity);
    memcpy(data, "S983", 4U);
    put_le32(data + 0x04U, numerator);
    put_le32(data + 0x08U, denominator);
    put_le32(data + 0x0cU, compression);
    put_le32(data + 0x10U, tag_offset);
    put_le32(data + 0x14U, HEADER_BYTES);
    put_le32(data + 0x18U, loop_offset);
    put_le32(data + 0x1cU, device_count);
    put_le32(data + 0x20U, device_type);
    put_le32(data + 0x24U, clock);
    put_le32(data + 0x28U, pan);
    return HEADER_BYTES;
}

static size_t append_write(uint8_t *data, size_t offset, uint8_t reg,
                           uint8_t value)
{
    data[offset++] = 0x00U;
    data[offset++] = reg;
    data[offset++] = value;
    return offset;
}

static size_t append_varint_wait(uint8_t *data, size_t offset, uint64_t raw)
{
    data[offset++] = 0xfeU;
    do {
        uint8_t byte = (uint8_t)(raw & 0x7fU);
        raw >>= 7U;
        data[offset++] = (uint8_t)(byte | (raw != 0U ? 0x80U : 0U));
    } while (raw != 0U);
    return offset;
}

static void parse_success(const uint8_t *data, size_t size,
                          struct parsed_fixture *fixture)
{
    uint32_t list_crc32;
    uint8_t list_sha256[32];
    int step;

    memset(fixture, 0, sizeof(*fixture));
    assert(np2opngen_s98_parser_init(&fixture->parser, data, size) == 0);
    while ((step = np2opngen_s98_parser_next(&fixture->parser,
                                              &fixture->events[fixture->event_count])) ==
           NP2_OPNGEN_S98_NEXT_EVENT) {
        assert(fixture->event_count < MAX_EVENTS - 1U);
        ++fixture->event_count;
    }
    assert(step == NP2_OPNGEN_S98_NEXT_END);
    assert(fixture->parser.result_category == NP2_OPNGEN_S98_RESULT_PASS);
    assert(fixture->parser.metadata.emitted_event_count == fixture->event_count);
    assert(fixture->parser.metadata.ignored_write_count == 0U);
    assert(np2opngen_s98_parser_event_trace_finish(
               &fixture->parser, &fixture->trace_count, &fixture->trace_crc32,
               fixture->trace_sha256) == NP2_SYNTH_EVENT_STATUS_OK);
    assert(fixture->trace_count == fixture->event_count);
    assert(np2opngen_synth_event_trace(fixture->events, fixture->event_count,
                                       &list_crc32, list_sha256) ==
           NP2_SYNTH_EVENT_STATUS_OK);
    assert(list_crc32 == fixture->trace_crc32);
    assert(memcmp(list_sha256, fixture->trace_sha256, sizeof(list_sha256)) ==
           0);
}

static void expect_failure(const char *name, const uint8_t *data, size_t size,
                           enum np2opngen_s98_result_category category)
{
    struct np2opngen_s98_parser parser;
    struct np2opngen_synth_event event;
    int result;

    result = np2opngen_s98_parser_init(&parser, data, size);
    if (result == 0) {
        do {
            result = np2opngen_s98_parser_next(&parser, &event);
        } while (result == NP2_OPNGEN_S98_NEXT_EVENT);
        assert(result == NP2_OPNGEN_S98_NEXT_ERROR && name != 0);
    }
    assert(parser.result_category == category && name != 0);
}

static void expect_register(const struct np2opngen_synth_event *event,
                            uint64_t timestamp, uint64_t sequence,
                            uint16_t reg, uint8_t value)
{
    assert(event->sample_timestamp == timestamp);
    assert(event->sequence == sequence);
    assert(event->type == NP2_SYNTH_EVENT_REGISTER_WRITE);
    assert(event->payload.register_write.chbase == 0U);
    assert(event->payload.register_write.reg == reg);
    assert(event->payload.register_write.value == value);
}

static void expect_key(const struct np2opngen_synth_event *event,
                       uint64_t timestamp, uint64_t sequence,
                       uint8_t channel, uint8_t mask)
{
    assert(event->sample_timestamp == timestamp);
    assert(event->sequence == sequence);
    assert(event->type == NP2_SYNTH_EVENT_KEY_EVENT);
    assert(event->payload.key_event.channel == channel);
    assert(event->payload.key_event.value == mask);
    assert(event->payload.key_event.reserved == 0U);
}

static void test_byte_exact_oracles(void)
{
    uint8_t data[256];
    size_t end;
    struct parsed_fixture fixture;

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    parse_success(data, end, &fixture);
    assert(fixture.event_count == 1U);
    expect_register(&fixture.events[0], 0U, 0U, 0x30U, 0x01U);
    assert(fixture.parser.metadata.end_frame == 0U);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    end = append_write(data, end, 0xa4U, 0x24U);
    end = append_write(data, end, 0xa0U, 0x20U);
    data[end++] = 0xfdU;
    parse_success(data, end, &fixture);
    assert(fixture.event_count == 2U);
    expect_register(&fixture.events[0], 0U, 0U, 0xa4U, 0x24U);
    expect_register(&fixture.events[1], 0U, 1U, 0xa0U, 0x20U);

    end = build_header(data, sizeof(data), 0U, 0U, 1U, 2U, 3993600U, 0U, 0U,
                       0U, 0U);
    data[end++] = 0xffU;
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    parse_success(data, end, &fixture);
    assert(fixture.parser.metadata.raw_timer_numerator == 0U);
    assert(fixture.parser.metadata.raw_timer_denominator == 0U);
    assert(fixture.parser.metadata.effective_timer_numerator == 10U);
    assert(fixture.parser.metadata.effective_timer_denominator == 1000U);
    expect_register(&fixture.events[0], 480U, 0U, 0x30U, 0x01U);
    assert(fixture.parser.metadata.end_frame == 480U);

    end = build_header(data, sizeof(data), 1U, 44100U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    end = append_varint_wait(data, end, 128U); /* logical wait = 130 */
    end = append_write(data, end, 0x28U, 0xf2U);
    data[end++] = 0xfdU;
    parse_success(data, end, &fixture);
    assert(fixture.parser.metadata.final_sync_count == 130U);
    expect_key(&fixture.events[0], 141U, 0U, 2U, 0xf0U);
    assert(fixture.parser.metadata.end_frame == 141U);
}

static void test_timing_equivalence_and_boundaries(void)
{
    uint8_t small_waits[512];
    uint8_t combined_wait[128];
    uint8_t large_safe[128];
    uint8_t overflow[128];
    size_t end;
    unsigned i;
    struct parsed_fixture small;
    struct parsed_fixture combined;
    struct parsed_fixture huge;

    end = build_header(small_waits, sizeof(small_waits), 1U, 44100U, 1U, 2U,
                       3993600U, 0U, 0U, 0U, 0U);
    for (i = 0U; i < 147U; ++i) {
        small_waits[end++] = 0xffU;
    }
    end = append_write(small_waits, end, 0x30U, 0x01U);
    small_waits[end++] = 0xfdU;
    parse_success(small_waits, end, &small);
    expect_register(&small.events[0], 160U, 0U, 0x30U, 0x01U);

    end = build_header(combined_wait, sizeof(combined_wait), 1U, 44100U, 1U,
                       2U, 3993600U, 0U, 0U, 0U, 0U);
    end = append_varint_wait(combined_wait, end, 145U); /* 145 + 2 = 147 */
    end = append_write(combined_wait, end, 0x30U, 0x01U);
    combined_wait[end++] = 0xfdU;
    parse_success(combined_wait, end, &combined);
    expect_register(&combined.events[0], 160U, 0U, 0x30U, 0x01U);
    assert(small.parser.metadata.end_frame == combined.parser.metadata.end_frame);
    assert(small.parser.metadata.final_sync_count ==
           combined.parser.metadata.final_sync_count);

    end = build_header(combined_wait, sizeof(combined_wait), 1U, 44100U, 1U,
                       2U, 3993600U, 0U, 0U, 0U, 0U);
    end = append_varint_wait(combined_wait, end, 144U); /* logical 146 */
    end = append_write(combined_wait, end, 0x30U, 0x01U);
    combined_wait[end++] = 0xfdU;
    parse_success(combined_wait, end, &combined);
    expect_register(&combined.events[0], 158U, 0U, 0x30U, 0x01U);

    end = build_header(large_safe, sizeof(large_safe), 1U, 48000U, 1U, 2U,
                       3993600U, 0U, 0U, 0U, 0U);
    end = append_varint_wait(large_safe, end, UINT64_MAX - 2U);
    end = append_write(large_safe, end, 0x30U, 0x01U);
    large_safe[end++] = 0xfdU;
    parse_success(large_safe, end, &huge);
    expect_register(&huge.events[0], UINT64_MAX, 0U, 0x30U, 0x01U);

    end = build_header(overflow, sizeof(overflow), UINT32_MAX, 1U, 1U, 2U,
                       3993600U, 0U, 0U, 0U, 0U);
    end = append_varint_wait(overflow, end, UINT64_MAX - 2U);
    end = append_write(overflow, end, 0x30U, 0x01U);
    overflow[end++] = 0xfdU;
    expect_failure("timing overflow", overflow, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);
}

static void test_valid_tag_and_clock_policy(void)
{
    uint8_t data[128];
    size_t end;
    struct parsed_fixture fixture;

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0x34U, 0U);
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    assert(end == 0x34U);
    memcpy(data + end, "[S98]", 5U);
    parse_success(data, end + 5U, &fixture);
    assert(fixture.parser.metadata.tag_offset == 0x34U);
    assert(fixture.parser.metadata.clock_policy ==
           NP2_OPNGEN_S98_CLOCK_EXACT_NP2);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 4000000U, 0U,
                       0U, 0U, 0U);
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    parse_success(data, end, &fixture);
    assert(fixture.parser.metadata.clock_policy ==
           NP2_OPNGEN_S98_CLOCK_WORKLOAD_CLOCK_MISMATCH);
}

static void test_failure_matrix(void)
{
    uint8_t data[128];
    size_t end;
    unsigned i;

    memset(data, 0, sizeof(data));
    expect_failure("short header", data, 31U, NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[0] = 'X';
    expect_failure("bad magic", data, end, NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    expect_failure("truncated device", data, 0x20U,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    put_le32(data + 0x14U, 0x80U);
    expect_failure("bad data offset", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0x7fU, 0U);
    data[end++] = 0xfdU;
    expect_failure("bad tag offset", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0x00U;
    data[end++] = 0x30U;
    expect_failure("truncated write", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0xfeU;
    expect_failure("truncated fe", data, end, NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0xfeU;
    data[end++] = 0x80U;
    expect_failure("unterminated fe", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0xfeU;
    for (i = 0U; i < 9U; ++i) {
        data[end++] = 0xffU;
    }
    data[end++] = 0x02U;
    expect_failure("overflow fe", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    end = append_write(data, end, 0x30U, 0x01U);
    expect_failure("missing fd", data, end, NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       1U, 0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("compression", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0x31U);
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    expect_failure("loop inside write", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 0U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("legacy device count", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 2U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    put_le32(data + 0x14U, 0x40U);
    data[0x40U] = 0xfdU;
    expect_failure("multiple devices", data, 0x41U,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 4U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("non ym2203", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 123U, 0U, 0U,
                       0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("other clock", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 1U,
                       0U, 0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("nonzero pan", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 0U, 0U, 0U,
                       0U, 0U);
    data[end++] = 0xfdU;
    expect_failure("zero clock", data, end,
                   NP2_OPNGEN_S98_RESULT_MALFORMED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    put_le32(data + 0x2cU, 1U);
    data[end++] = 0xfdU;
    expect_failure("nonzero reserved", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0x30U);
    end = append_write(data, end, 0x30U, 0x01U);
    data[end++] = 0xfdU;
    expect_failure("valid loop unsupported", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    data[end++] = 0x01U;
    data[end++] = 0x30U;
    data[end++] = 0x01U;
    data[end++] = 0xfdU;
    expect_failure("extended port", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);

    for (i = 0U; i < 8U; ++i) {
        static const uint8_t rejected_regs[] = {
            0x07U, 0x10U, 0x24U, 0x27U, 0xa8U, 0xb4U, 0x93U, 0x22U,
        };
        end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U,
                           0U, 0U, 0U, 0U);
        end = append_write(data, end, rejected_regs[i], 0x00U);
        data[end++] = 0xfdU;
        expect_failure("unsupported register", data, end,
                       NP2_OPNGEN_S98_RESULT_UNSUPPORTED);
    }

    end = build_header(data, sizeof(data), 1U, 1000U, 1U, 2U, 3993600U, 0U,
                       0U, 0U, 0U);
    end = append_write(data, end, 0x28U, 0x04U);
    data[end++] = 0xfdU;
    expect_failure("unavailable key channel", data, end,
                   NP2_OPNGEN_S98_RESULT_UNSUPPORTED);
}

static void print_hex(const uint8_t *bytes, size_t count)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        printf("%02x", bytes[i]);
    }
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    long file_size;
    uint8_t *data;
    if (file == 0 || fseek(file, 0L, SEEK_END) != 0 ||
        (file_size = ftell(file)) < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        if (file != 0) {
            fclose(file);
        }
        return 0;
    }
    data = (uint8_t *)malloc((size_t)file_size);
    if (data == 0 || fread(data, 1U, (size_t)file_size, file) !=
                         (size_t)file_size ||
        fclose(file) != 0) {
        free(data);
        return 0;
    }
    *size_out = (size_t)file_size;
    return data;
}

static void test_generated_fixtures(const char *directory)
{
    static const struct {
        const char *name;
        size_t event_count;
        uint64_t end_frame;
    } cases[] = {
        {"fm_single_tone", 12U, 2400U},
        {"fm_frequency_change", 7U, 320U},
        {"fm_three_channel", 18U, 1200U},
        {"fm_same_timestamp_burst", 7U, 48U},
        {"fm_envelope", 12U, 384U},
        {"fm_algorithm_feedback", 9U, 384U},
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char path[512];
        uint8_t *data;
        size_t size;
        struct parsed_fixture fixture;
        np2_sha256_context source_sha256;
        uint8_t source_digest[NP2_SHA256_DIGEST_SIZE];
        int written = snprintf(path, sizeof(path), "%s/%s.s98", directory,
                               cases[i].name);
        assert(written > 0 && (size_t)written < sizeof(path));
        data = read_file(path, &size);
        assert(data != 0);
        parse_success(data, size, &fixture);
        assert(fixture.event_count == cases[i].event_count);
        assert(fixture.parser.metadata.end_frame == cases[i].end_frame);
        assert(fixture.parser.metadata.declared_device_clock_hz == 3993600U);
        assert(fixture.parser.metadata.clock_policy ==
               NP2_OPNGEN_S98_CLOCK_EXACT_NP2);
        np2_sha256_init(&source_sha256);
        np2_sha256_update(&source_sha256, data, size);
        np2_sha256_final(&source_sha256, source_digest);
        printf("S98_PARSER_FIXTURE name=%s result=PASS s98_version=%" PRIu32
               " device_count=%" PRIu32 " device_type=%" PRIu32
               " declared_clock_hz=%" PRIu32
               " effective_opngen_clock_hz=%" PRIu32 " clock_policy=%s"
               " raw_timer=%" PRIu32 "/%" PRIu32
               " effective_timer=%" PRIu32 "/%" PRIu32
               " data_offset=%" PRIu32 " tag_offset=%" PRIu32
               " loop_offset=%" PRIu32 " source_bytes=%zu source_sha256=",
               cases[i].name, fixture.parser.metadata.s98_version,
               fixture.parser.metadata.device_count,
               fixture.parser.metadata.device_type,
               fixture.parser.metadata.declared_device_clock_hz,
               fixture.parser.metadata.effective_opngen_clock_hz,
               np2opngen_s98_clock_policy_name(
                   fixture.parser.metadata.clock_policy),
               fixture.parser.metadata.raw_timer_numerator,
               fixture.parser.metadata.raw_timer_denominator,
               fixture.parser.metadata.effective_timer_numerator,
               fixture.parser.metadata.effective_timer_denominator,
               fixture.parser.metadata.data_offset, fixture.parser.metadata.tag_offset,
               fixture.parser.metadata.loop_offset, size);
        print_hex(source_digest, sizeof(source_digest));
        printf(" source_write_count=%" PRIu64 " emitted_event_count=%" PRIu64
               " ignored_write_count=%" PRIu64 " final_sync_count=%" PRIu64
               " end_frame=%" PRIu64 " event_crc32=0x%08" PRIx32
               " event_sha256=",
               fixture.parser.metadata.source_write_count,
               fixture.parser.metadata.emitted_event_count,
               fixture.parser.metadata.ignored_write_count,
               fixture.parser.metadata.final_sync_count,
               fixture.parser.metadata.end_frame, fixture.trace_crc32);
        print_hex(fixture.trace_sha256, sizeof(fixture.trace_sha256));
        printf("\n");
        free(data);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--fixture-dir") != 0) {
        fprintf(stderr, "Usage: %s --fixture-dir DIR\n", argv[0]);
        return 2;
    }
    test_byte_exact_oracles();
    test_timing_equivalence_and_boundaries();
    test_valid_tag_and_clock_policy();
    test_failure_matrix();
    test_generated_fixtures(argv[2]);
    printf("S98_PARSER_META s98_version=3 effective_opngen_clock_hz=3993600 "
           "timer_default=10/1000 timing_44100=160/147 "
           "ignored_write_count=0\n");
    printf("S98_PARSER_RESULT=PASS\n");
    return 0;
}
