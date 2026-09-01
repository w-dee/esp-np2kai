#ifndef NP2_AUDIO86_RUNTIME_TRANSPORT_H
#define NP2_AUDIO86_RUNTIME_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define NP2_AUDIO86_RUNTIME_ATOMIC(type) std::atomic<type>
#define NP2_AUDIO86_RUNTIME_STATIC_ASSERT static_assert
#define NP2_AUDIO86_RUNTIME_ALIGNOF(type) alignof(type)
extern "C" {
#else
#include <stdatomic.h>
#define NP2_AUDIO86_RUNTIME_ATOMIC(type) _Atomic type
#define NP2_AUDIO86_RUNTIME_STATIC_ASSERT _Static_assert
#define NP2_AUDIO86_RUNTIME_ALIGNOF(type) _Alignof(type)
#endif

/* Cross-core publication is deliberately limited to naturally aligned
 * 32-bit atomics.  Owner-local frame counters remain 64-bit ordinary data. */
struct np2audio86_runtime_control {
    NP2_AUDIO86_RUNTIME_ATOMIC(uint32_t) first_error;
    NP2_AUDIO86_RUNTIME_ATOMIC(uint32_t) stop;
    NP2_AUDIO86_RUNTIME_ATOMIC(uint32_t) producer_done;
    NP2_AUDIO86_RUNTIME_ATOMIC(uint32_t) reset_ack_ordinal;
    NP2_AUDIO86_RUNTIME_ATOMIC(uint32_t) committed_frame_low32;
};

struct np2audio86_runtime_producer_clock {
    uint64_t committed_frame_owner;
};

struct np2audio86_runtime_consumer_clock {
    uint64_t committed_frame_reconstructed;
    uint32_t previous_low32;
};

enum np2audio86_runtime_horizon_status {
    NP2_AUDIO86_RUNTIME_HORIZON_OK = 0,
    NP2_AUDIO86_RUNTIME_HORIZON_ARGUMENT,
    NP2_AUDIO86_RUNTIME_HORIZON_NONMONOTONIC,
    NP2_AUDIO86_RUNTIME_HORIZON_AMBIGUOUS,
    NP2_AUDIO86_RUNTIME_HORIZON_OVERFLOW,
};

NP2_AUDIO86_RUNTIME_STATIC_ASSERT(
    sizeof(struct np2audio86_runtime_control) == 20U,
    "86R.5 control state must remain five 32-bit atomics");
NP2_AUDIO86_RUNTIME_STATIC_ASSERT(
    NP2_AUDIO86_RUNTIME_ALIGNOF(struct np2audio86_runtime_control) >= 4U,
    "86R.5 control state must be naturally aligned");
NP2_AUDIO86_RUNTIME_STATIC_ASSERT(
    offsetof(struct np2audio86_runtime_control, first_error) % 4U == 0U &&
        offsetof(struct np2audio86_runtime_control, stop) % 4U == 0U &&
        offsetof(struct np2audio86_runtime_control, producer_done) % 4U == 0U &&
        offsetof(struct np2audio86_runtime_control, reset_ack_ordinal) % 4U == 0U &&
        offsetof(struct np2audio86_runtime_control, committed_frame_low32) % 4U == 0U,
    "86R.5 atomic fields must be 4-byte aligned");

void np2audio86_runtime_control_init(
    struct np2audio86_runtime_control *control);
bool np2audio86_runtime_first_error_publish(
    struct np2audio86_runtime_control *control, uint32_t error);
uint32_t np2audio86_runtime_first_error(
    const struct np2audio86_runtime_control *control);
void np2audio86_runtime_stop_publish(
    struct np2audio86_runtime_control *control);
bool np2audio86_runtime_stop_requested(
    const struct np2audio86_runtime_control *control);
void np2audio86_runtime_producer_done_publish(
    struct np2audio86_runtime_control *control);
bool np2audio86_runtime_producer_done(
    const struct np2audio86_runtime_control *control);
void np2audio86_runtime_reset_ack_publish(
    struct np2audio86_runtime_control *control, uint32_t ordinal);
uint32_t np2audio86_runtime_reset_ack(
    const struct np2audio86_runtime_control *control);
int np2audio86_runtime_horizon_publish(
    struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_producer_clock *producer, uint64_t frame);
int np2audio86_runtime_horizon_observe(
    const struct np2audio86_runtime_control *control,
    struct np2audio86_runtime_consumer_clock *consumer);

#ifdef __cplusplus
}
#endif

#endif /* NP2_AUDIO86_RUNTIME_TRANSPORT_H */
