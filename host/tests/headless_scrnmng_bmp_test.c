#define _XOPEN_SOURCE 700

#include <compiler.h>
#include <scrnmng.h>
#include <scrnmng_bmp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t read_u16le(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32le(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
			((uint32_t)bytes[1] << 8) |
			((uint32_t)bytes[2] << 16) |
			((uint32_t)bytes[3] << 24);
}

int main(void)
{
	char path[] = "/tmp/np2kai-scrnmng-bmp-XXXXXX";
	const SCRNSURF *surface;
	FILE *file;
	uint8_t bytes[70];
	int descriptor;

	scrnmng_shutdown();
	if (scrnmng_write_bmp(path) != SCRNMNG_BMP_NOT_INITIALIZED ||
			!scrnmng_initialize()) {
		return 1;
	}
	scrnmng_setwidth(0, 2);
	scrnmng_setheight(0, 2);
	surface = scrnmng_surflock();
	if (surface == NULL) {
		return 1;
	}
	/* Row 0: red, green. Row 1: blue, white. */
	surface->ptr[0] = 0x00;
	surface->ptr[1] = 0xf8;
	surface->ptr[2] = 0xe0;
	surface->ptr[3] = 0x07;
	surface->ptr[surface->yalign + 0] = 0x1f;
	surface->ptr[surface->yalign + 1] = 0x00;
	surface->ptr[surface->yalign + 2] = 0xff;
	surface->ptr[surface->yalign + 3] = 0xff;
	scrnmng_surfunlock(surface);

	descriptor = mkstemp(path);
	if (descriptor < 0) {
		return 1;
	}
	close(descriptor);
	if (scrnmng_write_bmp(path) != SCRNMNG_BMP_OK) {
		unlink(path);
		return 1;
	}
	file = fopen(path, "rb");
	if (file == NULL || fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes) ||
			fgetc(file) != EOF) {
		if (file != NULL) {
			fclose(file);
		}
		unlink(path);
		return 1;
	}
	fclose(file);
	unlink(path);

	if (bytes[0] != 'B' || bytes[1] != 'M' ||
			read_u32le(bytes + 2) != 70 || read_u32le(bytes + 10) != 54 ||
			read_u32le(bytes + 14) != 40 || read_u32le(bytes + 18) != 2 ||
			read_u32le(bytes + 22) != 2 || read_u16le(bytes + 26) != 1 ||
			read_u16le(bytes + 28) != 24 || read_u32le(bytes + 30) != 0 ||
			read_u32le(bytes + 34) != 16) {
		return 1;
	}
	/* Bottom-up row: blue, white, then deterministic zero padding. */
	if (bytes[54] != 0xff || bytes[55] != 0x00 || bytes[56] != 0x00 ||
			bytes[57] != 0xff || bytes[58] != 0xff || bytes[59] != 0xff ||
			bytes[60] != 0x00 || bytes[61] != 0x00 ||
			/* Top row: red, green, then deterministic zero padding. */
			bytes[62] != 0x00 || bytes[63] != 0x00 || bytes[64] != 0xff ||
			bytes[65] != 0x00 || bytes[66] != 0xff || bytes[67] != 0x00 ||
			bytes[68] != 0x00 || bytes[69] != 0x00) {
		return 1;
	}

	surface = scrnmng_surflock();
	if (surface == NULL || scrnmng_write_bmp(path) != SCRNMNG_BMP_BUSY) {
		if (surface != NULL) {
			scrnmng_surfunlock(surface);
		}
		return 1;
	}
	scrnmng_surfunlock(surface);
	scrnmng_shutdown();
	return 0;
}
