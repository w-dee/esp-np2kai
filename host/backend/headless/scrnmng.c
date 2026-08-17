#include <compiler.h>
#include <scrnmng.h>
#include <scrnmng_storage.h>
#include <vram/scrndraw.h>

#include <stdint.h>

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
	if (!scrnmng_initialized || scrnmng_failed || scrnmng_surface.ptr == NULL) {
		return NULL;
	}
	scrnmng_locked = 1;
	return &scrnmng_surface;
}

void scrnmng_surfunlock(const SCRNSURF *surf)
{
	if (surf != &scrnmng_surface) {
		return;
	}
	scrnmng_locked = 0;
	if (scrnmng_resize_pending && !scrnmng_failed) {
		(void)scrnmng_resize();
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
