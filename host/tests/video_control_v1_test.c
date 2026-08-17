#include "video_control_v1.h"

#include <assert.h>
#include <string.h>

static void make_valid(uint8_t bytes[NP2V_CONTROL_SIZE])
{
	memset(bytes, 0, NP2V_CONTROL_SIZE);
	memcpy(bytes, "NP2V", 4);
	bytes[4] = 1;
	bytes[6] = 16;
	bytes[8] = NP2V_CONTROL_SIZE;
	bytes[10] = 1;
	bytes[31] = NP2V_STATE_SCENE_READY;
}

static void set_scene(uint8_t bytes[NP2V_CONTROL_SIZE], uint16_t scene_id)
{
	bytes[10] = (uint8_t)(scene_id & 0xff);
	bytes[11] = (uint8_t)(scene_id >> 8);
}

int main(void)
{
	uint8_t bytes[NP2V_CONTROL_SIZE];
	np2v_control control;

	memset(bytes, 0, sizeof(bytes));
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) ==
			NP2V_CONTROL_UNINITIALIZED);
	bytes[0] = 'N';
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) ==
			NP2V_CONTROL_PARTIAL);
	make_valid(bytes);
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_VALID);
	assert(control.scene_id == 1);
	assert(control.state == NP2V_STATE_SCENE_READY);
	set_scene(bytes, 2);
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	assert(np2v_control_parse_for_scene(bytes, sizeof(bytes), 2, &control) ==
			NP2V_CONTROL_VALID);
	assert(control.scene_id == 2);

	bytes[0] = 'X';
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	make_valid(bytes);
	bytes[4] = 2;
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	make_valid(bytes);
	bytes[31] = 9;
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	make_valid(bytes);
	bytes[8] = 31;
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	make_valid(bytes);
	bytes[14] = 1;
	assert(np2v_control_parse(bytes, sizeof(bytes), &control) == NP2V_CONTROL_INVALID);
	assert(np2v_control_parse(bytes, sizeof(bytes) - 1, &control) == NP2V_CONTROL_INVALID);
	return 0;
}
