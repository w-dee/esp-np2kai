#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "np2opngen_e1b_stream.h"
#include "np2opngen_s98.h"

#define PCM_BYTES_PER_FRAME 4U
#define SHA256_HEX_BYTES 65U

struct fixture_expectation {
    const char *name;
    uint64_t event_count;
    uint64_t end_frame;
    uint32_t event_crc32;
};

static const struct fixture_expectation EXPECTATIONS[] = {
    {"fm_single_tone", 12U, 2400U, UINT32_C(0x8fac7f6d)},
    {"fm_frequency_change", 7U, 320U, UINT32_C(0x5bf3a304)},
    {"fm_three_channel", 18U, 1200U, UINT32_C(0xfba7a7f3)},
    {"fm_same_timestamp_burst", 7U, 48U, UINT32_C(0x5804f6b4)},
    {"fm_envelope", 12U, 384U, UINT32_C(0x62660d17)},
    {"fm_algorithm_feedback", 9U, 384U, UINT32_C(0x6c5d3ff1)},
};

struct event_identity {
    uint64_t count;
    uint32_t crc32;
    uint8_t sha256[32];
};

struct pcm_collector {
    uint8_t *bytes;
    size_t size;
    size_t capacity;
    int failed;
};

struct integration_context {
    const uint8_t *producer_source;
    size_t producer_source_size;
    const struct event_identity *preflight_identity;
    const struct np2opngen_s98_metadata *preflight_metadata;
    struct np2opngen_spsc_queue queue;
    struct np2opngen_e1b_control control;
    struct np2opngen_e1b_worker worker;
    struct pcm_collector pcm;
    pthread_t worker_thread;
    pthread_t producer_thread;
    atomic_bool worker_started;
    int worker_status;
    int producer_status;
    uint64_t full_retries;
    struct event_identity producer_identity;
    struct np2opngen_s98_metadata producer_metadata;
};

static void die(const char *message)
{
    fprintf(stderr, "S98_S3_ERROR %s\n", message);
    exit(EXIT_FAILURE);
}

static void digest_hex(const uint8_t digest[32], char output[SHA256_HEX_BYTES])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0U; i < 32U; ++i) {
        output[i * 2U] = hex[digest[i] >> 4U];
        output[i * 2U + 1U] = hex[digest[i] & 0x0fU];
    }
    output[64] = '\0';
}

static int read_file(const char *path, uint8_t **data_out, size_t *size_out)
{
    FILE *file;
    long length;
    uint8_t *data;
    size_t size;

    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return -1;
    }
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    size = (size_t)length;
    data = (uint8_t *)malloc(size == 0U ? 1U : size);
    if (data == NULL || (size != 0U &&
                         fread(data, 1U, size, file) != size)) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);
    *data_out = data;
    *size_out = size;
    return 0;
}

static void bytes_sha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    np2_sha256_context context;
    np2_sha256_init(&context);
    np2_sha256_update(&context, data, size);
    np2_sha256_final(&context, digest);
}

static int identity_equal(const struct event_identity *left,
                          const struct event_identity *right)
{
    return left->count == right->count && left->crc32 == right->crc32 &&
           memcmp(left->sha256, right->sha256, sizeof(left->sha256)) == 0;
}

static int metadata_equal(const struct np2opngen_s98_metadata *left,
                          const struct np2opngen_s98_metadata *right)
{
#define META_FIELD(name) (left->name == right->name)
    return META_FIELD(s98_version) && META_FIELD(device_count) &&
           META_FIELD(device_type) && META_FIELD(declared_device_clock_hz) &&
           META_FIELD(effective_opngen_clock_hz) &&
           META_FIELD(raw_timer_numerator) &&
           META_FIELD(raw_timer_denominator) &&
           META_FIELD(effective_timer_numerator) &&
           META_FIELD(effective_timer_denominator) && META_FIELD(data_offset) &&
           META_FIELD(tag_offset) && META_FIELD(loop_offset) &&
           META_FIELD(source_write_count) &&
           META_FIELD(emitted_event_count) &&
           META_FIELD(ignored_write_count) && META_FIELD(final_sync_count) &&
           META_FIELD(end_frame) && META_FIELD(clock_policy);
#undef META_FIELD
}

