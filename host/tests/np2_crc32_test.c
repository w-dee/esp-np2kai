#include <np2_crc32.h>

#include <stdint.h>

int main(void)
{
	static const uint8_t check[] = "123456789";
	uint32_t running;

	if (np2_crc32_iso_hdlc(check, sizeof(check) - 1) != UINT32_C(0xcbf43926) ||
			np2_crc32_iso_hdlc(NULL, 0) != 0) {
		return 1;
	}
	running = np2_crc32_iso_hdlc_init();
	running = np2_crc32_iso_hdlc_update(running, check, 3);
	running = np2_crc32_iso_hdlc_update(running, check + 3,
			sizeof(check) - 1 - 3);
	if (np2_crc32_iso_hdlc_finish(running) != UINT32_C(0xcbf43926)) {
		return 1;
	}
	return 0;
}
