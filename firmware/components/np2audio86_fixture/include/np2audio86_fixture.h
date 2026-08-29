#ifndef NP2_AUDIO86_FIXTURE_H
#define NP2_AUDIO86_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np2_sha256.h"

#ifdef __cplusplus
#include <atomic>
#define NP2_AUDIO86_ASYNC_ATOMIC(type) std::atomic<type>
#define NP2_AUDIO86_STATIC_ASSERT static_assert
extern "C" {
#else
#include <stdatomic.h>
#define NP2_AUDIO86_ASYNC_ATOMIC(type) _Atomic type
#define NP2_AUDIO86_STATIC_ASSERT _Static_assert
#endif

#define NP2_AUDIO86_RATE_HZ 48000U
#define NP2_AUDIO86_QUANTUM_FRAMES 240U
#define NP2_AUDIO86_DURATION_FRAMES 2880000U
#define NP2_AUDIO86_QUANTA 12000U
#define NP2_AUDIO86_CHANNELS 2U
#define NP2_AUDIO86_PCM_BYTES \
    (NP2_AUDIO86_DURATION_FRAMES * NP2_AUDIO86_CHANNELS * 2U)
#define NP2_AUDIO86_PCM86_SOURCE_RATE_HZ 44100U
#define NP2_AUDIO86_PCM86_SOURCE_CHANNELS 2U
#define NP2_AUDIO86_PCM86_SOURCE_BITS 16U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES 8192U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES \
    (NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES * 4U)

NP2_AUDIO86_STATIC_ASSERT(NP2_AUDIO86_DURATION_FRAMES ==
                              NP2_AUDIO86_RATE_HZ * 60U,
                          "86H.2 duration geometry");
NP2_AUDIO86_STATIC_ASSERT(
    NP2_AUDIO86_DURATION_FRAMES % NP2_AUDIO86_QUANTUM_FRAMES == 0U,
    "86H.2 quantum geometry");
NP2_AUDIO86_STATIC_ASSERT(NP2_AUDIO86_QUANTA * NP2_AUDIO86_QUANTUM_FRAMES ==
                              NP2_AUDIO86_DURATION_FRAMES,
                          "86H.2 quantum count");
NP2_AUDIO86_STATIC_ASSERT(NP2_AUDIO86_PCM_BYTES == 11520000U,
                          "86H.2 byte geometry");

struct np2audio86_fixture_result {
    uint64_t frames;
    uint64_t bytes;
    uint64_t quanta;
    uint32_t pcm_crc32;
    uint8_t pcm_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t control_crc32;
    uint8_t control_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t source_crc32;
    uint8_t source_sha256[NP2_SHA256_DIGEST_SIZE];
    uint32_t control_events;
    uint32_t mid_quantum_events;
    uint64_t pcm86_bytes_supplied;
    uint64_t pcm86_bytes_consumed;
    uint32_t pcm86_refills;
    int32_t pcm86_fifo_min;
    int32_t pcm86_fifo_max;
    uint64_t mix_peak_abs;
    uint64_t clamped_samples;
    uint8_t fm_contribution;
    uint8_t psg_contribution;
    uint8_t rhythm_contribution;
    uint8_t pcm86_contribution;
    uint8_t pcm86_fifo_underrun;
    uint8_t sequence_error;
    uint8_t arithmetic_error;
};

/* 86H.3 input-side asynchronous transport.  These types are deliberately
 * independent from the E1B SynthEvent ABI: PCM86 data runs need a bounded
 * byte transport and a 128-record event queue. */
#define NP2_AUDIO86_ASYNC_EVENT_CAPACITY 128U
#define NP2_AUDIO86_ASYNC_BYTE_CAPACITY 65536U
#define NP2_AUDIO86_ASYNC_MAX_DATA_RUN 32768U
#define NP2_AUDIO86_ASYNC_MAX_EVENTS 333U

enum np2audio86_event_opcode {
    NP2_AUDIO86_EVENT_FM_KEY = 1U,
    NP2_AUDIO86_EVENT_PSG_REGISTER = 2U,
    NP2_AUDIO86_EVENT_PCM86_DATA_RUN = 3U,
    NP2_AUDIO86_EVENT_BARRIER_RESERVED = 0x80000000U,
};

struct np2audio86_event {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
};

NP2_AUDIO86_STATIC_ASSERT(sizeof(struct np2audio86_event) == 24U,
                          "86H.3 Audio86Event must remain 24 bytes");

enum np2audio86_transport_status {
    NP2_AUDIO86_TRANSPORT_OK = 0,
    NP2_AUDIO86_TRANSPORT_EMPTY,
    NP2_AUDIO86_TRANSPORT_FULL,
    NP2_AUDIO86_TRANSPORT_ARGUMENT,
    NP2_AUDIO86_TRANSPORT_INVARIANT,
};

struct np2audio86_event_ring {
    struct np2audio86_event slots[NP2_AUDIO86_ASYNC_EVENT_CAPACITY];
    NP2_AUDIO86_ASYNC_ATOMIC(uint32_t) head;
    NP2_AUDIO86_ASYNC_ATOMIC(uint32_t) tail;
};

NP2_AUDIO86_STATIC_ASSERT(
    sizeof(((struct np2audio86_event_ring *)0)->slots) == 3072U,
    "86H.3 event payload storage must remain 3072 bytes");

struct np2audio86_byte_ring {
    uint8_t bytes[NP2_AUDIO86_ASYNC_BYTE_CAPACITY];
    NP2_AUDIO86_ASYNC_ATOMIC(uint32_t) head;
    NP2_AUDIO86_ASYNC_ATOMIC(uint32_t) tail;
};

NP2_AUDIO86_STATIC_ASSERT(
    sizeof(((struct np2audio86_byte_ring *)0)->bytes) == 65536U,
    "86H.3 byte payload storage must remain 65536 bytes");

void np2audio86_event_ring_init(struct np2audio86_event_ring *ring);
int np2audio86_event_ring_enqueue(struct np2audio86_event_ring *ring,
                                   const struct np2audio86_event *event);
int np2audio86_event_ring_dequeue(struct np2audio86_event_ring *ring,
                                   struct np2audio86_event *event);
uint32_t np2audio86_event_ring_occupancy(
    const struct np2audio86_event_ring *ring);

void np2audio86_byte_ring_init(struct np2audio86_byte_ring *ring);
int np2audio86_byte_ring_push(struct np2audio86_byte_ring *ring,
                              const uint8_t *bytes, size_t count);
int np2audio86_byte_ring_pop(struct np2audio86_byte_ring *ring,
                             uint8_t *bytes, size_t count);
uint32_t np2audio86_byte_ring_occupancy(
    const struct np2audio86_byte_ring *ring);

enum np2audio86_async_mode {
    NP2_AUDIO86_ASYNC_PRODUCER_FAST_WORKER_YIELD = 0,
    NP2_AUDIO86_ASYNC_PRODUCER_YIELD_WORKER_FAST,
    NP2_AUDIO86_ASYNC_DETERMINISTIC_ALTERNATING,
    NP2_AUDIO86_ASYNC_BYTE_TRANSPORT_PRESSURE,
};

/* Stable semantic classifications used by the host async proof.  The first
 * 15 values are retained for compatibility with the 86H.3 host result; the
 * additional values separate transport faults without entering the portable
 * P4 runtime API. */
enum np2audio86_async_error {
    NP2_AUDIO86_ASYNC_ERROR_NONE = 0,
    NP2_AUDIO86_ASYNC_ERROR_ARGUMENT,
    NP2_AUDIO86_ASYNC_ERROR_PLAN,
    NP2_AUDIO86_ASYNC_ERROR_SEQUENCE,
    NP2_AUDIO86_ASYNC_ERROR_TIMESTAMP,
    NP2_AUDIO86_ASYNC_ERROR_OPCODE,
    NP2_AUDIO86_ASYNC_ERROR_PAYLOAD,
    NP2_AUDIO86_ASYNC_ERROR_BYTE_RING,
    NP2_AUDIO86_ASYNC_ERROR_PCM86_UNDERRUN,
    NP2_AUDIO86_ASYNC_ERROR_ARITHMETIC,
    NP2_AUDIO86_ASYNC_ERROR_CANONICAL,
    NP2_AUDIO86_ASYNC_ERROR_PRODUCER,
    NP2_AUDIO86_ASYNC_ERROR_WORKER,
    NP2_AUDIO86_ASYNC_ERROR_COMPLETION,
    NP2_AUDIO86_ASYNC_ERROR_LIVENESS,
    NP2_AUDIO86_ASYNC_ERROR_DATA_LENGTH,
    NP2_AUDIO86_ASYNC_ERROR_DATA_AVAILABILITY,
    NP2_AUDIO86_ASYNC_ERROR_WATERMARK,
    NP2_AUDIO86_ASYNC_ERROR_PREMATURE_COMPLETION,
    NP2_AUDIO86_ASYNC_ERROR_TRANSPORT_INVARIANT,
    NP2_AUDIO86_ASYNC_ERROR_ORACLE_MISMATCH,
};

struct np2audio86_async_result {
    struct np2audio86_fixture_result oracle;
    uint32_t mode;
    uint8_t passed;
    uint32_t first_error;
    uint64_t event_push_count;
    uint64_t event_pop_count;
    uint64_t event_full_wait_count;
    uint64_t event_empty_wait_count;
    uint32_t event_high_water;
    uint64_t pcm86_data_run_count;
    uint64_t pcm86_byte_push_bytes;
    uint64_t pcm86_byte_pop_bytes;
    uint64_t pcm86_byte_full_wait_count;
    uint64_t pcm86_byte_empty_wait_count;
    uint32_t pcm86_byte_high_water;
    uint64_t watermark_publish_count;
    uint64_t worker_wait_watermark_count;
    uint64_t producer_yield_count;
    uint64_t worker_yield_count;
    uint64_t transport_event_count;
    uint32_t transport_event_crc32;
    uint8_t transport_event_sha256[NP2_SHA256_DIGEST_SIZE];
};

/* Async fixture lifecycles are supported sequentially.  Independent
 * simultaneous calls remain unsupported because the upstream OPNGEN/PSG
 * initialization uses shared global configuration. */
int np2audio86_async_run(enum np2audio86_async_mode mode,
                         struct np2audio86_async_result *result);
const char *np2audio86_async_mode_name(enum np2audio86_async_mode mode);
const char *np2audio86_async_error_name(uint32_t error);

#if defined(NP2_AUDIO86_ASYNC_HARDENING_TEST)

enum np2audio86_async_test_fault {
    NP2_AUDIO86_TEST_FAULT_NONE = 0,
    NP2_AUDIO86_TEST_FAULT_DUPLICATE_SEQUENCE,
    NP2_AUDIO86_TEST_FAULT_SKIPPED_SEQUENCE,
    NP2_AUDIO86_TEST_FAULT_TIMESTAMP_BEHIND,
    NP2_AUDIO86_TEST_FAULT_TIMESTAMP_END,
    NP2_AUDIO86_TEST_FAULT_INVALID_OPCODE,
    NP2_AUDIO86_TEST_FAULT_RESERVED_OPCODE,
    NP2_AUDIO86_TEST_FAULT_DATA_ZERO,
    NP2_AUDIO86_TEST_FAULT_DATA_UNALIGNED,
    NP2_AUDIO86_TEST_FAULT_DATA_OVERSIZE,
    NP2_AUDIO86_TEST_FAULT_DATA_UNAVAILABLE,
    NP2_AUDIO86_TEST_FAULT_CUT_BEFORE_BYTES,
    NP2_AUDIO86_TEST_FAULT_CUT_AFTER_BYTES,
    NP2_AUDIO86_TEST_FAULT_CUT_AFTER_EVENT,
    NP2_AUDIO86_TEST_FAULT_CUT_AFTER_WATERMARK,
    NP2_AUDIO86_TEST_FAULT_WATERMARK_REGRESSION,
    NP2_AUDIO86_TEST_FAULT_WATERMARK_OVER_END,
    NP2_AUDIO86_TEST_FAULT_DONE_EARLY,
    NP2_AUDIO86_TEST_FAULT_WITHHOLD_FINAL,
    NP2_AUDIO86_TEST_FAULT_WATERMARK_PAST_EVENT,
    NP2_AUDIO86_TEST_FAULT_INCOMPLETE_GROUP,
    NP2_AUDIO86_TEST_FAULT_WORKER,
    NP2_AUDIO86_TEST_FAULT_FIRST_ERROR_IMMUTABILITY,
    NP2_AUDIO86_TEST_FAULT_PRODUCER_CREATE,
    NP2_AUDIO86_TEST_FAULT_WORKER_CREATE,
};

enum np2audio86_async_test_gate {
    NP2_AUDIO86_TEST_GATE_NONE = 0,
    NP2_AUDIO86_TEST_GATE_AFTER_BYTE_PUBLICATION,
    NP2_AUDIO86_TEST_GATE_AFTER_EVENT_PUBLICATION,
    NP2_AUDIO86_TEST_GATE_BEFORE_WATERMARK,
    NP2_AUDIO86_TEST_GATE_AFTER_WATERMARK,
    NP2_AUDIO86_TEST_GATE_BEFORE_EVENT_TAIL,
    NP2_AUDIO86_TEST_GATE_AFTER_BYTE_COPY,
    NP2_AUDIO86_TEST_GATE_WORKER_WATERMARK_WAIT,
    NP2_AUDIO86_TEST_GATE_PRODUCER_BYTE_FULL,
};

#define NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_PUBLICATION (1U << 0)
#define NP2_AUDIO86_TEST_YIELD_AFTER_EVENT_PUBLICATION (1U << 1)
#define NP2_AUDIO86_TEST_YIELD_BEFORE_WATERMARK (1U << 2)
#define NP2_AUDIO86_TEST_YIELD_AFTER_WATERMARK (1U << 3)
#define NP2_AUDIO86_TEST_YIELD_BEFORE_EVENT_TAIL (1U << 4)
#define NP2_AUDIO86_TEST_YIELD_AFTER_BYTE_COPY (1U << 5)
#define NP2_AUDIO86_TEST_YIELD_WATERMARK_WAIT (1U << 6)

struct np2audio86_async_test_control {
    uint32_t fault;
    uint32_t gate;
    uint32_t target_event;
    uint32_t yield_flags;
    NP2_AUDIO86_ASYNC_ATOMIC(bool) gate_reached;
    NP2_AUDIO86_ASYNC_ATOMIC(bool) gate_release;
    NP2_AUDIO86_ASYNC_ATOMIC(bool) fault_injected;
};

enum np2audio86_async_test_terminal {
    NP2_AUDIO86_TEST_TERMINAL_NOT_STARTED = 0,
    NP2_AUDIO86_TEST_TERMINAL_COMPLETED,
    NP2_AUDIO86_TEST_TERMINAL_ABORTED,
};

struct np2audio86_async_test_observer {
    uint32_t expected_error;
    uint32_t observed_error;
    uint8_t injected;
    uint8_t detected;
    uint8_t producer_created;
    uint8_t worker_created;
    uint8_t producer_reaped;
    uint8_t worker_reaped;
    uint8_t workload_success;
    uint8_t peer_unblocked;
    uint8_t later_error_attempted;
    uint32_t producer_terminal;
    uint32_t worker_terminal;
    uint32_t event_residual;
    uint32_t byte_residual;
};

void np2audio86_async_test_control_init(
    struct np2audio86_async_test_control *control);
int np2audio86_async_run_with_test_control(
    enum np2audio86_async_mode mode,
    struct np2audio86_async_test_control *control,
    struct np2audio86_async_test_observer *observer,
    struct np2audio86_async_result *result);
int np2audio86_async_test_prevalidate(unsigned case_id);
int np2audio86_async_test_byte_copy(
    const struct np2audio86_byte_ring *ring, uint8_t *bytes, size_t count);
int np2audio86_async_test_byte_consume(struct np2audio86_byte_ring *ring,
                                       size_t count);

#endif

/* Render the complete synchronous native reference into compact identities. */
int np2audio86_fixture_render(struct np2audio86_fixture_result *result);

/* Compare a result with the frozen compact golden constants. */
int np2audio86_fixture_matches_golden(
    const struct np2audio86_fixture_result *result);

/* Emit the stable machine-readable summary markers. */
void np2audio86_fixture_print_result(
    const struct np2audio86_fixture_result *result);

#ifdef __cplusplus
}
#endif

#undef NP2_AUDIO86_ASYNC_ATOMIC
#undef NP2_AUDIO86_STATIC_ASSERT

#endif /* NP2_AUDIO86_FIXTURE_H */