static int trace_finish(struct np2opngen_s98_parser *parser,
                        struct event_identity *identity)
{
    return np2opngen_s98_parser_event_trace_finish(
               parser, &identity->count, &identity->crc32, identity->sha256) ==
           NP2_SYNTH_EVENT_STATUS_OK
               ? 0
               : -1;
}

static int preflight(const uint8_t *source, size_t source_size,
                     struct np2opngen_s98_parser *parser,
                     struct event_identity *identity,
                     uint64_t *same_timestamp_pairs)
{
    struct np2opngen_synth_event event;
    uint64_t previous_timestamp = 0U;
    int has_previous = 0;
    int result;

    if (np2opngen_s98_parser_init(parser, source, source_size) != 0) {
        return -1;
    }
    do {
        result = np2opngen_s98_parser_next(parser, &event);
        if (result == NP2_OPNGEN_S98_NEXT_EVENT) {
            if (has_previous && event.sample_timestamp == previous_timestamp &&
                same_timestamp_pairs != NULL) {
                ++*same_timestamp_pairs;
            }
            previous_timestamp = event.sample_timestamp;
            has_previous = 1;
        }
    } while (result == NP2_OPNGEN_S98_NEXT_EVENT);
    if (result != NP2_OPNGEN_S98_NEXT_END ||
        parser->result_category != NP2_OPNGEN_S98_RESULT_PASS ||
        trace_finish(parser, identity) != 0) {
        return -1;
    }
    return identity->count == parser->metadata.emitted_event_count ? 0 : -1;
}

static int pcm_sink(const uint8_t *canonical_pcm, size_t pcm_bytes,
                    uint64_t frame_offset, void *context)
{
    struct pcm_collector *collector = (struct pcm_collector *)context;
    size_t expected_offset;
    size_t required;
    uint8_t *grown;

    if (collector == NULL || canonical_pcm == NULL ||
        frame_offset > SIZE_MAX / PCM_BYTES_PER_FRAME ||
        (size_t)frame_offset * PCM_BYTES_PER_FRAME != collector->size) {
        if (collector != NULL) {
            collector->failed = 1;
        }
        return -1;
    }
    expected_offset = (size_t)frame_offset * PCM_BYTES_PER_FRAME;
    if (pcm_bytes > SIZE_MAX - expected_offset) {
        collector->failed = 1;
        return -1;
    }
    required = expected_offset + pcm_bytes;
    if (required > collector->capacity) {
        size_t capacity = collector->capacity == 0U ? 4096U : collector->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        grown = (uint8_t *)realloc(collector->bytes, capacity);
        if (grown == NULL) {
            collector->failed = 1;
            return -1;
        }
        collector->bytes = grown;
        collector->capacity = capacity;
    }
    memcpy(collector->bytes + expected_offset, canonical_pcm, pcm_bytes);
    collector->size = required;
    return 0;
}

static void *worker_thread_main(void *opaque)
{
    struct integration_context *context = (struct integration_context *)opaque;
    int step;
    atomic_store_explicit(&context->worker_started, true, memory_order_release);
    for (;;) {
        step = np2opngen_e1b_worker_step(&context->worker);
        if (step == NP2_OPNGEN_E1B_STEP_COMPLETE ||
            step == NP2_OPNGEN_E1B_STEP_FAILED) {
            context->worker_status = step;
            return NULL;
        }
        sched_yield();
    }
}

static int producer_fail(struct integration_context *context)
{
    np2opngen_e1b_control_fail(&context->control,
                               NP2_OPNGEN_E1B_ERROR_GENERATOR);
    context->producer_status = -1;
    return -1;
}

