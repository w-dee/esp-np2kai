#include <compiler.h>
#include <scrnmng.h>

#include "np2_presentation.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	np2_presentation_publisher *publisher;
	np2_presentation_status last_status;
	unsigned int callback_count;
	uintptr_t last_guest_address;
} hook_context;

static int check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "scrnmng presentation test failure: %s\n", message);
		return 0;
	}
	return 1;
}

static void publish_hook(const SCRNMNG_PUBLISH_VIEW *view, void *opaque)
{
	hook_context *context = (hook_context *)opaque;
	np2_presentation_source_view source;

	context->callback_count++;
	context->last_guest_address = (uintptr_t)view->ptr;
	source.ptr = view->ptr;
	source.width = (uint32_t)view->width;
	source.height = (uint32_t)view->height;
	source.pitch = view->pitch;
	source.bpp = view->bpp;
	source.pixel_format = (np2_presentation_pixel_format)view->pixel_format;
	source.source_generation = view->surface_generation;
	source.source_update_sequence = view->surface_update_sequence;
	context->last_status = np2_presentation_submit(context->publisher, &source);
}

static void fill_surface(const SCRNSURF *surface, UINT8 base)
{
	int y;
	int x;

	for (y = 0; y < surface->height; ++y) {
		for (x = 0; x < surface->width * 2; ++x) {
			surface->ptr[(size_t)y * (size_t)surface->yalign +
				(size_t)x] = (UINT8)(base + (UINT8)(y * 17 + x));
		}
	}
}

static int check_frame(const np2_presentation_frame_view *view,
	uint32_t width, uint32_t height, uint32_t generation,
	uint32_t sequence, UINT8 base)
{
	uint32_t y;
	uint32_t x;

	if (view->width != width || view->height != height ||
			view->pitch != (size_t)width * 2U || view->bpp != 16U ||
			view->pixel_format != NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE ||
			view->source_generation != generation ||
			view->source_update_sequence != sequence) {
		return 0;
	}
	for (y = 0U; y < height; ++y) {
		for (x = 0U; x < width * 2U; ++x) {
			if (view->ptr[(size_t)y * view->pitch + x] !=
					(UINT8)(base + (UINT8)(y * 17U + x))) {
				return 0;
			}
		}
	}
	return 1;
}

