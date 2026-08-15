#include <compiler.h>
#include <fontmng.h>

int main(void)
{
	void *first = fontmng_create(16, 0, "");
	void *second = fontmng_create(16, 0, "");

	if (first != NULL || second != NULL) {
		return 1;
	}
	if (fontmng_get(NULL, "A") != NULL) {
		return 1;
	}

	fontmng_destroy(NULL);
	fontmng_destroy(first);
	fontmng_destroy(second);
	return 0;
}
