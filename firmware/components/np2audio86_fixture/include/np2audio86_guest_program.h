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

#ifdef __cplusplus
}
#endif

#endif /* NP2AUDIO86_GUEST_PROGRAM_H */