static void *producer_thread_main(void *opaque)
{
    struct integration_context *context = (struct integration_context *)opaque;
    struct np2opngen_s98_parser parser;
    struct np2opngen_synth_event pending;
    int have_pending = 0;
    int result;

    context->producer_status = -1;
    if (np2opngen_s98_parser_init(&parser, context->producer_source,
                                  context->producer_source_size) != 0) {
        producer_fail(context);
        return NULL;
    }
    for (;;) {
        if (np2opngen_e1b_control_first_error(&context->control) !=
            NP2_OPNGEN_E1B_ERROR_NONE) {
            producer_fail(context);
            return NULL;
        }
        if (!have_pending) {
            result = np2opngen_s98_parser_next(&parser, &pending);
            if (result == NP2_OPNGEN_S98_NEXT_END) {
                break;
            }
            if (result != NP2_OPNGEN_S98_NEXT_EVENT) {
                producer_fail(context);
                return NULL;
            }
            have_pending = 1;
        }
        result = np2opngen_spsc_enqueue(&context->queue, &pending);
        if (result == NP2_OPNGEN_SPSC_OK) {
            have_pending = 0;
        } else if (result == NP2_OPNGEN_SPSC_FULL) {
            ++context->full_retries;
            sched_yield();
        } else {
            producer_fail(context);
            return NULL;
        }
    }
    if (trace_finish(&parser, &context->producer_identity) != 0 ||
        !metadata_equal(context->preflight_metadata, &parser.metadata) ||
        !identity_equal(context->preflight_identity,
                        &context->producer_identity)) {
        producer_fail(context);
        return NULL;
    }
    context->producer_metadata = parser.metadata;
    context->producer_status = 0;
    atomic_store_explicit(&context->control.producer_done, true,
                          memory_order_release);
    return NULL;
}