int main(void)
{
	np2_presentation_publisher publisher;
	np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT];
	uint8_t presentation_storage[NP2_PRESENTATION_SLOT_COUNT][16U * 4U * 2U];
	hook_context context;
	const SCRNSURF *surface;
	const SCRNSURF *new_surface;
	np2_presentation_frame_view old_view;
	np2_presentation_frame_view next_view;
	np2_presentation_frame_view resized_view;
	np2_presentation_token old_token;
	np2_presentation_token next_token;
	np2_presentation_token resized_token;
	uint8_t old_pixels[8U * 4U * 2U];
	uint32_t generation;
	uint32_t sequence;
	unsigned int index;
	unsigned int callbacks_before_lock;

	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		slots[index].ptr = presentation_storage[index];
		slots[index].capacity = sizeof(presentation_storage[index]);
	}
	if (!check(np2_presentation_init(&publisher, slots) == NP2_PRESENTATION_OK,
			"publisher initialization")) {
		return 1;
	}
	memset(&context, 0, sizeof(context));
	context.publisher = &publisher;
	context.last_status = NP2_PRESENTATION_INVALID_ARGUMENT;
	scrnmng_shutdown();
	scrnmng_setwidth(0, 8);
	scrnmng_setheight(0, 4);
	if (!check(scrnmng_initialize(), "headless initialization")) {
		return 1;
	}
	scrnmng_set_publish_hook(publish_hook, &context);
	surface = scrnmng_surflock();
	if (!check(surface != NULL, "first guest lock")) {
		return 1;
	}
	fill_surface(surface, 0x10U);
	if (!check(np2_presentation_acquire(&publisher, &old_view, &old_token) ==
			NP2_PRESENTATION_NO_FRAME, "no publish before unlock")) {
		scrnmng_surfunlock(surface);
		return 1;
	}
	scrnmng_surfunlock(surface);
	if (!check(context.callback_count == 1U &&
			context.last_status == NP2_PRESENTATION_OK,
			"first synchronous publish")) {
		return 1;
	}
	if (!check(np2_presentation_acquire(&publisher, &old_view, &old_token) ==
			NP2_PRESENTATION_OK, "first presentation acquire") ||
			!check(old_view.ptr != surface->ptr,
				"presentation does not expose guest pointer") ||
			!check(check_frame(&old_view, 8U, 4U, 1U, 1U, 0x10U),
				"first presentation metadata/pixels")) {
		return 1;
	}
	memcpy(old_pixels, old_view.ptr, sizeof(old_pixels));

	surface = scrnmng_surflock();
	if (!check(surface != NULL, "second guest lock")) {
		return 1;
	}
	fill_surface(surface, 0x30U);
	callbacks_before_lock = context.callback_count;
	if (!check(memcmp(old_view.ptr, old_pixels, sizeof(old_pixels)) == 0 &&
			context.callback_count == callbacks_before_lock,
			"acquired frame is independent while guest is locked")) {
		scrnmng_surfunlock(surface);
		return 1;
	}
	scrnmng_surfunlock(surface);
	if (!check(context.callback_count == callbacks_before_lock + 1U,
			"second publish occurs at unlock")) {
		return 1;
	}
	if (!check(np2_presentation_release(&publisher, &old_token) ==
			NP2_PRESENTATION_OK, "release first presentation")) {
		return 1;
	}
	if (!check(np2_presentation_acquire(&publisher, &next_view, &next_token) ==
			NP2_PRESENTATION_OK &&
			check_frame(&next_view, 8U, 4U, 1U, 2U, 0x30U),
			"second presentation metadata/pixels")) {
		return 1;
	}

	if (!check(np2_presentation_release(&publisher, &next_token) ==
			NP2_PRESENTATION_OK, "release second presentation")) {
		return 1;
	}
	if (!check(np2_presentation_acquire(&publisher, &next_view, &next_token) ==
			NP2_PRESENTATION_NO_FRAME, "empty pending state")) {
		return 1;
	}

	/* Resize frees the guest surface, but never touches the acquired copy. */
	surface = scrnmng_surflock();
	if (!check(surface != NULL, "resize preparation lock")) {
		return 1;
	}
	fill_surface(surface, 0x50U);
	scrnmng_surfunlock(surface);
	if (!check(np2_presentation_acquire(&publisher, &next_view, &next_token) ==
			NP2_PRESENTATION_OK, "acquired frame before resize")) {
		return 1;
	}
	memcpy(old_pixels, next_view.ptr, sizeof(old_pixels));
	scrnmng_setwidth(0, 16);
	scrnmng_setheight(0, 4);
	if (!check(memcmp(next_view.ptr, old_pixels, sizeof(old_pixels)) == 0,
			"acquired frame survives guest resize")) {
		return 1;
	}
	scrnmng_get_surface_counters(&generation, &sequence);
	if (!check(generation == 2U && sequence == 3U,
			"resize generation and completed sequence")) {
		return 1;
	}
	new_surface = scrnmng_surflock();
	if (!check(new_surface != NULL && new_surface->width == 16 &&
			new_surface->height == 4, "resized guest surface")) {
		return 1;
	}
	fill_surface(new_surface, 0x70U);
	callbacks_before_lock = context.callback_count;
	if (!check(context.callback_count == callbacks_before_lock &&
			memcmp(next_view.ptr, old_pixels, sizeof(old_pixels)) == 0,
			"old acquired frame remains stable during resized lock")) {
		scrnmng_surfunlock(new_surface);
		return 1;
	}
	scrnmng_surfunlock(new_surface);
	if (!check(context.callback_count == callbacks_before_lock + 1U,
			"resized publish at unlock")) {
		return 1;
	}
	if (!check(np2_presentation_release(&publisher, &next_token) ==
			NP2_PRESENTATION_OK, "release pre-resize presentation")) {
		return 1;
	}
	if (!check(np2_presentation_acquire(&publisher, &resized_view,
			&resized_token) == NP2_PRESENTATION_OK &&
			check_frame(&resized_view, 16U, 4U, 2U, 4U, 0x70U),
			"resized presentation metadata/pixels")) {
		return 1;
	}
	if (!check((uintptr_t)resized_view.ptr != context.last_guest_address,
			"resized presentation pointer remains owned")) {
		return 1;
	}
	if (!check(np2_presentation_release(&publisher, &resized_token) ==
			NP2_PRESENTATION_OK, "release resized presentation")) {
		return 1;
	}
	scrnmng_set_publish_hook(NULL, NULL);
	callbacks_before_lock = context.callback_count;
	scrnmng_shutdown();
	if (!check(context.callback_count == callbacks_before_lock,
			"shutdown does not invoke hook")) {
		return 1;
	}
	printf("NP2SCRNMNG_PRESENTATION_RESULT=PASS\n");
	return 0;
}
