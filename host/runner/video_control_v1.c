#include "video_control_v1.h"

#include <string.h>

#define NP2V_STATE_OFFSET 31U

static uint16_t read_u16le(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static int all_zero(const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; ++index) {
		if (bytes[index] != 0) {
			return 0;
		}
	}
	return 1;
}

np2v_control_status np2v_control_parse(
		const uint8_t *bytes, size_t length, np2v_control *control)
{
	uint16_t version;
	uint16_t header_size;
	uint16_t block_size;
	uint16_t scene_id;
	np2v_state state;

	if (control != NULL) {
		memset(control, 0, sizeof(*control));
	}
	if (bytes == NULL || length < NP2V_CONTROL_SIZE) {
		return NP2V_CONTROL_INVALID;
	}
	if (all_zero(bytes, NP2V_CONTROL_SIZE)) {
		return NP2V_CONTROL_UNINITIALIZED;
	}
	/* State is the commit byte and is written last by the guest. */
	if (bytes[NP2V_STATE_OFFSET] == 0) {
		return NP2V_CONTROL_PARTIAL;
	}
	if (memcmp(bytes, "NP2V", 4) != 0) {
		return NP2V_CONTROL_INVALID;
	}

	version = read_u16le(bytes + 4);
	header_size = read_u16le(bytes + 6);
	block_size = read_u16le(bytes + 8);
	scene_id = read_u16le(bytes + 10);
	state = (np2v_state)bytes[NP2V_STATE_OFFSET];
	if (version != NP2V_CONTROL_VERSION ||
			header_size != NP2V_CONTROL_HEADER_SIZE ||
			block_size != NP2V_CONTROL_SIZE ||
			scene_id != NP2V_CONTROL_SCENE_ID ||
			state < NP2V_STATE_BOOTING || state > NP2V_STATE_ERROR) {
		return NP2V_CONTROL_INVALID;
	}
	if (!all_zero(bytes + 14, 17)) {
		return NP2V_CONTROL_INVALID;
	}
	if (control != NULL) {
		control->version = version;
		control->header_size = header_size;
		control->block_size = block_size;
		control->scene_id = scene_id;
		control->diagnostic = read_u16le(bytes + 12);
		control->state = state;
	}
	return NP2V_CONTROL_VALID;
}

const char *np2v_control_status_name(np2v_control_status status)
{
	switch (status) {
	case NP2V_CONTROL_UNINITIALIZED:
		return "uninitialized";
	case NP2V_CONTROL_VALID:
		return "valid";
	case NP2V_CONTROL_INVALID:
		return "invalid";
	case NP2V_CONTROL_PARTIAL:
		return "partial";
	default:
		return "invalid";
	}
}

const char *np2v_state_name(np2v_state state)
{
	switch (state) {
	case NP2V_STATE_BOOTING:
		return "BOOTING";
	case NP2V_STATE_PROGRAMMING_VIDEO:
		return "PROGRAMMING_VIDEO";
	case NP2V_STATE_SCENE_READY:
		return "SCENE_READY";
	case NP2V_STATE_ERROR:
		return "ERROR";
	default:
		return "INVALID";
	}
}
