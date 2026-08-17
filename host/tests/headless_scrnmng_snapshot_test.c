#include <compiler.h>
#include <np2_crc32.h>
#include <scrnmng.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t reference_crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t offset;

	/* Independent bit-at-a-time reference: the production helper processes
	 * each byte with a different loop shape. */
	for (offset = 0; offset < length; ++offset) {
		uint8_t byte = data[offset];
		unsigned bit;

		for (bit = 0; bit < 8; ++bit) {
			const uint32_t mix = (crc ^ byte) & UINT32_C(1);

			crc >>= 1;
			if (mix != 0) {
				crc ^= UINT32_C(0xedb88320);
			}
			byte >>= 1;
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static uint32_t reference_zero_crc(int width, int height)
{
	const size_t bytes = (size_t)width * (size_t)height * 2;
	uint8_t *zeroes = (uint8_t *)calloc(1, bytes);
	uint32_t crc;

	if (zeroes == NULL) {
		return UINT32_C(0xffffffff);
	}
	crc = reference_crc32(zeroes, bytes);
	free(zeroes);
	return crc;
}

static void put_expected_pixel(uint8_t *bytes, size_t pitch,
		int x, int y, uint16_t pixel)
{
	uint8_t *destination = bytes + (size_t)y * pitch + (size_t)x * 2;

	destination[0] = (uint8_t)(pixel & UINT16_C(0xff));
	destination[1] = (uint8_t)(pixel >> 8);
}

static uint16_t pattern_pixel(int x, int y)
{
	static const uint16_t colors[] = {
		UINT16_C(0x0000), UINT16_C(0xffff), UINT16_C(0xf800),
		UINT16_C(0x07e0), UINT16_C(0x001f), UINT16_C(0x11aa)
	};

	if (y == 0 && x < (int)(sizeof(colors) / sizeof(colors[0]))) {
		return colors[x];
	}
	return (uint16_t)((((x * 7 + y * 3) & 0x1f) << 11) |
			(((x * 11 + y * 5) & 0x3f) << 5) |
			((x * 13 + y * 17) & 0x1f));
}

static int check_snapshot(const SCRNMNG_SNAPSHOT *snapshot,
		int width, int height, uint32_t generation,
		uint32_t sequence, uint32_t expected_crc)
{
	return snapshot->width == width && snapshot->height == height &&
		snapshot->bpp == 16 &&
		snapshot->pixel_format == SCRNMNG_PIXEL_FORMAT_RGB565LE &&
		snapshot->pitch == (size_t)width * 2 &&
		snapshot->visible_bytes == (size_t)width * (size_t)height * 2 &&
		snapshot->surface_generation == generation &&
		snapshot->surface_update_sequence == sequence &&
		snapshot->crc_algorithm == NP2_CRC32_ISO_HDLC_ALGORITHM &&
		snapshot->crc_version == NP2_CRC32_ISO_HDLC_VERSION &&
		snapshot->crc32 == expected_crc;
}

int main(void)
{
	SCRNMNG_SNAPSHOT snapshot;
	const SCRNSURF *surface;
	uint8_t expected[8 * 4 * 2];
	uint32_t generation;
	uint32_t sequence;
	int index;

	scrnmng_shutdown();
	if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_NOT_INITIALIZED) {
		return 1;
	}
	if (!scrnmng_initialize() ||
			scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_OK ||
			!check_snapshot(&snapshot, 640, 400, 1, 0,
				reference_zero_crc(640, 400))) {
		return 1;
	}
	generation = snapshot.surface_generation;
	sequence = snapshot.surface_update_sequence;

	surface = scrnmng_surflock();
	if (surface == NULL ||
			scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_BUSY) {
		if (surface != NULL) {
			scrnmng_surfunlock(surface);
		}
		return 1;
	}
	scrnmng_surfunlock(surface);
	++sequence;

	scrnmng_setwidth(0, 8);
	scrnmng_setheight(0, 4);
	if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_OK ||
			!(snapshot.surface_generation > generation) ||
			snapshot.surface_update_sequence != sequence ||
			snapshot.crc32 != reference_zero_crc(8, 4)) {
		return 1;
	}
	generation = snapshot.surface_generation;

	memset(expected, 0, sizeof(expected));
	surface = scrnmng_surflock();
	if (surface == NULL) {
		return 1;
	}
	for (index = 0; index < 8 * 4; ++index) {
		const int x = index % 8;
		const int y = index / 8;
		const uint16_t pixel = pattern_pixel(x, y);
		uint8_t *destination = surface->ptr + (size_t)y *
				(size_t)surface->yalign + (size_t)x * 2;

		destination[0] = (uint8_t)(pixel & UINT16_C(0xff));
		destination[1] = (uint8_t)(pixel >> 8);
		put_expected_pixel(expected, 8 * 2, x, y, pixel);
	}
	scrnmng_surfunlock(surface);
	++sequence;
	if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_OK ||
			!check_snapshot(&snapshot, 8, 4, generation, sequence,
				reference_crc32(expected, sizeof(expected)))) {
		return 1;
	}

	surface = scrnmng_surflock();
	if (surface == NULL) {
		return 1;
	}
	for (index = 0; index < (int)sizeof(expected); ++index) {
		if (surface->ptr[index] != expected[index]) {
			scrnmng_surfunlock(surface);
			return 1;
		}
	}
	scrnmng_surfunlock(surface);
	++sequence;
	if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_OK ||
			!check_snapshot(&snapshot, 8, 4, generation, sequence,
				reference_crc32(expected, sizeof(expected)))) {
		return 1;
	}

	{
		static const int heights[] = { 200, 400, 480 };
		for (index = 0; index < (int)(sizeof(heights) / sizeof(heights[0]));
				++index) {
			const uint32_t old_generation = generation;

			scrnmng_setwidth(0, 640);
			scrnmng_setheight(0, heights[index]);
			if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_OK ||
					!(snapshot.surface_generation > old_generation) ||
					snapshot.surface_update_sequence != sequence ||
					snapshot.crc32 != reference_zero_crc(640, heights[index])) {
				return 1;
			}
			generation = snapshot.surface_generation;
		}
	}

	scrnmng_shutdown();
	if (scrnmng_snapshot(&snapshot) != SCRNMNG_SNAPSHOT_NOT_INITIALIZED) {
		return 1;
	}
	return 0;
}
