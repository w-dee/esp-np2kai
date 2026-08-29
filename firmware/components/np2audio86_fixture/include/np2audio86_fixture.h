#ifndef NP2_AUDIO86_FIXTURE_H
#define NP2_AUDIO86_FIXTURE_H

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

int np2audio86_async_run(enum np2audio86_async_mode mode,
                         struct np2audio86_async_result *result);
const char *np2audio86_async_mode_name(enum np2audio86_async_mode mode);
const char *np2audio86_async_error_name(uint32_t error);

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
