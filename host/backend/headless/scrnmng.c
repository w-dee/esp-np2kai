#include <compiler.h>
#include <np2_crc32.h>
#include <scrnmng.h>
#include <scrnmng_storage.h>
#include <vram/scrndraw.h>

#include <stdint.h>
#include <string.h>

static SCRNSURF scrnmng_surface;
static int scrnmng_requested_width = 640;
static int scrnmng_requested_height = 400;
static size_t scrnmng_bytes;
static int scrnmng_initialized;
static int scrnmng_failed;
static int scrnmng_locked;
static int scrnmng_resize_pending;
static int scrnmng_failure_width;
static int scrnmng_failure_height;
static size_t scrnmng_failure_bytes;
static uint32_t scrnmng_surface_generation;
static uint32_t scrnmng_surface_update_sequence;
static SCRNMNG_PUBLISH_HOOK scrnmng_publish_hook;
static void *scrnmng_publish_context;

static BOOL scrnmng_calculate_size(int width, int height, size_t *bytes)
{
	size_t pitch;

	if ((width <= 0) || (height <= 0) ||
			(width > SURFACE_WIDTH) || (height > SURFACE_HEIGHT)) {
		return FALSE;
	}
	if ((size_t)width > SIZE_MAX / sizeof(UINT16)) {
		return FALSE;
	}
	pitch = (size_t)width * sizeof(UINT16);
	if ((size_t)height > SIZE_MAX / pitch) {
		return FALSE;
	}
	*bytes = pitch * (size_t)height;
	return TRUE;
}

static void scrnmng_record_failure(int width, int height, size_t bytes)
{
	scrnmng_failed = 1;
	scrnmng_failure_width = width;
	scrnmng_failure_height = height;
	scrnmng_failure_bytes = bytes;
}

static BOOL scrnmng_resize(void)
{
	SCRNSURF next;
	size_t bytes;

	if (!scrnmng_calculate_size(scrnmng_requested_width,
			scrnmng_requested_height, &bytes)) {
		scrnmng_record_failure(scrnmng_requested_width,
			scrnmng_requested_height, 0);
		return FALSE;
	}
	if (scrnmng_initialized &&
			(scrnmng_surface.width == scrnmng_requested_width) &&
			(scrnmng_surface.height == scrnmng_requested_height)) {
		scrnmng_resize_pending = 0;
		return TRUE;
	}

	next.ptr = scrnmng_storage_alloc(bytes);
	if (next.ptr == NULL) {
		scrnmng_record_failure(scrnmng_requested_width,
				scrnmng_requested_height, bytes);
		return FALSE;
	}
	next.xalign = (int)sizeof(UINT16);
	next.yalign = scrnmng_requested_width * (int)sizeof(UINT16);
	next.width = scrnmng_requested_width;
	next.height = scrnmng_requested_height;
	next.bpp = 16;
	next.extend = 0;

	if (scrnmng_initialized) {
		scrnmng_storage_free(scrnmng_surface.ptr);
	}
	scrnmng_surface = next;
	scrnmng_bytes = bytes;
	scrnmng_initialized = 1;
	scrnmng_resize_pending = 0;
	++scrnmng_surface_generation;
	return TRUE;
}

BOOL scrnmng_initialize(void)
{
	if (scrnmng_failed) {
		return FALSE;
	}
	if (scrnmng_initialized) {
		return TRUE;
	}
	return scrnmng_resize();
}

void scrnmng_shutdown(void)
{
	if (scrnmng_initialized) {
		scrnmng_storage_free(scrnmng_surface.ptr);
	}
	scrnmng_surface.ptr = NULL;
	scrnmng_surface.xalign = 0;
	scrnmng_surface.yalign = 0;
	scrnmng_surface.width = 0;
	scrnmng_surface.height = 0;
	scrnmng_surface.bpp = 0;
	scrnmng_surface.extend = 0;
	scrnmng_bytes = 0;
	scrnmng_initialized = 0;
	scrnmng_failed = 0;
	scrnmng_locked = 0;
	scrnmng_resize_pending = 0;
	scrnmng_requested_width = 640;
	scrnmng_requested_height = 400;
	scrnmng_failure_width = 0;
	scrnmng_failure_height = 0;
	scrnmng_failure_bytes = 0;
	scrnmng_surface_generation = 0;
	scrnmng_surface_update_sequence = 0;
	scrnmng_publish_hook = NULL;
	scrnmng_publish_context = NULL;
}

