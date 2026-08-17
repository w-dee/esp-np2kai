#ifndef NP2_HOST_CRC32_H
#define NP2_HOST_CRC32_H

#include <stddef.h>
#include <stdint.h>

#define NP2_CRC32_ISO_HDLC_ALGORITHM 1U
#define NP2_CRC32_ISO_HDLC_VERSION 1U

#ifdef __cplusplus
extern "C" {
#endif

uint32_t np2_crc32_iso_hdlc_init(void);
uint32_t np2_crc32_iso_hdlc_update(uint32_t running,
		const uint8_t *data, size_t length);
uint32_t np2_crc32_iso_hdlc_finish(uint32_t running);
uint32_t np2_crc32_iso_hdlc(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* NP2_HOST_CRC32_H */
