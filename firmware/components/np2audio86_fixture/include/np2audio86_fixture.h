#ifndef NP2_AUDIO86_FIXTURE_H
#define NP2_AUDIO86_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <compiler.h>
#include "np2_sha256.h"
#include <sound/opngen.h>
#include <sound/pcmmix.h>
#include <sound/pcm86.h>
#include <sound/psggen.h>

#include "np2audio86_core.h"

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

/* Portable plan and incremental renderer API shared by native and P4 paths. */
int np2audio86_async_build_plan(struct np2audio86_event *plan,
                                size_t *count);
int np2audio86_async_validate_plan(const struct np2audio86_event *plan,
                                   size_t count);
int np2audio86_render_init(struct np2audio86_render_state *state);
int np2audio86_render_init_with_source(struct np2audio86_render_state *state,
                                       const uint8_t *source);
/* Fixture-private mutable-state decorator.  The caller must first establish
 * a neutral core instance with core_render_init/reset. */
int np2audio86_fixture_decorate_render_state(
    struct np2audio86_render_state *state);
#if defined(NP2AUDIO86_GUEST_TEST)
/* Host-only seams for proving cold-initialization ownership and propagation. */
void np2audio86_test_opngen_initialize_reset(void);
uint32_t np2audio86_test_opngen_initialize_call_count(void);
void np2audio86_test_opngen_initialize_fail_next(void);
#endif
void np2audio86_render_set_profile_clock(
    struct np2audio86_render_state *state,
    uint64_t (*now_us)(void *opaque), void *opaque);
int np2audio86_render_apply_event(struct np2audio86_render_state *state,
                                  const struct np2audio86_event *event);
/* Narrow synchronous Domain-A façade for the authentic 86R guest replay.
 * These operations reuse the same OPNGEN/PSG/rhythm/PCM state and render
 * paths as the established 86H fixture; no guest-domain state crosses here. */
int np2audio86_render_apply_opna_register(
    struct np2audio86_render_state *state, uint16_t address, uint8_t value);
int np2audio86_render_apply_opna_csm(struct np2audio86_render_state *state);
int np2audio86_render_apply_pcm86_control(
    struct np2audio86_render_state *state, uint8_t register_index,
    uint8_t value);
/* Reset mutable renderer state after a successful cold render_init call in
 * the same runtime lifetime.  Process-global OPN tables remain unchanged. */
int np2audio86_render_reset(struct np2audio86_render_state *state);
int np2audio86_render_pcm86_push(struct np2audio86_render_state *state,
                                 const uint8_t *bytes, size_t count);
int np2audio86_render_span(struct np2audio86_render_state *state,
                           SINT32 *mix, size_t frames,
                           struct np2audio86_fixture_result *result);
int np2audio86_render_quantum(struct np2audio86_render_state *state,
                              const struct np2audio86_event *plan,
                              size_t plan_count, uint8_t *canonical,
                              size_t canonical_bytes,
                              struct np2audio86_fixture_result *result);
void np2audio86_fixture_hash_control(struct np2audio86_fixture_result *result);
void np2audio86_fixture_hash_source(struct np2audio86_fixture_result *result,
                                    const uint8_t *source);
int np2audio86_fixture_generate_source(uint8_t *source);
int np2audio86_fixture_control_matches_golden(
    const struct np2audio86_fixture_result *result);
int np2audio86_fixture_source_matches_golden(
    const struct np2audio86_fixture_result *result);

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