static int write_binary(const char *path, const uint8_t *data, size_t size)
{
    FILE *file;
    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "wb");
    if (file == NULL || (size != 0U && fwrite(data, 1U, size, file) != size)) {
        if (file != NULL) {
            fclose(file);
        }
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int run_success(const char *fixture_name, const uint8_t *source,
                       size_t source_size, const char *pcm_path)
{
    struct np2opngen_s98_parser pass1;
    struct event_identity preflight_identity;
    struct event_identity consumer_identity;
    struct integration_context context;
    struct np2opngen_e1b_pcm_sink sink;
    uint8_t source_sha256[32];
    uint8_t source_sha256_after[32];
    uint8_t original_source_sha256[32];
    char source_sha_hex[SHA256_HEX_BYTES];
    char preflight_sha_hex[SHA256_HEX_BYTES];
    char producer_sha_hex[SHA256_HEX_BYTES];
    char consumer_sha_hex[SHA256_HEX_BYTES];
    char pcm_sha_hex[SHA256_HEX_BYTES];
    uint8_t pcm_sha256[32];
    uint32_t pcm_crc32;
    uint64_t same_timestamp_pairs = 0U;
    const struct fixture_expectation *expectation = NULL;
    int is_retrofm = strcmp(fixture_name, "retrofm_pocket_demo") == 0;
    size_t i;
    int status;

    for (i = 0U; i < sizeof(EXPECTATIONS) / sizeof(EXPECTATIONS[0]); ++i) {
        if (strcmp(EXPECTATIONS[i].name, fixture_name) == 0) {
            expectation = &EXPECTATIONS[i];
            break;
        }
    }
    bytes_sha256(source, source_size, original_source_sha256);
    if ((!is_retrofm && expectation == NULL) ||
        preflight(source, source_size, &pass1, &preflight_identity,
                  &same_timestamp_pairs) != 0) {
        return -1;
    }
    if (is_retrofm) {
        if (source_size != 3753U || preflight_identity.count != 1047U ||
            pass1.metadata.declared_device_clock_hz != 4000000U ||
            pass1.metadata.effective_opngen_clock_hz != 3993600U ||
            pass1.metadata.clock_policy !=
                NP2_OPNGEN_S98_CLOCK_WORKLOAD_CLOCK_MISMATCH ||
            pass1.metadata.source_write_count != 1047U ||
            pass1.metadata.ignored_write_count != 0U ||
            pass1.metadata.final_sync_count != 530082U ||
            pass1.metadata.end_frame != 576960U) {
            return -1;
        }
    } else if (preflight_identity.count != expectation->event_count ||
               preflight_identity.crc32 != expectation->event_crc32 ||
               pass1.metadata.end_frame != expectation->end_frame ||
               (strcmp(fixture_name, "fm_same_timestamp_burst") == 0 &&
                same_timestamp_pairs != 5U)) {
        return -1;
    }
    memcpy(source_sha256, original_source_sha256, sizeof(source_sha256));

    memset(&context, 0, sizeof(context));
    context.producer_source = source;
    context.producer_source_size = source_size;
    context.preflight_identity = &preflight_identity;
    context.preflight_metadata = &pass1.metadata;
    np2opngen_spsc_init(&context.queue);
    np2opngen_e1b_control_init(&context.control);
    sink.write = pcm_sink;
    sink.context = &context.pcm;
    if (np2opngen_e1b_worker_init_with_sink(
            &context.worker, &context.queue, &context.control,
            pass1.metadata.end_frame, 0U, pass1.metadata.emitted_event_count,
            &sink) != 0) {
        return -1;
    }
    atomic_init(&context.worker_started, false);
    if (pthread_create(&context.worker_thread, NULL, worker_thread_main,
                       &context) != 0) {
        np2opngen_e1b_worker_destroy(&context.worker);
        return -1;
    }
    while (!atomic_load_explicit(&context.worker_started, memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_create(&context.producer_thread, NULL, producer_thread_main,
                       &context) != 0) {
        np2opngen_e1b_control_fail(&context.control,
                                   NP2_OPNGEN_E1B_ERROR_THREAD);
        atomic_store_explicit(&context.control.producer_done, true,
                              memory_order_release);
        pthread_join(context.worker_thread, NULL);
        np2opngen_e1b_worker_destroy(&context.worker);
        return -1;
    }
    pthread_join(context.producer_thread, NULL);
    pthread_join(context.worker_thread, NULL);
    status = context.worker_status == NP2_OPNGEN_E1B_STEP_COMPLETE &&
             context.producer_status == 0 && context.pcm.failed == 0;
    if (status) {
        status = np2opngen_e1b_worker_event_trace_finish(
                     &context.worker, &consumer_identity.count,
                     &consumer_identity.crc32, consumer_identity.sha256) ==
                     NP2_SYNTH_EVENT_STATUS_OK;
    }
    np2opngen_e1b_worker_destroy(&context.worker);
    bytes_sha256(source, source_size, source_sha256_after);
    status = status && memcmp(source_sha256, source_sha256_after,
                              sizeof(source_sha256)) == 0 &&
             identity_equal(&preflight_identity, &context.producer_identity) &&
             identity_equal(&context.producer_identity, &consumer_identity) &&
             context.worker.rendered_frames == pass1.metadata.end_frame &&
             context.pcm.size == (size_t)pass1.metadata.end_frame *
                                      PCM_BYTES_PER_FRAME;
    if (!status) {
        free(context.pcm.bytes);
        return -1;
    }
    pcm_crc32 = np2_crc32_iso_hdlc(context.pcm.bytes, context.pcm.size);
    bytes_sha256(context.pcm.bytes, context.pcm.size, pcm_sha256);
    digest_hex(source_sha256, source_sha_hex);
    digest_hex(preflight_identity.sha256, preflight_sha_hex);
    digest_hex(context.producer_identity.sha256, producer_sha_hex);
    digest_hex(consumer_identity.sha256, consumer_sha_hex);
    digest_hex(pcm_sha256, pcm_sha_hex);
    printf("S98_S3_SOURCE fixture=%s source_bytes=%zu source_sha256=%s\n",
           fixture_name, source_size, source_sha_hex);
    printf("S98_S3_META fixture=%s s98_version=%" PRIu32
           " device_count=%" PRIu32 " device_type=%" PRIu32
           " declared_clock=%" PRIu32 " effective_clock=%" PRIu32
           " clock_policy=%s raw_timer=%" PRIu32 "/%" PRIu32
           " effective_timer=%" PRIu32 "/%" PRIu32
           " data_offset=%" PRIu32 " tag_offset=%" PRIu32
           " loop_offset=%" PRIu32 " source_writes=%" PRIu64
           " ignored_writes=%" PRIu64 " final_sync=%" PRIu64
           " end_frame=%" PRIu64 "\n",
           fixture_name, pass1.metadata.s98_version, pass1.metadata.device_count,
           pass1.metadata.device_type, pass1.metadata.declared_device_clock_hz,
           pass1.metadata.effective_opngen_clock_hz,
           np2opngen_s98_clock_policy_name(pass1.metadata.clock_policy),
           pass1.metadata.raw_timer_numerator,
           pass1.metadata.raw_timer_denominator,
           pass1.metadata.effective_timer_numerator,
           pass1.metadata.effective_timer_denominator, pass1.metadata.data_offset,
           pass1.metadata.tag_offset, pass1.metadata.loop_offset,
           pass1.metadata.source_write_count, pass1.metadata.ignored_write_count,
           pass1.metadata.final_sync_count, pass1.metadata.end_frame);
    printf("S98_S3_EVENTS fixture=%s preflight_count=%" PRIu64
           " preflight_crc32=%08" PRIx32 " preflight_sha256=%s"
           " producer_count=%" PRIu64 " producer_crc32=%08" PRIx32
           " producer_sha256=%s consumer_count=%" PRIu64
           " consumer_crc32=%08" PRIx32 " consumer_sha256=%s"
           " sequence_errors=%" PRIu64 " same_timestamp_pairs=%" PRIu64 "\n",
           fixture_name, preflight_identity.count, preflight_identity.crc32,
           preflight_sha_hex, context.producer_identity.count,
           context.producer_identity.crc32, producer_sha_hex,
           consumer_identity.count, consumer_identity.crc32, consumer_sha_hex,
           context.worker.sequence_errors, same_timestamp_pairs);
    printf("S98_S3_PCM fixture=%s frames=%" PRIu64 " bytes=%zu crc32=%08" PRIx32
           " sha256=%s\n",
           fixture_name, context.worker.rendered_frames, context.pcm.size,
           pcm_crc32, pcm_sha_hex);
    printf("S98_S3_INVARIANTS fixture=%s parser_repeat=PASS"
           " transport_identity=PASS source_immutable=PASS\n",
           fixture_name);
    printf("S98_S3_RESULT fixture=%s PASS\n", fixture_name);
    if (write_binary(pcm_path, context.pcm.bytes, context.pcm.size) != 0) {
        free(context.pcm.bytes);
        return -1;
    }
    free(context.pcm.bytes);
    return 0;
}

static int run_failure_test(const char *fixture_dir)
{
    char path[4096];
    uint8_t *source = NULL;
    uint8_t *mutated = NULL;
    size_t source_size;
    struct np2opngen_s98_parser pass1;
    struct event_identity identity;
    struct integration_context context;
    struct np2opngen_e1b_pcm_sink sink;
    size_t i;
    int mutated_write = 0;

    if (snprintf(path, sizeof(path), "%s/fm_single_tone.s98", fixture_dir) >=
            (int)sizeof(path) ||
        read_file(path, &source, &source_size) != 0 ||
        preflight(source, source_size, &pass1, &identity, NULL) != 0) {
        free(source);
        return -1;
    }
    mutated = (uint8_t *)malloc(source_size);
    if (mutated == NULL) {
        free(source);
        return -1;
    }
    memcpy(mutated, source, source_size);
    for (i = 0x30U; i + 2U < source_size; ++i) {
        if (mutated[i] == 0x00U) {
            mutated[i + 1U] = 0x22U; /* isolated pass-2 unsupported register */
            mutated_write = 1;
            break;
        }
    }
    if (!mutated_write) {
        free(mutated);
        free(source);
        return -1;
    }
    memset(&context, 0, sizeof(context));
    context.producer_source = mutated;
    context.producer_source_size = source_size;
    context.preflight_identity = &identity;
    context.preflight_metadata = &pass1.metadata;
    np2opngen_spsc_init(&context.queue);
    np2opngen_e1b_control_init(&context.control);
    sink.write = pcm_sink;
    sink.context = &context.pcm;
    if (np2opngen_e1b_worker_init_with_sink(
            &context.worker, &context.queue, &context.control,
            pass1.metadata.end_frame, 0U, pass1.metadata.emitted_event_count,
            &sink) != 0) {
        free(mutated);
        free(source);
        return -1;
    }
    atomic_init(&context.worker_started, false);
    if (pthread_create(&context.worker_thread, NULL, worker_thread_main,
                       &context) != 0) {
        np2opngen_e1b_worker_destroy(&context.worker);
        free(mutated);
        free(source);
        return -1;
    }
    while (!atomic_load_explicit(&context.worker_started, memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_create(&context.producer_thread, NULL, producer_thread_main,
                       &context) != 0) {
        np2opngen_e1b_control_fail(&context.control,
                                   NP2_OPNGEN_E1B_ERROR_THREAD);
        atomic_store_explicit(&context.control.producer_done, true,
                              memory_order_release);
        pthread_join(context.worker_thread, NULL);
        np2opngen_e1b_worker_destroy(&context.worker);
        free(mutated);
        free(source);
        return -1;
    }
    pthread_join(context.producer_thread, NULL);
    pthread_join(context.worker_thread, NULL);
    if (context.producer_status == 0 ||
        context.worker_status != NP2_OPNGEN_E1B_STEP_FAILED ||
        np2opngen_e1b_control_first_error(&context.control) !=
            NP2_OPNGEN_E1B_ERROR_GENERATOR || context.pcm.size != 0U) {
        np2opngen_e1b_worker_destroy(&context.worker);
        free(context.pcm.bytes);
        free(mutated);
        free(source);
        return -1;
    }
    np2opngen_e1b_worker_destroy(&context.worker);
    free(context.pcm.bytes);
    free(mutated);
    free(source);
    printf("S98_S3_FAILURE second_pass_parser=PASS first_error=GENERATOR"
           " worker=FAILED producer=STOPPED threads_joined=PASS pcm_pass=NONE\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *fixture_dir = NULL;
    const char *fixture_name = NULL;
    const char *fixture_file = NULL;
    const char *pcm_path = NULL;
    int failure_test = 0;
    uint8_t *source = NULL;
    size_t source_size = 0U;
    char path[4096];
    int i;
    int result;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fixture-dir") == 0 && i + 1 < argc) {
            fixture_dir = argv[++i];
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            fixture_name = argv[++i];
        } else if (strcmp(argv[i], "--fixture-file") == 0 && i + 1 < argc) {
            fixture_file = argv[++i];
        } else if (strcmp(argv[i], "--pcm-out") == 0 && i + 1 < argc) {
            pcm_path = argv[++i];
        } else if (strcmp(argv[i], "--failure-test") == 0) {
            failure_test = 1;
        } else {
            die("usage: --fixture-dir DIR --fixture NAME [--pcm-out PATH] or --fixture-file PATH [--pcm-out PATH]");
        }
    }
    if (fixture_dir == NULL && fixture_file == NULL) {
        die("--fixture-dir is required");
    }
    if (failure_test) {
        if (fixture_dir == NULL || fixture_file != NULL) {
            die("--failure-test requires --fixture-dir");
        }
        return run_failure_test(fixture_dir) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (fixture_file != NULL) {
        fixture_name = "retrofm_pocket_demo";
        if (read_file(fixture_file, &source, &source_size) != 0) {
            die("unable to read fixture file");
        }
    } else {
        if (fixture_name == NULL ||
            snprintf(path, sizeof(path), "%s/%s.s98", fixture_dir,
                     fixture_name) >= (int)sizeof(path) ||
            read_file(path, &source, &source_size) != 0) {
            die("unable to read fixture");
        }
    }
    result = run_success(fixture_name, source, source_size, pcm_path);
    free(source);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
