#include <assert.h>

#include <compiler.h>

int main(void) {
	UINT32 previous = np2_host_gettick();

	for (UINT index = 0; index < 10000; index++) {
		UINT32 current = np2_host_gettick();
		UINT32 delta = current - previous;

		assert(delta < 0x80000000U);
		previous = current;
	}

	return 0;
}
