#include "np2_sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
	UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
	UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
	UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
	UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
	UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
	UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
	UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
	UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
	UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
	UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
	UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
	UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
	UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
	UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
	UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
	UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
	return (value >> count) | (value << (32U - count));
}

static uint32_t read_u32be(const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
			((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_u32be(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

static void transform(np2_sha256_context *context, const uint8_t block[64])
{
	uint32_t words[64];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t g;
	uint32_t h;
	unsigned i;

	for (i = 0; i < 16; ++i) {
		words[i] = read_u32be(block + i * 4U);
	}
	for (; i < 64; ++i) {
		const uint32_t s0 = rotate_right(words[i - 15], 7) ^
				rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
		const uint32_t s1 = rotate_right(words[i - 2], 17) ^
				rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}

	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	for (i = 0; i < 64; ++i) {
		const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
		const uint32_t choose = (e & f) ^ ((~e) & g);
		const uint32_t temporary1 = h + s1 + choose + round_constants[i] + words[i];
		const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
		const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t temporary2 = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}

	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

void np2_sha256_init(np2_sha256_context *context)
{
	if (context == NULL) {
		return;
	}
	context->state[0] = UINT32_C(0x6a09e667);
	context->state[1] = UINT32_C(0xbb67ae85);
	context->state[2] = UINT32_C(0x3c6ef372);
	context->state[3] = UINT32_C(0xa54ff53a);
	context->state[4] = UINT32_C(0x510e527f);
	context->state[5] = UINT32_C(0x9b05688c);
	context->state[6] = UINT32_C(0x1f83d9ab);
	context->state[7] = UINT32_C(0x5be0cd19);
	context->bit_count = 0;
	context->buffer_size = 0;
}

void np2_sha256_update(np2_sha256_context *context,
		const uint8_t *data, size_t length)
{
	if (context == NULL || (data == NULL && length != 0)) {
		return;
	}
	context->bit_count += (uint64_t)length * UINT64_C(8);
	while (length != 0) {
		size_t available = sizeof(context->buffer) - context->buffer_size;
		size_t copied = length < available ? length : available;

		memcpy(context->buffer + context->buffer_size, data, copied);
		context->buffer_size += copied;
		data += copied;
		length -= copied;
		if (context->buffer_size == sizeof(context->buffer)) {
			transform(context, context->buffer);
			context->buffer_size = 0;
		}
	}
}

void np2_sha256_final(np2_sha256_context *context,
		uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
	uint64_t bit_count;
	unsigned i;

	if (context == NULL || digest == NULL) {
		return;
	}
	bit_count = context->bit_count;
	context->buffer[context->buffer_size++] = 0x80;
	if (context->buffer_size > 56) {
		while (context->buffer_size < sizeof(context->buffer)) {
			context->buffer[context->buffer_size++] = 0;
		}
		transform(context, context->buffer);
		context->buffer_size = 0;
	}
	while (context->buffer_size < 56) {
		context->buffer[context->buffer_size++] = 0;
	}
	for (i = 0; i < 8; ++i) {
		context->buffer[56 + i] = (uint8_t)(bit_count >> (56U - i * 8U));
	}
	transform(context, context->buffer);
	for (i = 0; i < 8; ++i) {
		write_u32be(digest + i * 4U, context->state[i]);
	}
}
