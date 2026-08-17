#ifndef NP2_HOST_SHA256_H
#define NP2_HOST_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define NP2_SHA256_DIGEST_SIZE 32U

typedef struct {
	uint32_t state[8];
	uint64_t bit_count;
	uint8_t buffer[64];
	size_t buffer_size;
} np2_sha256_context;

void np2_sha256_init(np2_sha256_context *context);
void np2_sha256_update(np2_sha256_context *context,
		const uint8_t *data, size_t length);
void np2_sha256_final(np2_sha256_context *context,
		uint8_t digest[NP2_SHA256_DIGEST_SIZE]);

#endif /* NP2_HOST_SHA256_H */
