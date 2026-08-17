#include <compiler.h>
#include <scrnmng.h>
#include <scrnmng_bmp.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_u16le(FILE *file, uint16_t value)
{
	const uint8_t bytes[2] = {
		(uint8_t)(value & UINT16_C(0xff)),
		(uint8_t)(value >> 8)
	};

	return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static int write_u32le(FILE *file, uint32_t value)
{
	const uint8_t bytes[4] = {
		(uint8_t)(value & UINT32_C(0xff)),
		(uint8_t)((value >> 8) & UINT32_C(0xff)),
		(uint8_t)((value >> 16) & UINT32_C(0xff)),
		(uint8_t)(value >> 24)
	};

	return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static void rgb565_to_bgr888(uint16_t pixel, uint8_t *bgr)
{
	const uint8_t red5 = (uint8_t)((pixel >> 11) & 0x1f);
	const uint8_t green6 = (uint8_t)((pixel >> 5) & 0x3f);
	const uint8_t blue5 = (uint8_t)(pixel & 0x1f);

	/* Bit replication is deterministic and uses no floating point. */
	bgr[0] = (uint8_t)((blue5 << 3) | (blue5 >> 2));
	bgr[1] = (uint8_t)((green6 << 2) | (green6 >> 4));
	bgr[2] = (uint8_t)((red5 << 3) | (red5 >> 2));
}

const char *scrnmng_bmp_status_name(SCRNMNG_BMP_STATUS status)
{
	switch (status) {
	case SCRNMNG_BMP_OK:
		return "ok";
	case SCRNMNG_BMP_INVALID_ARGUMENT:
		return "invalid_argument";
	case SCRNMNG_BMP_NOT_INITIALIZED:
		return "not_initialized";
	case SCRNMNG_BMP_FAILED:
		return "failed";
	case SCRNMNG_BMP_BUSY:
		return "busy";
	case SCRNMNG_BMP_UNSUPPORTED:
		return "unsupported";
	case SCRNMNG_BMP_IO_ERROR:
		return "io_error";
	default:
		return "invalid";
	}
}

SCRNMNG_BMP_STATUS scrnmng_write_bmp(const char *path)
{
	SCRNMNG_SURFACE_VIEW view;
	SCRNMNG_SNAPSHOT_STATUS view_status;
	FILE *file;
	uint8_t *row_buffer;
	size_t row_bytes;
	size_t row_stride;
	size_t image_bytes;
	size_t file_size;
	size_t output_y;

	if (path == NULL || path[0] == '\0') {
		return SCRNMNG_BMP_INVALID_ARGUMENT;
	}
	view_status = scrnmng_get_surface_view(&view);
	if (view_status != SCRNMNG_SNAPSHOT_OK) {
		switch (view_status) {
		case SCRNMNG_SNAPSHOT_NOT_INITIALIZED:
			return SCRNMNG_BMP_NOT_INITIALIZED;
		case SCRNMNG_SNAPSHOT_FAILED:
			return SCRNMNG_BMP_FAILED;
		case SCRNMNG_SNAPSHOT_BUSY:
			return SCRNMNG_BMP_BUSY;
		case SCRNMNG_SNAPSHOT_UNSUPPORTED:
			return SCRNMNG_BMP_UNSUPPORTED;
		default:
			return SCRNMNG_BMP_INVALID_ARGUMENT;
		}
	}
	if (view.width <= 0 || view.height <= 0 || view.bpp != 16 ||
			view.pixel_format != SCRNMNG_PIXEL_FORMAT_RGB565LE) {
		return SCRNMNG_BMP_UNSUPPORTED;
	}
	if (view.pitch < (size_t)view.width * 2) {
		return SCRNMNG_BMP_UNSUPPORTED;
	}
	if ((size_t)view.width > (SIZE_MAX - 3) / 3) {
		return SCRNMNG_BMP_UNSUPPORTED;
	}
	row_bytes = (size_t)view.width * 3;
	row_stride = (row_bytes + 3) & ~(size_t)3;
	if ((size_t)view.height > SIZE_MAX / row_stride) {
		return SCRNMNG_BMP_UNSUPPORTED;
	}
	image_bytes = row_stride * (size_t)view.height;
	if (image_bytes > SIZE_MAX - 54 || image_bytes + 54 > UINT32_MAX ||
			(size_t)view.width > INT32_MAX || (size_t)view.height > INT32_MAX) {
		return SCRNMNG_BMP_UNSUPPORTED;
	}
	file_size = image_bytes + 54;
	row_buffer = (uint8_t *)calloc(1, row_stride);
	if (row_buffer == NULL) {
		return SCRNMNG_BMP_IO_ERROR;
	}
	file = fopen(path, "wb");
	if (file == NULL) {
		free(row_buffer);
		return SCRNMNG_BMP_IO_ERROR;
	}
	if (fputc('B', file) == EOF || fputc('M', file) == EOF ||
			!write_u32le(file, (uint32_t)file_size) ||
			!write_u16le(file, 0) || !write_u16le(file, 0) ||
			!write_u32le(file, 54) || !write_u32le(file, 40) ||
			!write_u32le(file, (uint32_t)view.width) ||
			!write_u32le(file, (uint32_t)view.height) ||
			!write_u16le(file, 1) || !write_u16le(file, 24) ||
			!write_u32le(file, 0) ||
			!write_u32le(file, (uint32_t)image_bytes) ||
			!write_u32le(file, 0) || !write_u32le(file, 0) ||
			!write_u32le(file, 0) || !write_u32le(file, 0)) {
		fclose(file);
		free(row_buffer);
		return SCRNMNG_BMP_IO_ERROR;
	}
	for (output_y = 0; output_y < (size_t)view.height; ++output_y) {
		const size_t source_y = (size_t)view.height - output_y - 1;
		size_t x;

		memset(row_buffer, 0, row_stride);
		for (x = 0; x < (size_t)view.width; ++x) {
			const uint8_t *pixel = view.ptr + source_y * view.pitch + x * 2;
			const uint16_t value = (uint16_t)pixel[0] |
					(uint16_t)((uint16_t)pixel[1] << 8);
			rgb565_to_bgr888(value, row_buffer + x * 3);
		}
		if (fwrite(row_buffer, 1, row_stride, file) != row_stride) {
			fclose(file);
			free(row_buffer);
			return SCRNMNG_BMP_IO_ERROR;
		}
	}
	if (fclose(file) != 0) {
		free(row_buffer);
		return SCRNMNG_BMP_IO_ERROR;
	}
	free(row_buffer);
	return SCRNMNG_BMP_OK;
}
