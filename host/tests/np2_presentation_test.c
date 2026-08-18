#define _POSIX_C_SOURCE 200809L

#include "np2_presentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "presentation test failure: %s\n", message);
		return 0;
	}
	return 1;
}

static int check_status(np2_presentation_status actual,
	np2_presentation_status expected, const char *message)
{
	if (actual != expected) {
		fprintf(stderr, "presentation test failure: %s (got %d, expected %d)\n",
			message, (int)actual, (int)expected);
		return 0;
	}
	return 1;
}

static np2_presentation_source_view source_view(const uint8_t *ptr,
	uint32_t width, uint32_t height, size_t pitch, uint32_t generation,
	uint32_t sequence)
{
	np2_presentation_source_view source;

	source.ptr = ptr;
	source.width = width;
	source.height = height;
	source.pitch = pitch;
	source.bpp = 16U;
	source.pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE;
	source.source_generation = generation;
	source.source_update_sequence = sequence;
	return source;
}

static int test_initialization_and_slot_ranges(void)
{
	np2_presentation_publisher publisher = {0};
	np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_source_view source;
	np2_presentation_frame_view view;
	np2_presentation_token token;
	uint8_t backing[192];
	uint8_t distinct0[64];
	uint8_t distinct1[64];
	uint8_t pixels[4] = {0x12, 0x34, 0x56, 0x78};

	memset(backing, 0, sizeof(backing));
	publisher.published_sequence = UINT64_MAX;
	publisher.initialized = true;
	if (!check_status(np2_presentation_init(&publisher, NULL),
			NP2_PRESENTATION_INVALID_ARGUMENT, "failed init status") ||
			!check(!publisher.initialized, "failed init clears initialized")) {
		return 0;
	}

	slots[0].ptr = backing;
	slots[0].capacity = 64U;
	slots[1].ptr = backing;
	slots[1].capacity = 64U;
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_ALIAS, "exact slot alias rejected") ||
			!check(!publisher.initialized, "alias init leaves publisher unusable")) {
		return 0;
	}
	slots[1].ptr = backing + 32U;
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_ALIAS, "partial slot overlap rejected")) {
		return 0;
	}
	slots[0].capacity = 96U;
	slots[1].ptr = backing + 16U;
	slots[1].capacity = 16U;
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_ALIAS, "contained slot range rejected")) {
		return 0;
	}
	slots[0].ptr = backing;
	slots[0].capacity = 64U;
	slots[1].ptr = backing + 64U;
	slots[1].capacity = 64U;
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_OK, "adjacent slot ranges accepted")) {
		return 0;
	}
	slots[0].ptr = distinct0;
	slots[0].capacity = sizeof(distinct0);
	slots[1].ptr = distinct1;
	slots[1].capacity = sizeof(distinct1);
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_OK, "distinct slot ranges accepted")) {
		return 0;
	}
	source = source_view(pixels, 2U, 1U, 4U, 3U, 1U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_OK, "first submit after reset")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, &view, &token),
			NP2_PRESENTATION_OK, "first acquire after reset") ||
			!check(view.published_sequence == 1U,
				"first publication sequence resets to one")) {
		return 0;
	}
	return check_status(np2_presentation_release(&publisher, &token),
		NP2_PRESENTATION_OK, "release reset test frame");
}

static int test_invalid_api(void)
{
	np2_presentation_publisher publisher;
	np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_source_view source;
	uint8_t storage[2][64];
	uint8_t pixels[16];
	np2_presentation_frame_view view;
	np2_presentation_token token;

	memset(storage, 0, sizeof(storage));
	memset(pixels, 0, sizeof(pixels));
	slots[0].ptr = storage[0];
	slots[0].capacity = sizeof(storage[0]);
	slots[1].ptr = storage[1];
	slots[1].capacity = sizeof(storage[1]);
	if (!check_status(np2_presentation_init(NULL, slots),
			NP2_PRESENTATION_INVALID_ARGUMENT, "NULL publisher")) {
		return 0;
	}
	if (!check_status(np2_presentation_init(&publisher, NULL),
			NP2_PRESENTATION_INVALID_ARGUMENT, "NULL slot configuration")) {
		return 0;
	}
	slots[1].capacity = 0U;
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_INVALID_ARGUMENT, "zero slot capacity")) {
		return 0;
	}
	slots[1].capacity = sizeof(storage[1]);
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_OK, "valid initialization")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, NULL, &token),
			NP2_PRESENTATION_INVALID_ARGUMENT, "NULL acquired view")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, &view, NULL),
			NP2_PRESENTATION_INVALID_ARGUMENT, "NULL token")) {
		return 0;
	}
	source = source_view(pixels, 2U, 1U, 4U, 1U, 1U);
	source.bpp = 15U;
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_INVALID_FRAME, "unsupported bpp")) {
		return 0;
	}
	source.bpp = 16U;
	source.pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_NONE;
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_INVALID_FRAME, "unsupported pixel format")) {
		return 0;
	}
	source.pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE;
	source.width = 0U;
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_INVALID_FRAME, "zero width")) {
		return 0;
	}
	source.width = 2U;
	source.pitch = 3U;
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_INVALID_FRAME, "short source pitch")) {
		return 0;
	}
	source.pitch = 4U;
	source.width = UINT32_MAX;
	source.height = 2U;
	source.pitch = SIZE_MAX;
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_INVALID_FRAME, "source size overflow")) {
		return 0;
	}
	source = source_view(storage[0], 2U, 1U, 4U, 1U, 1U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_ALIAS, "source/storage alias")) {
		return 0;
	}
	return check_status(np2_presentation_acquire(&publisher, &view, &token),
		NP2_PRESENTATION_NO_FRAME, "acquire with no pending frame");
}

