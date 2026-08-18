#include "np2presentation_probe.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <compiler.h>
#include "np2_presentation.h"
#include "scrnmng.h"

#define NP2PRESENT_SLOT_BYTES (640U * 400U * 2U)
#define NP2PRESENT_SLOT_COUNT 2U
#define NP2PRESENT_TIMEOUT_MS 5000U
#define NP2PRESENT_CONSUMER_STACK_WORDS 4096U

typedef enum {
	NP2PRESENT_COMMAND_CHECK_NO_FRAME = 1,
	NP2PRESENT_COMMAND_ACQUIRE = 2,
	NP2PRESENT_COMMAND_CHECK_HELD = 3,
	NP2PRESENT_COMMAND_RELEASE_ACQUIRE = 4,
	NP2PRESENT_COMMAND_STOP = 5
} np2present_command;

typedef struct {
	np2_presentation_publisher publisher;
	np2_presentation_slot_storage slots[NP2PRESENT_SLOT_COUNT];
	TaskHandle_t producer_task;
	TaskHandle_t consumer_task;
	np2present_command command;
	bool consumer_alive;
	bool consumer_holds_frame;
	bool command_ok;
	const char *command_reason;
	np2_presentation_frame_view held_view;
	np2_presentation_token held_token;
	np2_presentation_status last_submit_status;
	uint8_t expected_frame_id;
	uint32_t expected_width;
	uint32_t expected_height;
	uint32_t expected_generation;
	uint32_t expected_update_sequence;
	uint64_t expected_published_sequence;
	uintptr_t expected_guest_address;
	uint32_t last_checked_hash;
	uint32_t last_old_hash;
	uint32_t guest_bytes;
	uint32_t guest_generation;
	uint32_t guest_sequence;
	bool guest_initialized;
	bool publisher_initialized;
	bool hook_registered;
	bool slot0_allocated;
	bool slot1_allocated;
} np2present_state;

static np2present_state s_np2present;
static StaticTask_t s_consumer_task_buffer;
static StackType_t s_consumer_stack[NP2PRESENT_CONSUMER_STACK_WORDS];

static uint8_t np2present_pattern_byte(uint8_t frame_id, size_t index)
{
	return (uint8_t)((unsigned)frame_id * 29U + index * 17U + (index >> 8));
}

static uint32_t np2present_frame_hash(uint8_t frame_id, uint32_t width,
	uint32_t height)
{
	uint32_t hash = UINT32_C(2166136261);
	uint32_t row;
	uint32_t column;
	size_t row_bytes = (size_t)width * 2U;

	for (row = 0U; row < height; ++row) {
		for (column = 0U; column < row_bytes; ++column) {
			hash ^= np2present_pattern_byte(frame_id,
				(size_t)row * row_bytes + column);
			hash *= UINT32_C(16777619);
		}
	}
	return hash;
}

static uint32_t np2present_view_hash(
	const np2_presentation_frame_view *view)
{
	uint32_t hash = UINT32_C(2166136261);
	uint32_t row;
	uint32_t column;
	size_t row_bytes = (size_t)view->width * 2U;

	for (row = 0U; row < view->height; ++row) {
		for (column = 0U; column < row_bytes; ++column) {
			hash ^= view->ptr[(size_t)row * view->pitch + column];
			hash *= UINT32_C(16777619);
		}
	}
	return hash;
}

static void np2present_write_guest_frame(const SCRNSURF *surface,
	uint8_t frame_id)
{
	int row;
	int column;

	for (row = 0; row < surface->height; ++row) {
		for (column = 0; column < surface->width * 2; ++column) {
			surface->ptr[(size_t)row * (size_t)surface->yalign +
				(size_t)column] = np2present_pattern_byte(frame_id,
				(size_t)row * (size_t)surface->width * 2U +
				(size_t)column);
		}
	}
}

