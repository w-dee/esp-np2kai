#ifndef NP2_HOST_SCRNMNG_BMP_H
#define NP2_HOST_SCRNMNG_BMP_H

typedef enum {
	SCRNMNG_BMP_OK = 0,
	SCRNMNG_BMP_INVALID_ARGUMENT = 1,
	SCRNMNG_BMP_NOT_INITIALIZED = 2,
	SCRNMNG_BMP_FAILED = 3,
	SCRNMNG_BMP_BUSY = 4,
	SCRNMNG_BMP_UNSUPPORTED = 5,
	SCRNMNG_BMP_IO_ERROR = 6
} SCRNMNG_BMP_STATUS;

#ifdef __cplusplus
extern "C" {
#endif

SCRNMNG_BMP_STATUS scrnmng_write_bmp(const char *path);
const char *scrnmng_bmp_status_name(SCRNMNG_BMP_STATUS status);

#ifdef __cplusplus
}
#endif

#endif /* NP2_HOST_SCRNMNG_BMP_H */
