#include "np2_presentation.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	NP2_PRESENTATION_SLOT_FREE = 0,
	NP2_PRESENTATION_SLOT_WRITING = 1,
	NP2_PRESENTATION_SLOT_PENDING = 2,
	NP2_PRESENTATION_SLOT_ACQUIRED = 3
};

static bool np2_presentation_checked_sizes(
	const np2_presentation_source_view *source,
	size_t *row_bytes, size_t *visible_bytes, size_t *source_span)
{
	size_t rows_before_last;

	if ((source == NULL) || (source->ptr == NULL) ||
			(source->width == 0U) || (source->height == 0U) ||
			(source->bpp != 16U) ||
			(source->pixel_format != NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE)) {
		return false;
	}
	*row_bytes = (size_t)source->width * sizeof(uint16_t);
	if ((*row_bytes == 0U) ||
			(*row_bytes / sizeof(uint16_t)) != (size_t)source->width) {
		return false;
	}
	if (source->pitch < *row_bytes) {
		return false;
	}
	*visible_bytes = (size_t)source->height * *row_bytes;
	if ((*visible_bytes / *row_bytes) != (size_t)source->height) {
		return false;
	}
	rows_before_last = (size_t)source->height - 1U;
	if (rows_before_last > (SIZE_MAX - *row_bytes) / source->pitch) {
		return false;
	}
	*source_span = rows_before_last * source->pitch + *row_bytes;
	return true;
}

static bool np2_presentation_ranges_overlap(
	const void *a, size_t a_size, const void *b, size_t b_size)
{
	uintptr_t a_start;
	uintptr_t b_start;

	if ((a_size == 0U) || (b_size == 0U)) {
		return false;
	}
	a_start = (uintptr_t)a;
	b_start = (uintptr_t)b;
	if ((a_start > UINTPTR_MAX - a_size) ||
			(b_start > UINTPTR_MAX - b_size)) {
		return true;
	}
	return (a_start < b_start + b_size) &&
			(b_start < a_start + a_size);
}

static bool np2_presentation_source_aliases_slot(
	const np2_presentation_source_view *source,
	size_t source_span, const np2_presentation_slot_storage *slot)
{
	return np2_presentation_ranges_overlap(source->ptr, source_span,
		slot->ptr, slot->capacity);
}

static int np2_presentation_claim_slot(
	np2_presentation_publisher *publisher, size_t visible_bytes,
	bool *replaced_pending)
{
	uint32_t expected;
	unsigned int index;

	*replaced_pending = false;
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		if (publisher->slots[index].capacity < visible_bytes) {
			continue;
		}
		if (atomic_load_explicit(&publisher->slot_state[index],
				memory_order_acquire) != NP2_PRESENTATION_SLOT_PENDING) {
			continue;
		}
		expected = NP2_PRESENTATION_SLOT_PENDING;
		if (atomic_compare_exchange_strong_explicit(
				&publisher->slot_state[index], &expected,
				NP2_PRESENTATION_SLOT_WRITING, memory_order_acq_rel,
				memory_order_acquire)) {
			*replaced_pending = true;
			return (int)index;
		}
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		if (publisher->slots[index].capacity < visible_bytes) {
			continue;
		}
		if (atomic_load_explicit(&publisher->slot_state[index],
				memory_order_acquire) != NP2_PRESENTATION_SLOT_FREE) {
			continue;
		}
		expected = NP2_PRESENTATION_SLOT_FREE;
		if (atomic_compare_exchange_strong_explicit(
				&publisher->slot_state[index], &expected,
				NP2_PRESENTATION_SLOT_WRITING, memory_order_acq_rel,
				memory_order_acquire)) {
			return (int)index;
		}
	}
	return -1;
}

np2_presentation_status np2_presentation_init(
	np2_presentation_publisher *publisher,
	const np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT])
{
	unsigned int index;

	if ((publisher == NULL) || (slots == NULL)) {
		return NP2_PRESENTATION_INVALID_ARGUMENT;
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		if ((slots[index].ptr == NULL) || (slots[index].capacity == 0U)) {
			return NP2_PRESENTATION_INVALID_ARGUMENT;
		}
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		publisher->slots[index] = slots[index];
		publisher->slot_lease[index] = 0U;
		publisher->frame[index] = (np2_presentation_frame_view){0};
		atomic_init(&publisher->slot_state[index],
			NP2_PRESENTATION_SLOT_FREE);
	}
	atomic_init(&publisher->coalesced_count, 0U);
	atomic_init(&publisher->dropped_count, 0U);
	publisher->initialized = true;
	return NP2_PRESENTATION_OK;
}

