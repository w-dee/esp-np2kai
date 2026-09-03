/* Canonical real-i286 PC-9801-86 guest program shared by host evidence and
 * the future P4 real-guest profile.  This module contains bytes only; it has
 * no host process, filesystem, or transport dependency. */
#ifndef NP2AUDIO86_GUEST_PROGRAM_H
#define NP2AUDIO86_GUEST_PROGRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t np2audio86_guest_program_build(uint8_t *out, size_t capacity);

/* Derived host/reference workload.  The original 86R.2 50 ms builder above
 * remains the canonical short fixture and is intentionally independent. */
#define NP2_AUDIO86_GUEST_SUSTAINED_2S_FRAMES 96000U
#define NP2_AUDIO86_GUEST_SUSTAINED_2S_BYTES 384000U
#define NP2_AUDIO86_GUEST_SUSTAINED_2S_QUANTA_240 400U
#define NP2_AUDIO86_GUEST_SUSTAINED_2S_POLL_COUNT 187U
#define NP2_AUDIO86_GUEST_SUSTAINED_INNER_COUNT 65536U
#define NP2_AUDIO86_GUEST_SUSTAINED_LOOP_CYCLES UINT64_C(98044100)

size_t np2audio86_guest_program_build_sustained_2s(uint8_t *out,
                                                   size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_GUEST_PROGRAM_H */