static int test_copy_and_coalescing(void)
{
	np2_presentation_publisher publisher;
	np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_source_view source;
	np2_presentation_frame_view old_view;
	np2_presentation_frame_view latest_view;
	np2_presentation_token old_token;
	np2_presentation_token latest_token;
	np2_presentation_token stale_token;
	uint8_t storage[2][64];
	uint8_t pixels[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xaa, 0xbb,
		0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0xcc, 0xdd
	};
	uint8_t old_pixels[12];
	uint8_t latest_pixels[12] = {
		0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
		0x51, 0x52, 0x53, 0x54, 0x55, 0x56
	};
	uint8_t next_pixels[12] = {
		0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
		0x71, 0x72, 0x73, 0x74, 0x75, 0x76
	};
	uint8_t newest_pixels[12] = {
		0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
		0x91, 0x92, 0x93, 0x94, 0x95, 0x96
	};

	slots[0].ptr = storage[0];
	slots[0].capacity = sizeof(storage[0]);
	slots[1].ptr = storage[1];
	slots[1].capacity = sizeof(storage[1]);
	if (!check_status(np2_presentation_init(&publisher, slots),
			NP2_PRESENTATION_OK, "publisher initialization")) {
		return 0;
	}
	source = source_view(pixels, 3U, 2U, 8U, 7U, 11U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_OK, "first submit")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, &old_view,
			&old_token), NP2_PRESENTATION_OK, "first acquire")) {
		return 0;
	}
	if (!check(old_view.ptr != source.ptr, "presentation pointer is owned")) {
		return 0;
	}
	if (!check((old_view.pitch == 6U) && (old_view.published_sequence == 1U) &&
			(old_view.source_generation == 7U) &&
			(old_view.source_update_sequence == 11U), "first metadata")) {
		return 0;
	}
	memcpy(old_pixels, old_view.ptr, sizeof(old_pixels));
	if (!check(memcmp(old_pixels, pixels, 6U) == 0 &&
			memcmp(old_pixels + 6U, pixels + 8U, 6U) == 0,
			"row copy omits padding")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, &latest_view,
			&latest_token), NP2_PRESENTATION_BUSY, "single acquired consumer")) {
		return 0;
	}
	stale_token = old_token;
	source = source_view(latest_pixels, 3U, 2U, 6U, 8U, 12U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_OK, "N+1 submit")) {
		return 0;
	}
	source = source_view(next_pixels, 3U, 2U, 6U, 9U, 13U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_OK, "N+2 coalescing submit")) {
		return 0;
	}
	source = source_view(newest_pixels, 3U, 2U, 6U, 10U, 14U);
	if (!check_status(np2_presentation_submit(&publisher, &source),
			NP2_PRESENTATION_OK, "N+3 coalescing submit")) {
		return 0;
	}
	if (!check(memcmp(old_view.ptr, old_pixels, sizeof(old_pixels)) == 0,
			"acquired frame remains unchanged")) {
		return 0;
	}
	if (!check(np2_presentation_coalesced_count(&publisher) == 2U,
			"coalescing count")) {
		return 0;
	}
	if (!check_status(np2_presentation_release(&publisher, &old_token),
			NP2_PRESENTATION_OK, "release first token")) {
		return 0;
	}
	if (!check_status(np2_presentation_acquire(&publisher, &latest_view,
			&latest_token), NP2_PRESENTATION_OK, "acquire latest pending")) {
		return 0;
	}
	if (!check(old_view.ptr != latest_view.ptr,
			"coalescing uses a distinct non-acquired slot") ||
			!check(latest_view.published_sequence == 4U &&
			latest_view.source_generation == 10U &&
			memcmp(latest_view.ptr, newest_pixels, sizeof(newest_pixels)) == 0,
			"latest pending frame")) {
		return 0;
	}
	if (!check_status(np2_presentation_release(&publisher, &latest_token),
			NP2_PRESENTATION_OK, "release latest token")) {
		return 0;
	}
	if (!check_status(np2_presentation_release(&publisher, &stale_token),
			NP2_PRESENTATION_INVALID_TOKEN, "stale token rejected")) {
		return 0;
	}
	if (!check_status(np2_presentation_release(&publisher, &latest_token),
			NP2_PRESENTATION_INVALID_TOKEN, "double release rejected")) {
		return 0;
	}
	return check(np2_presentation_dropped_count(&publisher) == 0U,
		"no drops in coalescing sequence");
}