np2_presentation_status np2_presentation_submit(
	np2_presentation_publisher *publisher,
	const np2_presentation_source_view *source)
{
	size_t row_bytes;
	size_t visible_bytes;
	size_t source_span;
	bool replaced_pending;
	int claimed;
	unsigned int index;
	uint32_t row;
	np2_presentation_frame_view *frame;

	if ((publisher == NULL) || (source == NULL) || !publisher->initialized) {
		return NP2_PRESENTATION_INVALID_ARGUMENT;
	}
	if (!np2_presentation_checked_sizes(source, &row_bytes,
			&visible_bytes, &source_span)) {
		return NP2_PRESENTATION_INVALID_FRAME;
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		if (np2_presentation_source_aliases_slot(source, source_span,
				&publisher->slots[index])) {
			return NP2_PRESENTATION_ALIAS;
		}
	}
	claimed = np2_presentation_claim_slot(publisher, visible_bytes,
		&replaced_pending);
	if (claimed < 0) {
		bool has_capacity = false;

		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			if (publisher->slots[index].capacity >= visible_bytes) {
				has_capacity = true;
				break;
			}
		}
		(void)atomic_fetch_add_explicit(&publisher->dropped_count, 1U,
			memory_order_relaxed);
		return has_capacity ? NP2_PRESENTATION_DROPPED :
			NP2_PRESENTATION_CAPACITY_EXCEEDED;
	}

	frame = &publisher->frame[claimed];
	for (row = 0U; row < source->height; ++row) {
		memcpy(publisher->slots[claimed].ptr + (size_t)row * row_bytes,
			source->ptr + (size_t)row * source->pitch, row_bytes);
	}
	publisher->published_sequence++;
	frame->ptr = publisher->slots[claimed].ptr;
	frame->width = source->width;
	frame->height = source->height;
	frame->pitch = row_bytes;
	frame->bpp = source->bpp;
	frame->pixel_format = source->pixel_format;
	frame->source_generation = source->source_generation;
	frame->source_update_sequence = source->source_update_sequence;
	frame->published_sequence = publisher->published_sequence;
	if (replaced_pending) {
		(void)atomic_fetch_add_explicit(&publisher->coalesced_count, 1U,
			memory_order_relaxed);
	}
	atomic_store_explicit(&publisher->slot_state[claimed],
		NP2_PRESENTATION_SLOT_PENDING, memory_order_release);
	return NP2_PRESENTATION_OK;
}

np2_presentation_status np2_presentation_acquire(
	np2_presentation_publisher *publisher,
	np2_presentation_frame_view *view,
	np2_presentation_token *token)
{
	unsigned int index;
	uint32_t expected;

	if ((publisher == NULL) || (view == NULL) || (token == NULL) ||
			!publisher->initialized) {
		return NP2_PRESENTATION_INVALID_ARGUMENT;
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		if (atomic_load_explicit(&publisher->slot_state[index],
				memory_order_acquire) == NP2_PRESENTATION_SLOT_ACQUIRED) {
			return NP2_PRESENTATION_BUSY;
		}
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		expected = NP2_PRESENTATION_SLOT_PENDING;
		if (!atomic_compare_exchange_strong_explicit(
				&publisher->slot_state[index], &expected,
				NP2_PRESENTATION_SLOT_ACQUIRED, memory_order_acq_rel,
				memory_order_acquire)) {
			continue;
		}
		if (publisher->slot_lease[index] == UINT32_MAX) {
			publisher->slot_lease[index] = 1U;
		} else {
			++publisher->slot_lease[index];
		}
		*view = publisher->frame[index];
		token->slot_index = index;
		token->lease = publisher->slot_lease[index];
		return NP2_PRESENTATION_OK;
	}
	return NP2_PRESENTATION_NO_FRAME;
}

np2_presentation_status np2_presentation_release(
	np2_presentation_publisher *publisher, np2_presentation_token *token)
{
	uint32_t expected;

	if ((publisher == NULL) || (token == NULL) || !publisher->initialized ||
			(token->slot_index >= NP2_PRESENTATION_SLOT_COUNT) ||
			(token->lease == 0U)) {
		return NP2_PRESENTATION_INVALID_TOKEN;
	}
	if (atomic_load_explicit(&publisher->slot_state[token->slot_index],
			memory_order_acquire) != NP2_PRESENTATION_SLOT_ACQUIRED ||
			publisher->slot_lease[token->slot_index] != token->lease) {
		return NP2_PRESENTATION_INVALID_TOKEN;
	}
	expected = NP2_PRESENTATION_SLOT_ACQUIRED;
	if (!atomic_compare_exchange_strong_explicit(
			&publisher->slot_state[token->slot_index], &expected,
			NP2_PRESENTATION_SLOT_FREE, memory_order_release,
			memory_order_acquire)) {
		return NP2_PRESENTATION_INVALID_TOKEN;
	}
	token->slot_index = UINT32_MAX;
	token->lease = 0U;
	return NP2_PRESENTATION_OK;
}

uint32_t np2_presentation_coalesced_count(
	const np2_presentation_publisher *publisher)
{
	if (publisher == NULL) {
		return 0U;
	}
	return atomic_load_explicit(&publisher->coalesced_count,
		memory_order_relaxed);
}

uint32_t np2_presentation_dropped_count(
	const np2_presentation_publisher *publisher)
{
	if (publisher == NULL) {
		return 0U;
	}
	return atomic_load_explicit(&publisher->dropped_count,
		memory_order_relaxed);
}
