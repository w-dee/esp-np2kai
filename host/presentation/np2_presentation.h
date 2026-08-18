#ifndef NP2_PRESENTATION_H
#define NP2_PRESENTATION_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
	_Atomic uint32_t slot_state[NP2_PRESENTATION_SLOT_COUNT];
	uint32_t slot_lease[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_frame_view frame[NP2_PRESENTATION_SLOT_COUNT];
	uint64_t published_sequence;
	_Atomic uint32_t coalesced_count;
	_Atomic uint32_t dropped_count;
	bool initialized;
} np2_presentation_publisher;

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

#endif /* NP2_PRESENTATION_H */