BOOL scrnmng_haserror(void)
{
	return scrnmng_failed ? TRUE : FALSE;
}

void scrnmng_getstatus(SCRNMNG_STATUS *status)
{
	if (status == NULL) {
		return;
	}
	status->width = scrnmng_surface.width;
	status->height = scrnmng_surface.height;
	status->requested_width = scrnmng_failed ? scrnmng_failure_width :
			scrnmng_requested_width;
	status->requested_height = scrnmng_failed ? scrnmng_failure_height :
		scrnmng_requested_height;
	status->bytes = scrnmng_failed ? scrnmng_failure_bytes : scrnmng_bytes;
	status->initialized = scrnmng_initialized ? TRUE : FALSE;
	status->failed = scrnmng_failed ? TRUE : FALSE;
	status->external = (scrnmng_initialized && scrnmng_surface.ptr != NULL &&
			scrnmng_storage_is_external(scrnmng_surface.ptr)) ? TRUE : FALSE;
}

const SCRNSURF *scrnmng_surflock(void)
{
	if (!scrnmng_initialized || scrnmng_failed || scrnmng_locked ||
			scrnmng_surface.ptr == NULL) {
		return NULL;
	}
	scrnmng_locked = 1;
	return &scrnmng_surface;
}

void scrnmng_surfunlock(const SCRNSURF *surf)
{
	SCRNMNG_PUBLISH_VIEW publish_view;

	if (!scrnmng_locked || surf != &scrnmng_surface) {
		return;
	}
	if (scrnmng_publish_hook != NULL) {
		publish_view.ptr = scrnmng_surface.ptr;
		publish_view.width = scrnmng_surface.width;
		publish_view.height = scrnmng_surface.height;
		publish_view.bpp = scrnmng_surface.bpp;
		publish_view.pixel_format = SCRNMNG_PIXEL_FORMAT_RGB565LE;
		publish_view.pitch = (size_t)scrnmng_surface.yalign;
		publish_view.surface_generation = scrnmng_surface_generation;
		/* The hook receives the logical completed value before the stored
		 * Step 7A counter is incremented. */
		publish_view.surface_update_sequence =
			scrnmng_surface_update_sequence + 1U;
		scrnmng_publish_hook(&publish_view, scrnmng_publish_context);
	}
	scrnmng_locked = 0;
	++scrnmng_surface_update_sequence;
	if (scrnmng_resize_pending && !scrnmng_failed) {
		(void)scrnmng_resize();
	}
}

void scrnmng_set_publish_hook(SCRNMNG_PUBLISH_HOOK hook, void *context)
{
	scrnmng_publish_hook = hook;
	scrnmng_publish_context = (hook != NULL) ? context : NULL;
}

const char *scrnmng_snapshot_status_name(SCRNMNG_SNAPSHOT_STATUS status)
{
	switch (status) {
	case SCRNMNG_SNAPSHOT_OK:
		return "ok";
	case SCRNMNG_SNAPSHOT_INVALID_ARGUMENT:
		return "invalid_argument";
	case SCRNMNG_SNAPSHOT_NOT_INITIALIZED:
		return "not_initialized";
	case SCRNMNG_SNAPSHOT_FAILED:
		return "failed";
	case SCRNMNG_SNAPSHOT_BUSY:
		return "busy";
	case SCRNMNG_SNAPSHOT_UNSUPPORTED:
		return "unsupported";
	default:
		return "invalid";
	}
}

SCRNMNG_SNAPSHOT_STATUS scrnmng_get_surface_view(SCRNMNG_SURFACE_VIEW *view)
{
	if (view == NULL) {
		return SCRNMNG_SNAPSHOT_INVALID_ARGUMENT;
	}
	if (scrnmng_failed) {
		return SCRNMNG_SNAPSHOT_FAILED;
	}
	if (!scrnmng_initialized) {
		return SCRNMNG_SNAPSHOT_NOT_INITIALIZED;
	}
	if (scrnmng_locked) {
		return SCRNMNG_SNAPSHOT_BUSY;
	}
	if ((scrnmng_surface.ptr == NULL) || (scrnmng_surface.bpp != 16)) {
		return SCRNMNG_SNAPSHOT_UNSUPPORTED;
	}
	view->ptr = scrnmng_surface.ptr;
	view->width = scrnmng_surface.width;
	view->height = scrnmng_surface.height;
	view->bpp = scrnmng_surface.bpp;
	view->pixel_format = SCRNMNG_PIXEL_FORMAT_RGB565LE;
	view->pitch = (size_t)scrnmng_surface.yalign;
	return SCRNMNG_SNAPSHOT_OK;
}

