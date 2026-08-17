#include <compiler.h>
#include <scrnmng.h>

#include <stddef.h>

static RGB32 make_color(UINT8 red, UINT8 green, UINT8 blue)
{
	RGB32 color;

	color.d = RGB32D(red, green, blue);
	return color;
}

static int surface_is_zero(const SCRNSURF *surface)
{
	size_t bytes;
	size_t index;

	bytes = (size_t)surface->yalign * (size_t)surface->height;
	for (index = 0; index < bytes; ++index) {
		if (surface->ptr[index] != 0) {
			return 0;
		}
	}
	return 1;
}

static int check_surface(int width, int height, size_t bytes)
{
	const SCRNSURF *surface;
	SCRNMNG_STATUS status;

	surface = scrnmng_surflock();
	if (surface == NULL || surface->ptr == NULL ||
			surface->width != width || surface->height != height ||
			surface->xalign != 2 || surface->yalign != width * 2 ||
			surface->bpp != 16 || surface->extend != 0 ||
			!surface_is_zero(surface)) {
		if (surface != NULL) {
			scrnmng_surfunlock(surface);
		}
		return 0;
	}
	scrnmng_surfunlock(surface);
	scrnmng_getstatus(&status);
	return status.initialized && !status.failed &&
			status.width == width && status.height == height &&
			status.bytes == bytes;
}

static int resize_to(int width, int height)
{
	scrnmng_setwidth(0, width);
	scrnmng_setheight(0, height);
	return !scrnmng_haserror() &&
		check_surface(width, height, (size_t)width * (size_t)height * 2);
}

int main(void)
{
	const SCRNSURF *surface;
	const SCRNSURF *locked_surface;
	SCRNSURF unrelated_surface;
	SCRNMNG_STATUS status;
	UINT8 preserved_value;

	scrnmng_shutdown();
	if (!scrnmng_initialize() ||
			!check_surface(640, 400, 640U * 400U * 2U)) {
		return 1;
	}

	surface = scrnmng_surflock();
	if (surface == NULL) {
		return 1;
	}
	preserved_value = surface->ptr[0];
	surface->ptr[0] = 0x5a;
	locked_surface = scrnmng_surflock();
	if (locked_surface != NULL || scrnmng_haserror()) {
		return 1;
	}
	scrnmng_surfunlock(NULL);
	unrelated_surface = *surface;
	scrnmng_surfunlock(&unrelated_surface);
	if (scrnmng_surflock() != NULL || scrnmng_haserror()) {
		return 1;
	}
	scrnmng_surfunlock(surface);

	locked_surface = scrnmng_surflock();
	if (locked_surface == NULL || locked_surface->ptr[0] != 0x5a ||
			preserved_value != 0) {
		scrnmng_surfunlock(locked_surface);
		return 1;
	}
	scrnmng_surfunlock(locked_surface);

	locked_surface = scrnmng_surflock();
	if (locked_surface == NULL) {
		return 1;
	}
	scrnmng_setwidth(0, 800);
	scrnmng_getstatus(&status);
	if (status.width != 640 || status.requested_width != 800 ||
			status.failed) {
		scrnmng_surfunlock(locked_surface);
		return 1;
	}
	scrnmng_surfunlock(locked_surface);
	if (!resize_to(800, 400) || !resize_to(640, 200) ||
			!resize_to(640, 400) || !resize_to(640, 480) ||
			!resize_to(1024, 800)) {
		return 1;
	}

	if (scrnmng_makepal16(make_color(0x00, 0x00, 0x00)) != 0x0000 ||
			scrnmng_makepal16(make_color(0xff, 0xff, 0xff)) != 0xffff ||
			scrnmng_makepal16(make_color(0xff, 0x00, 0x00)) != 0xf800 ||
			scrnmng_makepal16(make_color(0x00, 0xff, 0x00)) != 0x07e0 ||
			scrnmng_makepal16(make_color(0x00, 0x00, 0xff)) != 0x001f ||
			scrnmng_makepal16(make_color(0x12, 0x34, 0x56)) != 0x11aa) {
		return 1;
	}

	scrnmng_setwidth(0, 0);
	if (!scrnmng_haserror()) {
		return 1;
	}
	scrnmng_getstatus(&status);
	if (status.width != 1024 || status.height != 800 ||
			status.requested_width != 0 || status.bytes != 0) {
		return 1;
	}

	scrnmng_shutdown();
	if (!scrnmng_initialize() ||
			!check_surface(640, 400, 640U * 400U * 2U)) {
		return 1;
	}
	scrnmng_shutdown();
	return 0;
}