static int test_capacity_and_timing(void)
{
	np2_presentation_publisher small_publisher;
	np2_presentation_slot_storage small_slots[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_source_view source;
	uint8_t small_storage[2][4];
	uint8_t small_pixels[8] = {0};
	uint8_t *source_pixels;
	uint8_t *presentation_pixels[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_slot_storage timing_slots[NP2_PRESENTATION_SLOT_COUNT];
	np2_presentation_publisher timing_publisher;
	np2_presentation_frame_view view;
	np2_presentation_token token;
	struct timespec begin;
	struct timespec end;
	long long elapsed_ns;
	size_t bytes = 640U * 400U * 2U;
	unsigned int index;

	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		small_slots[index].ptr = small_storage[index];
		small_slots[index].capacity = sizeof(small_storage[index]);
	}
	if (!check_status(np2_presentation_init(&small_publisher, small_slots),
			NP2_PRESENTATION_OK, "small publisher initialization")) {
		return 0;
	}
	source = source_view(small_pixels, 3U, 1U, 6U, 1U, 1U);
	if (!check_status(np2_presentation_submit(&small_publisher, &source),
			NP2_PRESENTATION_CAPACITY_EXCEEDED, "capacity exceeded status")) {
		return 0;
	}
	if (!check(np2_presentation_dropped_count(&small_publisher) == 1U,
			"capacity drop count")) {
		return 0;
	}
	source_pixels = malloc(bytes);
	if (!check(source_pixels != NULL, "timing source allocation")) {
		return 0;
	}
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		presentation_pixels[index] = malloc(bytes);
		if (!check(presentation_pixels[index] != NULL,
				"timing presentation allocation")) {
			free(source_pixels);
			return 0;
		}
		timing_slots[index].ptr = presentation_pixels[index];
		timing_slots[index].capacity = bytes;
	}
	if (!check_status(np2_presentation_init(&timing_publisher, timing_slots),
			NP2_PRESENTATION_OK, "timing publisher initialization")) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return 0;
	}
	memset(source_pixels, 0x5a, bytes);
	source = source_view(source_pixels, 640U, 400U, 640U * 2U, 1U, 1U);
	if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return check(0, "clock start");
	}
	if (!check_status(np2_presentation_submit(&timing_publisher, &source),
			NP2_PRESENTATION_OK, "640x400 timing submit")) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return 0;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return check(0, "clock end");
	}
	if (!check_status(np2_presentation_acquire(&timing_publisher, &view,
			&token), NP2_PRESENTATION_OK, "timing acquire")) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return 0;
	}
	if (!check_status(np2_presentation_release(&timing_publisher, &token),
			NP2_PRESENTATION_OK, "timing release")) {
		free(source_pixels);
		for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
			free(presentation_pixels[index]);
		}
		return 0;
	}
	elapsed_ns = ((long long)end.tv_sec - (long long)begin.tv_sec) * 1000000000LL +
		((long long)end.tv_nsec - (long long)begin.tv_nsec);
	printf("NP2PRESENTATION_COPY_NS=%lld\n", elapsed_ns);
	free(source_pixels);
	for (index = 0U; index < NP2_PRESENTATION_SLOT_COUNT; ++index) {
		free(presentation_pixels[index]);
	}
	return check(elapsed_ns >= 0, "nonnegative timing");
}

int main(void)
{
	if (!test_initialization_and_slot_ranges() || !test_invalid_api() ||
			!test_copy_and_coalescing() ||
			!test_capacity_and_timing()) {
		return 1;
	}
	printf("NP2PRESENTATION_UNIT_RESULT=PASS\n");
	return 0;
}
