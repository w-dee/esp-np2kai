#ifndef NP2_VIDEO_CONTROL_V1_H
#define NP2_VIDEO_CONTROL_V1_H

#include <stddef.h>
#include <stdint.h>

#define NP2V_CONTROL_PHYSICAL_ADDRESS UINT32_C(0x2a000)
#define NP2V_CONTROL_SIZE UINT32_C(32)
#define NP2V_CONTROL_VERSION UINT16_C(1)
#define NP2V_CONTROL_HEADER_SIZE UINT16_C(16)
#define NP2V_CONTROL_SCENE_ID UINT16_C(1)

typedef enum {
	NP2V_STATE_INVALID = 0,
	NP2V_STATE_BOOTING = 1,
	NP2V_STATE_PROGRAMMING_VIDEO = 2,
	NP2V_STATE_SCENE_READY = 3,
	NP2V_STATE_ERROR = 4
} np2v_state;

typedef enum {
	NP2V_CONTROL_UNINITIALIZED = 0,
	NP2V_CONTROL_VALID = 1,
	NP2V_CONTROL_INVALID = 2,
	NP2V_CONTROL_PARTIAL = 3
} np2v_control_status;

typedef struct {
	uint16_t version;
	uint16_t header_size;
	uint16_t block_size;
	uint16_t scene_id;
	uint16_t diagnostic;
	np2v_state state;
} np2v_control;

np2v_control_status np2v_control_parse(
		const uint8_t *bytes, size_t length, np2v_control *control);
const char *np2v_control_status_name(np2v_control_status status);
const char *np2v_state_name(np2v_state state);

#endif /* NP2_VIDEO_CONTROL_V1_H */
