#ifndef NP2_PRESENTATION_H
#define NP2_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define NP2_PRESENTATION_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define NP2_PRESENTATION_ATOMIC(type) _Atomic type
#endif

#define NP2_PRESENTATION_SLOT_COUNT 2U

typedef enum {
	NP2_PRESENTATION_PIXEL_FORMAT_NONE = 0,
	NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE = 1
} np2_presentation_pixel_format;

typedef struct {
	const uint8_t *ptr;
	uint32_t width;
	uint32_t height;
	size_t pitch;
	uint32_t bpp;
	np2_presentation_pixel_format pixel_format;
	uint32_t source_generation;
	uint32_t source_update_sequence;
} np2_presentation_source_view;

typedef struct {
	const uint8_t *ptr;
	uint32_t width;
	uint32_t height;
	size_t pitch;
	uint32_t bpp;
	np2_presentation_pixel_format pixel_format;
	uint32_t source_generation;
	uint32_t source_update_sequence;
	uint64_t published_sequence;
} np2_presentation_frame_view;

typedef struct {
	uint8_t *ptr;
	size_t capacity;
} np2_presentation_slot_storage;

typedef struct {
	uint32_t slot_index;
	uint32_t lease;
} np2_presentation_token;

/* The 32-bit lease skips zero.  A stale-token collision is theoretically
 * possible only after 2^32 acquisitions of the same slot if an ancient token
 * is retained; this bounded wrap domain is accepted for Step 7B.1. */

typedef enum {
	NP2_PRESENTATION_OK = 0,
	NP2_PRESENTATION_INVALID_ARGUMENT,
	NP2_PRESENTATION_INVALID_FRAME,
	NP2_PRESENTATION_NO_FRAME,
	NP2_PRESENTATION_BUSY,
	NP2_PRESENTATION_DROPPED,
	NP2_PRESENTATION_CAPACITY_EXCEEDED,
	NP2_PRESENTATION_ALIAS,
	NP2_PRESENTATION_INVALID_TOKEN
} np2_presentation_status;

/*
 * The caller owns the two backing buffers and this publisher object.  The
 * publisher never allocates, frees, or waits for a consumer.  A frame view
 * returned by acquire is immutable and its ptr remains valid until the
 * matching release.  A pending frame may be replaced by a newer submission;
 * an acquired frame is never overwritten.  The caller must unregister any
 * scrnmng hook before destroying this object or its backing storage.
 */
typedef struct {
	np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT];
	NP2_PRESENTATION_ATOMIC(uint32_t) slot_state[NP2_PRESENTATION_SLOT_COUNT];
	uint32_t slot_lease[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_frame_view frame[NP2_PRESENTATION_SLOT_COUNT];
	uint64_t published_sequence;
	NP2_PRESENTATION_ATOMIC(uint32_t) coalesced_count;
	NP2_PRESENTATION_ATOMIC(uint32_t) dropped_count;
	bool initialized;
} np2_presentation_publisher;

/*
 * The caller need not pre-zero publisher.  Successful initialization resets
 * sequence, counters, metadata, and states.  The two supplied backing ranges
 * must be disjoint and remain valid for the publisher lifetime.  Reinitializing
 * while producer or consumer operations are active is unsupported.
 */
#ifdef __cplusplus
extern "C" {
#endif

np2_presentation_status np2_presentation_init(
	np2_presentation_publisher *publisher,
	const np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT]);

np2_presentation_status np2_presentation_submit(
	np2_presentation_publisher *publisher,
	const np2_presentation_source_view *source);

np2_presentation_status np2_presentation_acquire(
	np2_presentation_publisher *publisher,
	np2_presentation_frame_view *view,
	np2_presentation_token *token);

np2_presentation_status np2_presentation_release(
	np2_presentation_publisher *publisher,
	np2_presentation_token *token);

uint32_t np2_presentation_coalesced_count(
	const np2_presentation_publisher *publisher);
uint32_t np2_presentation_dropped_count(
	const np2_presentation_publisher *publisher);

#ifdef __cplusplus
}
#endif

#endif /* NP2_PRESENTATION_H */