SCRNMNG_SNAPSHOT_STATUS scrnmng_snapshot(SCRNMNG_SNAPSHOT *snapshot)
{
	SCRNMNG_SURFACE_VIEW view;
	SCRNMNG_SNAPSHOT_STATUS status;
	size_t row_bytes;
	size_t visible_bytes;
	size_t row;
	uint32_t crc;

	if (snapshot == NULL) {
		return SCRNMNG_SNAPSHOT_INVALID_ARGUMENT;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	status = scrnmng_get_surface_view(&view);
	if (status != SCRNMNG_SNAPSHOT_OK) {
		return status;
	}
	if ((size_t)view.width > SIZE_MAX / sizeof(UINT16)) {
		return SCRNMNG_SNAPSHOT_UNSUPPORTED;
	}
	row_bytes = (size_t)view.width * sizeof(UINT16);
	if ((size_t)view.height > SIZE_MAX / row_bytes) {
		return SCRNMNG_SNAPSHOT_UNSUPPORTED;
	}
	visible_bytes = row_bytes * (size_t)view.height;
	if (view.pitch < row_bytes) {
		return SCRNMNG_SNAPSHOT_UNSUPPORTED;
	}
	crc = np2_crc32_iso_hdlc_init();
	for (row = 0; row < (size_t)view.height; ++row) {
		crc = np2_crc32_iso_hdlc_update(crc,
				view.ptr + row * view.pitch, row_bytes);
	}
	snapshot->width = view.width;
	snapshot->height = view.height;
	snapshot->bpp = view.bpp;
	snapshot->pixel_format = view.pixel_format;
	snapshot->pitch = view.pitch;
	snapshot->visible_bytes = visible_bytes;
	snapshot->surface_generation = scrnmng_surface_generation;
	snapshot->surface_update_sequence = scrnmng_surface_update_sequence;
	snapshot->crc_algorithm = NP2_CRC32_ISO_HDLC_ALGORITHM;
	snapshot->crc_version = NP2_CRC32_ISO_HDLC_VERSION;
	snapshot->crc32 = np2_crc32_iso_hdlc_finish(crc);
	return SCRNMNG_SNAPSHOT_OK;
}

void scrnmng_get_surface_counters(uint32_t *generation, uint32_t *sequence)
{
	if (generation != NULL) {
		*generation = scrnmng_surface_generation;
	}
	if (sequence != NULL) {
		*sequence = scrnmng_surface_update_sequence;
	}
}

void scrnmng_setwidth(int posx, int width)
{
	(void)posx;
	if ((width <= 0) || (width > SURFACE_WIDTH)) {
		scrnmng_record_failure(width, scrnmng_requested_height, 0);
		return;
	}
	scrnmng_requested_width = width;
	if (scrnmng_initialized) {
		if (scrnmng_locked) {
			scrnmng_resize_pending = 1;
		}
		else if (!scrnmng_failed) {
			(void)scrnmng_resize();
		}
	}
}

void scrnmng_setheight(int posy, int height)
{
	(void)posy;
	if ((height <= 0) || (height > SURFACE_HEIGHT)) {
		scrnmng_record_failure(scrnmng_requested_width, height, 0);
		return;
	}
	scrnmng_requested_height = height;
	if (scrnmng_initialized) {
		if (scrnmng_locked) {
			scrnmng_resize_pending = 1;
		}
		else if (!scrnmng_failed) {
			(void)scrnmng_resize();
		}
	}
}

RGB16 scrnmng_makepal16(RGB32 pal32)
{
	return (RGB16)((((UINT16)pal32.p.r >> 3) << 11) |
			(((UINT16)pal32.p.g >> 2) << 5) |
			((UINT16)pal32.p.b >> 3));
}
