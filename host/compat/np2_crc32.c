#include <np2_crc32.h>

#include <stdint.h>

#define NP2_CRC32_ISO_HDLC_REFLECTED_POLYNOMIAL UINT32_C(0xedb88320)

uint32_t np2_crc32_iso_hdlc_init(void)
{
	return UINT32_C(0xffffffff);
}

uint32_t np2_crc32_iso_hdlc_update(uint32_t running,
		const uint8_t *data, size_t length)
{
	size_t offset;
	unsigned bit;

	if ((data == NULL) && (length != 0)) {
		return running;
	}
	for (offset = 0; offset < length; ++offset) {
		running ^= data[offset];
		for (bit = 0; bit < 8; ++bit) {
			running = (running & UINT32_C(1)) != 0 ?
				(running >> 1) ^ NP2_CRC32_ISO_HDLC_REFLECTED_POLYNOMIAL :
				(running >> 1);
		}
	}
	return running;
}

uint32_t np2_crc32_iso_hdlc_finish(uint32_t running)
{
	return running ^ UINT32_C(0xffffffff);
}

uint32_t np2_crc32_iso_hdlc(const uint8_t *data, size_t length)
{
	return np2_crc32_iso_hdlc_finish(
		np2_crc32_iso_hdlc_update(np2_crc32_iso_hdlc_init(), data, length));
}
