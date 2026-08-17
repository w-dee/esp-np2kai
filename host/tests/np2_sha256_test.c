#include "np2_sha256.h"

#include <assert.h>
#include <string.h>

static void assert_digest(const char *input, const char *expected)
{
	np2_sha256_context context;
	uint8_t digest[NP2_SHA256_DIGEST_SIZE];
	static const char digits[] = "0123456789abcdef";
	char actual[NP2_SHA256_DIGEST_SIZE * 2 + 1];
	size_t index;

	np2_sha256_init(&context);
	np2_sha256_update(&context, (const uint8_t *)input, strlen(input));
	np2_sha256_final(&context, digest);
	for (index = 0; index < NP2_SHA256_DIGEST_SIZE; ++index) {
		actual[index * 2] = digits[digest[index] >> 4];
		actual[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	actual[sizeof(actual) - 1] = '\0';
	assert(strcmp(actual, expected) == 0);
}

int main(void)
{
	assert_digest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	assert_digest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	return 0;
}