static void np2present_report_memory(const char *phase,
	uint32_t guest_bytes, int guest_external)
{
	printf("NP2PRESENT_MEMORY phase=%s psram_total=%lu free_spiram=%lu "
		"largest_spiram=%lu guest_bytes=%lu presentation_bytes=%lu "
		"guest_external=%d slot0_external=%d slot1_external=%d\n",
		phase,
		(unsigned long)esp_psram_get_size(),
		(unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
		(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
		(unsigned long)guest_bytes,
		(unsigned long)(NP2PRESENT_SLOT_BYTES * NP2PRESENT_SLOT_COUNT),
		guest_external,
		s_np2present.slot0_allocated &&
			s_np2present.slots[0].ptr != NULL &&
			esp_ptr_external_ram(s_np2present.slots[0].ptr),
		s_np2present.slot1_allocated &&
			s_np2present.slots[1].ptr != NULL &&
			esp_ptr_external_ram(s_np2present.slots[1].ptr));
}

static bool np2present_validate_view(
	const np2_presentation_frame_view *view, uintptr_t guest_address)
{
	if ((view->ptr == NULL) || !esp_ptr_external_ram(view->ptr) ||
			((uintptr_t)view->ptr == guest_address) ||
			(view->width != s_np2present.expected_width) ||
			(view->height != s_np2present.expected_height) ||
			(view->pitch != (size_t)view->width * 2U) ||
			(view->bpp != 16U) ||
			(view->pixel_format != NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE) ||
			(view->source_generation != s_np2present.expected_generation) ||
			(view->source_update_sequence !=
				s_np2present.expected_update_sequence) ||
			(view->published_sequence !=
				s_np2present.expected_published_sequence)) {
		return false;
	}
	s_np2present.last_checked_hash = np2present_view_hash(view);
	return s_np2present.last_checked_hash == np2present_frame_hash(
		s_np2present.expected_frame_id, view->width, view->height);
}

static void np2present_publish_hook(const SCRNMNG_PUBLISH_VIEW *view,
	void *context)
{
	np2present_state *state = (np2present_state *)context;
	np2_presentation_source_view source;

	/* This callback intentionally has no allocation, logging, waiting, or
	 * reentrant scrnmng call.  The source pointer is borrowed for this call. */
	source.ptr = view->ptr;
	source.width = (uint32_t)view->width;
	source.height = (uint32_t)view->height;
	source.pitch = view->pitch;
	source.bpp = view->bpp;
	source.pixel_format =
		(np2_presentation_pixel_format)view->pixel_format;
	source.source_generation = view->surface_generation;
	source.source_update_sequence = view->surface_update_sequence;
	state->last_submit_status = np2_presentation_submit(
		&state->publisher, &source);
}

static void np2present_consumer_set_failure(const char *reason)
{
	s_np2present.command_ok = false;
	s_np2present.command_reason = reason;
}

static void np2present_consumer_check_no_frame(void)
{
	np2_presentation_frame_view view;
	np2_presentation_token token;
	np2_presentation_status status;

	status = np2_presentation_acquire(&s_np2present.publisher, &view, &token);
	if (status == NP2_PRESENTATION_OK) {
		(void)np2_presentation_release(&s_np2present.publisher, &token);
		np2present_consumer_set_failure("frame_visible_before_unlock");
		return;
	}
	if (status != NP2_PRESENTATION_NO_FRAME) {
		np2present_consumer_set_failure("unexpected_preunlock_status");
		return;
	}
	s_np2present.command_ok = true;
}

static void np2present_consumer_acquire(void)
{
	np2_presentation_status status;

	status = np2_presentation_acquire(&s_np2present.publisher,
		&s_np2present.held_view, &s_np2present.held_token);
	if (status != NP2_PRESENTATION_OK) {
		np2present_consumer_set_failure("acquire_failed");
		return;
	}
	s_np2present.consumer_holds_frame = true;
	if (!np2present_validate_view(&s_np2present.held_view,
			s_np2present.expected_guest_address)) {
		np2present_consumer_set_failure("frame_metadata_or_payload_mismatch");
		return;
	}
	s_np2present.command_ok = true;
}

static void np2present_consumer_check_held(void)
{
	if (!s_np2present.consumer_holds_frame) {
		np2present_consumer_set_failure("no_acquired_frame");
		return;
	}
	s_np2present.last_checked_hash = np2present_view_hash(
		&s_np2present.held_view);
	if (s_np2present.last_checked_hash != s_np2present.last_old_hash) {
		np2present_consumer_set_failure("acquired_frame_changed");
		return;
	}
	s_np2present.command_ok = true;
}

static void np2present_consumer_release_acquire(void)
{
	np2_presentation_status status;

	if (!s_np2present.consumer_holds_frame) {
		np2present_consumer_set_failure("release_without_acquired_frame");
		return;
	}
	status = np2_presentation_release(&s_np2present.publisher,
		&s_np2present.held_token);
	if (status != NP2_PRESENTATION_OK) {
		np2present_consumer_set_failure("release_failed");
		return;
	}
	s_np2present.consumer_holds_frame = false;
	np2present_consumer_acquire();
}

static void np2present_consumer_stop(void)
{
	if (s_np2present.consumer_holds_frame) {
		if (np2_presentation_release(&s_np2present.publisher,
				&s_np2present.held_token) != NP2_PRESENTATION_OK) {
			s_np2present.command_reason = "cleanup_release_failed";
			s_np2present.command_ok = false;
			return;
		}
		s_np2present.consumer_holds_frame = false;
	}
	s_np2present.command_ok = true;
	s_np2present.consumer_alive = false;
}

static void np2present_consumer_task(void *argument)
{
	(void)argument;
	s_np2present.consumer_alive = true;
	xTaskNotifyGive(s_np2present.producer_task);
	for (;;) {
		if (ulTaskNotifyTake(pdTRUE,
				pdMS_TO_TICKS(NP2PRESENT_TIMEOUT_MS)) == 0U) {
			s_np2present.command_ok = false;
			s_np2present.command_reason = "consumer_command_timeout";
			s_np2present.consumer_alive = false;
			s_np2present.consumer_task = NULL;
			xTaskNotifyGive(s_np2present.producer_task);
			vTaskDelete(NULL);
			return;
		}
		s_np2present.command_ok = false;
		s_np2present.command_reason = "unknown_command";
		switch (s_np2present.command) {
		case NP2PRESENT_COMMAND_CHECK_NO_FRAME:
			np2present_consumer_check_no_frame();
			break;
		case NP2PRESENT_COMMAND_ACQUIRE:
			np2present_consumer_acquire();
			break;
		case NP2PRESENT_COMMAND_CHECK_HELD:
			np2present_consumer_check_held();
			break;
		case NP2PRESENT_COMMAND_RELEASE_ACQUIRE:
			np2present_consumer_release_acquire();
			break;
		case NP2PRESENT_COMMAND_STOP:
			np2present_consumer_stop();
			s_np2present.consumer_task = NULL;
			xTaskNotifyGive(s_np2present.producer_task);
			vTaskDelete(NULL);
			return;
		default:
			break;
		}
		xTaskNotifyGive(s_np2present.producer_task);
	}
}

static bool np2present_send_command(np2present_command command)
{
	if (!s_np2present.consumer_alive) {
		return false;
	}
	s_np2present.command = command;
	s_np2present.command_ok = false;
	s_np2present.command_reason = "command_not_completed";
	xTaskNotifyGive(s_np2present.consumer_task);
	if (ulTaskNotifyTake(pdTRUE,
			pdMS_TO_TICKS(NP2PRESENT_TIMEOUT_MS)) == 0U) {
		return false;
	}
	return s_np2present.command_ok;
}

static bool np2present_publish_guest_frame(uint8_t frame_id)
{
	const SCRNSURF *surface = scrnmng_surflock();

	if (surface == NULL) {
		return false;
	}
	s_np2present.expected_guest_address = (uintptr_t)surface->ptr;
	np2present_write_guest_frame(surface, frame_id);
	scrnmng_surfunlock(surface);
	return s_np2present.last_submit_status == NP2_PRESENTATION_OK;
}

static esp_err_t np2present_fail(const char *reason)
{
	printf("NP2PRESENT_RESULT=FAIL reason=%s\n", reason);
	fflush(stdout);
	return ESP_FAIL;
}

static void np2present_cleanup(bool stop_consumer)
{
	bool consumer_stopped = !s_np2present.consumer_alive;

	if (stop_consumer && s_np2present.consumer_alive) {
		consumer_stopped = np2present_send_command(NP2PRESENT_COMMAND_STOP);
	}
	if (s_np2present.hook_registered) {
		scrnmng_set_publish_hook(NULL, NULL);
		s_np2present.hook_registered = false;
	}
	if (s_np2present.guest_initialized) {
		scrnmng_shutdown();
		s_np2present.guest_initialized = false;
	}
	/* If a consumer could not be stopped, leave storage allocated rather than
	 * risk a use-after-free.  The bounded harness process will terminate. */
	if (consumer_stopped) {
		if (s_np2present.slot0_allocated) {
			heap_caps_free(s_np2present.slots[0].ptr);
			s_np2present.slot0_allocated = false;
		}
		if (s_np2present.slot1_allocated) {
			heap_caps_free(s_np2present.slots[1].ptr);
			s_np2present.slot1_allocated = false;
		}
	}
}

esp_err_t np2presentation_probe_run(void)
{
	SCRNMNG_STATUS status;
	SCRNMNG_SURFACE_VIEW surface_view;
	uint32_t generation;
	uint32_t sequence;
	int slot0_lock_free;
	int slot1_lock_free;
	bool success = false;
	const char *failure = "unknown";

	s_np2present.producer_task = xTaskGetCurrentTaskHandle();
	s_np2present.last_submit_status = NP2_PRESENTATION_INVALID_ARGUMENT;
	if (!esp_psram_is_initialized()) {
		failure = "psram_not_initialized";
		goto cleanup;
	}
	np2present_report_memory("before_guest", 0U, 0);
	scrnmng_shutdown();
	if (!scrnmng_initialize()) {
		failure = "scrnmng_initialize";
		goto cleanup;
	}
	s_np2present.guest_initialized = true;
	scrnmng_getstatus(&status);
	if (!status.initialized || status.failed || !status.external ||
			status.width != 640 || status.height != 400 ||
			status.bytes != NP2PRESENT_SLOT_BYTES ||
			scrnmng_get_surface_view(&surface_view) != SCRNMNG_SNAPSHOT_OK ||
			surface_view.ptr == NULL ||
			!esp_ptr_external_ram(surface_view.ptr) ||
			surface_view.bpp != 16U || surface_view.pitch != 1280U) {
		failure = "guest_surface_contract";
		goto cleanup;
	}
	s_np2present.guest_bytes = (uint32_t)status.bytes;
	np2present_report_memory("after_guest", s_np2present.guest_bytes, 1);
	for (unsigned index = 0U; index < NP2PRESENT_SLOT_COUNT; ++index) {
		s_np2present.slots[index].ptr = heap_caps_malloc(
			NP2PRESENT_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if ((s_np2present.slots[index].ptr == NULL) ||
				!esp_ptr_external_ram(s_np2present.slots[index].ptr)) {
			failure = index == 0U ? "slot0_allocation" : "slot1_allocation";
			goto cleanup;
		}
		if (index == 0U) {
			s_np2present.slot0_allocated = true;
		} else {
			s_np2present.slot1_allocated = true;
		}
		s_np2present.slots[index].capacity = NP2PRESENT_SLOT_BYTES;
	}
	np2present_report_memory("after_slots", s_np2present.guest_bytes, 1);
	if (np2_presentation_init(&s_np2present.publisher,
			s_np2present.slots) != NP2_PRESENTATION_OK) {
		failure = "publisher_init";
		goto cleanup;
	}
	s_np2present.publisher_initialized = true;
	slot0_lock_free = atomic_is_lock_free(
		&s_np2present.publisher.slot_state[0]) ? 1 : 0;
	slot1_lock_free = atomic_is_lock_free(
		&s_np2present.publisher.slot_state[1]) ? 1 : 0;
	printf("NP2PRESENT_INIT guest_external=1 slot0_external=1 "
		"slot1_external=1 slots=2 slot_bytes=%lu guest_width=640 "
		"guest_height=400 guest_pitch=1280 guest_bpp=16 "
		"atomic32_lock_free=%d slot_state0_lock_free=%d "
		"slot_state1_lock_free=%d\n",
		(unsigned long)NP2PRESENT_SLOT_BYTES,
		slot0_lock_free && slot1_lock_free, slot0_lock_free,
		slot1_lock_free);
	if (!slot0_lock_free || !slot1_lock_free) {
		failure = "atomic32_not_lock_free";
		goto cleanup;
	}
	scrnmng_set_publish_hook(np2present_publish_hook, &s_np2present);
	s_np2present.hook_registered = true;
	s_np2present.consumer_task = xTaskCreateStatic(
		np2present_consumer_task, "np2present", NP2PRESENT_CONSUMER_STACK_WORDS,
		NULL, tskIDLE_PRIORITY + 1U, s_consumer_stack,
		&s_consumer_task_buffer);
	if (s_np2present.consumer_task == NULL ||
			ulTaskNotifyTake(pdTRUE,
				pdMS_TO_TICKS(NP2PRESENT_TIMEOUT_MS)) == 0U) {
		if ((s_np2present.consumer_task != NULL) &&
				!s_np2present.consumer_alive) {
			vTaskDelete(s_np2present.consumer_task);
			s_np2present.consumer_task = NULL;
		}
		failure = "consumer_start";
		goto cleanup;
	}

	/* Basic publication: the consumer checks the empty publisher while the
	 * producer still owns the guest lock. */
	{
		const SCRNSURF *surface = scrnmng_surflock();
		if (surface == NULL) {
			failure = "basic_guest_lock";
			goto cleanup;
		}
		s_np2present.expected_guest_address = (uintptr_t)surface->ptr;
		np2present_write_guest_frame(surface, 1U);
		if (!np2present_send_command(NP2PRESENT_COMMAND_CHECK_NO_FRAME)) {
			scrnmng_surfunlock(surface);
			failure = "preunlock_acquire";
			goto cleanup;
		}
		scrnmng_surfunlock(surface);
	}
	if (s_np2present.last_submit_status != NP2_PRESENTATION_OK) {
		failure = "basic_submit";
		goto cleanup;
	}
	s_np2present.expected_frame_id = 1U;
	s_np2present.expected_width = 640U;
	s_np2present.expected_height = 400U;
	s_np2present.expected_generation = 1U;
	s_np2present.expected_update_sequence = 1U;
	s_np2present.expected_published_sequence = 1U;
	if (!np2present_send_command(NP2PRESENT_COMMAND_ACQUIRE)) {
		failure = s_np2present.command_reason != NULL ?
			s_np2present.command_reason : "basic_acquire";
		goto cleanup;
	}
	printf("NP2PRESENT_BASIC result=PASS frame_id=1 guest_external=1 "
		"presentation_external=1 ptr_distinct=1 width=640 height=400 "
		"pitch=1280 bpp=16 published_sequence=1 hash=0x%08lx\n",
		(unsigned long)s_np2present.last_checked_hash);
	s_np2present.last_old_hash = s_np2present.last_checked_hash;

	/* Modify the guest while frame 1 is acquired.  The consumer hashes the
	 * complete owned 512000-byte frame before the guest unlocks. */
	{
		const SCRNSURF *surface = scrnmng_surflock();
		if (surface == NULL) {
			failure = "immutable_guest_lock";
			goto cleanup;
		}
		np2present_write_guest_frame(surface, 2U);
		if (surface->ptr[0] != np2present_pattern_byte(2U, 0U) ||
				!np2present_send_command(NP2PRESENT_COMMAND_CHECK_HELD)) {
			scrnmng_surfunlock(surface);
			failure = "immutable_check";
			goto cleanup;
		}
		scrnmng_surfunlock(surface);
	}
	if (s_np2present.last_submit_status != NP2_PRESENTATION_OK) {
		failure = "immutable_submit";
		goto cleanup;
	}
	printf("NP2PRESENT_IMMUTABLE result=PASS frame_id=1 guest_locked=1 "
		"full_hash=0x%08lx unchanged=1\n",
		(unsigned long)s_np2present.last_checked_hash);

	if (!np2present_publish_guest_frame(3U) ||
			!np2present_publish_guest_frame(4U)) {
		failure = "coalesce_submit";
		goto cleanup;
	}
	s_np2present.expected_frame_id = 4U;
	if (scrnmng_get_surface_view(&surface_view) != SCRNMNG_SNAPSHOT_OK) {
		failure = "coalesce_surface_query";
		goto cleanup;
	}
	s_np2present.expected_guest_address = (uintptr_t)surface_view.ptr;
	s_np2present.expected_width = 640U;
	s_np2present.expected_height = 400U;
	s_np2present.expected_generation = 1U;
	s_np2present.expected_update_sequence = 4U;
	s_np2present.expected_published_sequence = 4U;
	if (!np2present_send_command(NP2PRESENT_COMMAND_RELEASE_ACQUIRE) ||
		np2_presentation_coalesced_count(&s_np2present.publisher) != 2U ||
		np2_presentation_dropped_count(&s_np2present.publisher) != 0U) {
		failure = "coalesce_result";
		goto cleanup;
	}
	printf("NP2PRESENT_COALESCE result=PASS acquired_frame=4 "
		"published_sequence=4 coalesced_count=%lu dropped_count=%lu "
		"hash=0x%08lx\n",
		(unsigned long)np2_presentation_coalesced_count(
			&s_np2present.publisher),
		(unsigned long)np2_presentation_dropped_count(
			&s_np2present.publisher),
		(unsigned long)s_np2present.last_checked_hash);

	/* Request both dimensions while the old guest surface is locked.  The
	 * existing headless backend then performs one pending resize after its
	 * synchronous publish callback. */
	{
		const SCRNSURF *surface = scrnmng_surflock();
		if (surface == NULL) {
			failure = "resize_guest_lock";
			goto cleanup;
		}
		scrnmng_setwidth(0, 320);
		scrnmng_setheight(0, 200);
		scrnmng_surfunlock(surface);
	}
	if (s_np2present.last_submit_status != NP2_PRESENTATION_OK ||
			scrnmng_get_surface_view(&surface_view) != SCRNMNG_SNAPSHOT_OK) {
		failure = "resize_transition";
		goto cleanup;
	}
	scrnmng_get_surface_counters(&generation, &sequence);
	if (generation != 2U || surface_view.width != 320 ||
			surface_view.height != 200 || surface_view.pitch != 640U) {
		failure = "resize_generation";
		goto cleanup;
	}
	s_np2present.last_old_hash = np2present_frame_hash(4U, 640U, 400U);
	if (!np2present_send_command(NP2PRESENT_COMMAND_CHECK_HELD)) {
		failure = "resize_old_frame";
		goto cleanup;
	}
	if (!np2present_publish_guest_frame(5U)) {
		failure = "resize_submit";
		goto cleanup;
	}
	s_np2present.expected_frame_id = 5U;
	s_np2present.expected_guest_address = (uintptr_t)surface_view.ptr;
	s_np2present.expected_width = 320U;
	s_np2present.expected_height = 200U;
	s_np2present.expected_generation = 2U;
	s_np2present.expected_update_sequence = sequence + 1U;
	s_np2present.expected_published_sequence = 6U;
	if (!np2present_send_command(NP2PRESENT_COMMAND_RELEASE_ACQUIRE) ||
		s_np2present.held_view.width != 320U ||
		s_np2present.held_view.height != 200U ||
		s_np2present.held_view.pitch != 640U ||
		s_np2present.held_view.source_generation != generation) {
		failure = "resize_new_frame";
		goto cleanup;
	}
	np2present_report_memory("after_resize", 320U * 200U * 2U, 1);
	printf("NP2PRESENT_RESIZE result=PASS old_generation=1 "
		"new_generation=%lu old_frame_unchanged=1 width=320 height=200 "
		"pitch=640 source_generation=%lu source_update_sequence=%lu "
		"published_sequence=%lu hash=0x%08lx\n",
		(unsigned long)generation,
		(unsigned long)s_np2present.held_view.source_generation,
		(unsigned long)s_np2present.held_view.source_update_sequence,
		(unsigned long)s_np2present.held_view.published_sequence,
		(unsigned long)s_np2present.last_checked_hash);

	success = true;

cleanup:
	np2present_cleanup(s_np2present.consumer_alive);
	if (!success) {
		return np2present_fail(failure);
	}
	printf("NP2PRESENT_RESULT=PASS\n");
	fflush(stdout);
	return ESP_OK;
}
