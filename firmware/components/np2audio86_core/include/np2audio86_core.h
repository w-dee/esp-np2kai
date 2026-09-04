#ifndef NP2_AUDIO86_CORE_H
#define NP2_AUDIO86_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <compiler.h>
#include <sound/opngen.h>
#include <sound/pcmmix.h>
#include <sound/pcm86.h>
#include <sound/psggen.h>

#ifdef __cplusplus
#include <atomic>
#define NP2_AUDIO86_CORE_ATOMIC(type) std::atomic<type>
#define NP2_AUDIO86_CORE_STATIC_ASSERT static_assert
extern "C" {
#else
#include <stdatomic.h>
#define NP2_AUDIO86_CORE_ATOMIC(type) _Atomic type
#define NP2_AUDIO86_CORE_STATIC_ASSERT _Static_assert
#endif

#define NP2_AUDIO86_RATE_HZ 48000U
#define NP2_AUDIO86_QUANTUM_FRAMES 240U
#define NP2_AUDIO86_DURATION_FRAMES 2880000U
#define NP2_AUDIO86_QUANTA 12000U
#define NP2_AUDIO86_CHANNELS 2U
#define NP2_AUDIO86_PCM_BYTES \
    (NP2_AUDIO86_DURATION_FRAMES * NP2_AUDIO86_CHANNELS * 2U)

/* The source geometry is retained in the concrete renderer allocation so the
 * frozen 5D.3 fixture can decorate the neutral core without an ABI fork.  A
 * neutral core initialization leaves this storage empty and never refills it. */
#define NP2_AUDIO86_PCM86_SOURCE_RATE_HZ 44100U
#define NP2_AUDIO86_PCM86_SOURCE_CHANNELS 2U
#define NP2_AUDIO86_PCM86_SOURCE_BITS 16U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES 8192U
#define NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES \
    (NP2_AUDIO86_PCM86_SOURCE_PERIOD_FRAMES * 4U)

#define NP2_AUDIO86_FM_CHANNELS 6U
#define NP2_AUDIO86_RHYTHM_TRACKS 6U
#define NP2_AUDIO86_RHYTHM_MAX_SAMPLES 313U
#define NP2_AUDIO86_PCM86_FIFO_BYTES PCM86_BUFSIZE
#define NP2_AUDIO86_PCM86_REFILL_BYTES 32768U

#ifndef NP2_AUDIO86_ASYNC_EVENT_CAPACITY
#define NP2_AUDIO86_ASYNC_EVENT_CAPACITY 128U
#endif
#ifndef NP2_AUDIO86_ASYNC_BYTE_CAPACITY
#define NP2_AUDIO86_ASYNC_BYTE_CAPACITY 65536U
#endif
#ifndef NP2_AUDIO86_ASYNC_MAX_DATA_RUN
#define NP2_AUDIO86_ASYNC_MAX_DATA_RUN 32768U
#endif
#define NP2_AUDIO86_ASYNC_MAX_EVENTS 333U

#if (NP2_AUDIO86_ASYNC_EVENT_CAPACITY == 0U) || \
    ((NP2_AUDIO86_ASYNC_EVENT_CAPACITY & \
      (NP2_AUDIO86_ASYNC_EVENT_CAPACITY - 1U)) != 0U)
#error "NP2_AUDIO86_ASYNC_EVENT_CAPACITY must be a non-zero power of two"
#endif
#if (NP2_AUDIO86_ASYNC_BYTE_CAPACITY == 0U) || \
    ((NP2_AUDIO86_ASYNC_BYTE_CAPACITY & \
      (NP2_AUDIO86_ASYNC_BYTE_CAPACITY - 1U)) != 0U)
#error "NP2_AUDIO86_ASYNC_BYTE_CAPACITY must be a non-zero power of two"
#endif

NP2_AUDIO86_CORE_STATIC_ASSERT(
    NP2_AUDIO86_DURATION_FRAMES == NP2_AUDIO86_RATE_HZ * 60U,
    "86H.2 duration geometry");
NP2_AUDIO86_CORE_STATIC_ASSERT(
    NP2_AUDIO86_DURATION_FRAMES % NP2_AUDIO86_QUANTUM_FRAMES == 0U,
    "86H.2 quantum geometry");
NP2_AUDIO86_CORE_STATIC_ASSERT(
    NP2_AUDIO86_QUANTA * NP2_AUDIO86_QUANTUM_FRAMES ==
        NP2_AUDIO86_DURATION_FRAMES,
    "86H.2 quantum count");
NP2_AUDIO86_CORE_STATIC_ASSERT(NP2_AUDIO86_PCM_BYTES == 11520000U,
                               "86H.2 byte geometry");

enum np2audio86_event_opcode {
    NP2_AUDIO86_EVENT_FM_KEY = 1U,
    NP2_AUDIO86_EVENT_PSG_REGISTER = 2U,
    NP2_AUDIO86_EVENT_PCM86_DATA_RUN = 3U,
    NP2_AUDIO86_EVENT_RESET_BARRIER = 0x80000000U,
    NP2_AUDIO86_EVENT_BARRIER_RESERVED = NP2_AUDIO86_EVENT_RESET_BARRIER,
};

struct np2audio86_event {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
};

struct np2audio86_pcm86_feed {
    _PCM86 pcm;
    uint8_t source[NP2_AUDIO86_PCM86_SOURCE_PERIOD_BYTES];
    uint32_t source_frame;
    uint64_t supplied;
    uint32_t refills;
    int32_t fifo_min;
    int32_t fifo_max;
    uint8_t underrun;
};

/* Platform-neutral renderer allocation.  It contains no task, allocator,
 * physical sink, callback, or emulator-lifecycle ownership. */
struct np2audio86_render_state {
    _OPNGEN fm;
    _PSGGEN psg;
    struct {
        PMIXHDR hdr;
        PMIXTRK trk[NP2_AUDIO86_RHYTHM_TRACKS];
    } rhythm;
    PMIXTRK rhythm_tracks[NP2_AUDIO86_RHYTHM_TRACKS];
    SINT16 rhythm_samples[NP2_AUDIO86_RHYTHM_TRACKS]
                           [NP2_AUDIO86_RHYTHM_MAX_SAMPLES];
    struct np2audio86_pcm86_feed pcm86;
    SINT32 fm_scratch[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 psg_scratch[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 rhythm_scratch[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 pcm86_scratch[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    SINT32 mix_scratch[NP2_AUDIO86_QUANTUM_FRAMES * 2U];
    size_t next_event;
    uint64_t rendered_frames;
#if defined(NP2_AUDIO86_PROFILE)
    uint64_t profile_opngen_us;
    uint64_t profile_psggen_us;
    uint64_t profile_rhythm_us;
    uint64_t profile_pcm86_generation_us;
    uint64_t profile_mix_us;
    uint64_t (*profile_now_us)(void *opaque);
    void *profile_clock_opaque;
#endif
};

struct np2audio86_core_mix_result {
    uint8_t fm_contribution;
    uint8_t psg_contribution;
    uint8_t rhythm_contribution;
    uint8_t pcm86_contribution;
    uint8_t arithmetic_error;
};

enum np2audio86_transport_status {
    NP2_AUDIO86_TRANSPORT_OK = 0,
    NP2_AUDIO86_TRANSPORT_EMPTY,
    NP2_AUDIO86_TRANSPORT_FULL,
    NP2_AUDIO86_TRANSPORT_ARGUMENT,
    NP2_AUDIO86_TRANSPORT_INVARIANT,
};

#if defined(NP2AUDIO86_GUEST_TEST) && \
    defined(NP2_AUDIO86_GUEST_ASYNC_HARDENING_TEST)
enum np2audio86_async_hardening_cutpoint {
    NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_HEAD = 1,
    NP2_AUDIO86_ASYNC_CP_BYTE_HEAD_BEFORE_DATA_RUN,
    NP2_AUDIO86_ASYNC_CP_EVENT_SLOT_BEFORE_HEAD,
    NP2_AUDIO86_ASYNC_CP_DATA_RUN_BEFORE_BYTE_COPY,
    NP2_AUDIO86_ASYNC_CP_BYTE_COPY_BEFORE_TAIL,
    NP2_AUDIO86_ASYNC_CP_EVENT_BEFORE_TAIL,
    NP2_AUDIO86_ASYNC_CP_RESET_BEFORE_APPLY,
    NP2_AUDIO86_ASYNC_CP_RESET_AFTER_APPLY,
    NP2_AUDIO86_ASYNC_CP_ACK_BEFORE_PRODUCER_RESUME,
    NP2_AUDIO86_ASYNC_CP_PRODUCER_DONE_BEFORE_RELEASE,
    NP2_AUDIO86_ASYNC_CP_PRODUCER_DONE_BEFORE_TAIL_RENDER,
    NP2_AUDIO86_ASYNC_CP_COUNT,
};

int np2audio86_guest_async_hardening_cutpoint(
    enum np2audio86_async_hardening_cutpoint point,
    uint32_t event_head, uint32_t event_tail,
    uint32_t byte_head, uint32_t byte_tail, uint64_t auxiliary);
void np2audio86_guest_async_hardening_event_wrap(uint32_t head_before);
#endif

#if defined(NP2_AUDIO86_RESET_ORDINAL_TEST)
void np2audio86_reset_ordinal_after_publish_test_hook(uint32_t ordinal);
#endif

struct np2audio86_event_ring {
    struct np2audio86_event slots[NP2_AUDIO86_ASYNC_EVENT_CAPACITY];
    NP2_AUDIO86_CORE_ATOMIC(uint32_t) head;
    NP2_AUDIO86_CORE_ATOMIC(uint32_t) tail;
};

struct np2audio86_byte_ring {
    uint8_t bytes[NP2_AUDIO86_ASYNC_BYTE_CAPACITY];
    NP2_AUDIO86_CORE_ATOMIC(uint32_t) head;
    NP2_AUDIO86_CORE_ATOMIC(uint32_t) tail;
};

NP2_AUDIO86_CORE_STATIC_ASSERT(sizeof(struct np2audio86_event) == 24U,
                               "Audio86Event must remain 24 bytes");
#if NP2_AUDIO86_ASYNC_EVENT_CAPACITY == 128U
NP2_AUDIO86_CORE_STATIC_ASSERT(
    sizeof(((struct np2audio86_event_ring *)0)->slots) == 3072U,
    "event payload storage must remain 3072 bytes");
#endif
#if NP2_AUDIO86_ASYNC_BYTE_CAPACITY == 65536U
NP2_AUDIO86_CORE_STATIC_ASSERT(
    sizeof(((struct np2audio86_byte_ring *)0)->bytes) == 65536U,
    "byte payload storage must remain 65536 bytes");
#endif

void np2audio86_event_ring_init(struct np2audio86_event_ring *ring);
int np2audio86_event_ring_enqueue(struct np2audio86_event_ring *ring,
                                  const struct np2audio86_event *event);
int np2audio86_reset_event_ring_enqueue(
    struct np2audio86_event_ring *ring,
    const struct np2audio86_event *event,
    uint32_t *producer_reset_ordinal);
int np2audio86_event_ring_dequeue(struct np2audio86_event_ring *ring,
                                  struct np2audio86_event *event);
int np2audio86_event_ring_peek(const struct np2audio86_event_ring *ring,
                               const struct np2audio86_event **event);
int np2audio86_event_ring_consume(struct np2audio86_event_ring *ring);
uint32_t np2audio86_event_ring_occupancy(
    const struct np2audio86_event_ring *ring);

void np2audio86_byte_ring_init(struct np2audio86_byte_ring *ring);
int np2audio86_byte_ring_push(struct np2audio86_byte_ring *ring,
                             const uint8_t *bytes, size_t count);
int np2audio86_byte_ring_pop(struct np2audio86_byte_ring *ring,
                            uint8_t *bytes, size_t count);
int np2audio86_byte_ring_copy(const struct np2audio86_byte_ring *ring,
                             uint8_t *bytes, size_t count);
int np2audio86_byte_ring_consume(struct np2audio86_byte_ring *ring,
                                size_t count);
uint32_t np2audio86_byte_ring_occupancy(
    const struct np2audio86_byte_ring *ring);

/* Process-cold initialization is idempotent and is the sole owner of the
 * process-global OPN rate tables.  Neither object reset nor guest RESET calls
 * opngen_initialize(). */
int np2audio86_core_process_initialize(void);
int np2audio86_core_render_init(struct np2audio86_render_state *state);
int np2audio86_core_render_reset(struct np2audio86_render_state *state);
void np2audio86_core_render_destroy(struct np2audio86_render_state *state);
void np2audio86_core_render_set_profile_clock(
    struct np2audio86_render_state *state,
    uint64_t (*now_us)(void *opaque), void *opaque);
int np2audio86_core_render_apply_opna_register(
    struct np2audio86_render_state *state, uint16_t address, uint8_t value);
int np2audio86_core_render_apply_opna_csm(
    struct np2audio86_render_state *state);
int np2audio86_core_render_apply_pcm86_control(
    struct np2audio86_render_state *state, uint8_t register_index,
    uint8_t value);
int np2audio86_core_render_pcm86_push(
    struct np2audio86_render_state *state, const uint8_t *bytes,
    size_t count);
int np2audio86_core_render_span(
    struct np2audio86_render_state *state, SINT32 *mix, size_t frames,
    struct np2audio86_core_mix_result *result);

#if defined(NP2AUDIO86_GUEST_TEST)
void np2audio86_test_opngen_initialize_reset(void);
uint32_t np2audio86_test_opngen_initialize_call_count(void);
void np2audio86_test_opngen_initialize_fail_next(void);
#endif

enum np2audio86_core_guest_action_kind {
    NP2_AUDIO86_CORE_ACTION_OPNA_REGISTER = 1U,
    NP2_AUDIO86_CORE_ACTION_OPNA_CSM = 2U,
    NP2_AUDIO86_CORE_ACTION_PCM_CONTROL = 3U,
    NP2_AUDIO86_CORE_ACTION_RESET = 4U,
    NP2_AUDIO86_CORE_ACTION_DATA_RUN = 5U,
};

struct np2audio86_core_guest_action {
    uint64_t frame_timestamp;
    uint64_t sequence;
    uint32_t opcode;
    uint32_t payload;
    uint64_t byte_offset;
    uint32_t byte_count;
    uint8_t kind;
};

int np2audio86_core_guest_action_kind_for_opcode(uint32_t opcode,
                                                  uint8_t *kind);
int np2audio86_core_guest_action_apply(
    struct np2audio86_render_state *state,
    const struct np2audio86_core_guest_action *action,
    const uint8_t *data, size_t data_count);

#ifdef __cplusplus
}
#endif

#undef NP2_AUDIO86_CORE_ATOMIC
#undef NP2_AUDIO86_CORE_STATIC_ASSERT

#endif /* NP2_AUDIO86_CORE_H */
