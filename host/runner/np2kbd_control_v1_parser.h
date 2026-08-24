#ifndef NP2KBD_CONTROL_V1_PARSER_H
#define NP2KBD_CONTROL_V1_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define NP2KBD_CONTROL_V1_SIZE 64u
#define NP2KBD_CONTROL_V1_VERSION 1u
#define NP2KBD_CONTROL_V1_HEADER_SIZE 26u
#define NP2KBD_CONTROL_V1_BLOCK_SIZE 64u
#define NP2KBD_CONTROL_V1_FLAGS 0u
#define NP2KBD_CONTROL_V1_SUITE_ID UINT32_C(0x4e504b31)
#define NP2KBD_CONTROL_V1_BUILD_ID UINT32_C(0x00010001)
#define NP2KBD_CONTROL_V1_EXPECTED_MAKE UINT8_C(0x1d)
#define NP2KBD_CONTROL_V1_EXPECTED_BREAK UINT8_C(0x9d)

#define NP2KBD_CONTROL_V1_MAGIC_OFFSET 0u
#define NP2KBD_CONTROL_V1_VERSION_OFFSET 4u
#define NP2KBD_CONTROL_V1_HEADER_SIZE_OFFSET 6u
#define NP2KBD_CONTROL_V1_BLOCK_SIZE_OFFSET 8u
#define NP2KBD_CONTROL_V1_FLAGS_OFFSET 10u
#define NP2KBD_CONTROL_V1_SUITE_ID_OFFSET 12u
#define NP2KBD_CONTROL_V1_BUILD_ID_OFFSET 16u
#define NP2KBD_CONTROL_V1_EXPECTED_MAKE_OFFSET 20u
#define NP2KBD_CONTROL_V1_EXPECTED_BREAK_OFFSET 21u
#define NP2KBD_CONTROL_V1_OBSERVED_MAKE_OFFSET 22u
#define NP2KBD_CONTROL_V1_OBSERVED_BREAK_OFFSET 23u
#define NP2KBD_CONTROL_V1_FAILURE_REASON_OFFSET 24u
#define NP2KBD_CONTROL_V1_RESERVED_BODY_OFFSET 26u
#define NP2KBD_CONTROL_V1_RESERVED_BODY_SIZE 30u
#define NP2KBD_CONTROL_V1_CRC_OFFSET 56u
#define NP2KBD_CONTROL_V1_CRC_END 56u
#define NP2KBD_CONTROL_V1_STATE_OFFSET 60u
#define NP2KBD_CONTROL_V1_RESERVED_TAIL_OFFSET 61u
#define NP2KBD_CONTROL_V1_RESERVED_TAIL_SIZE 3u

typedef enum {
	NP2KBD_CONTROL_V1_STATE_UNINITIALIZED = 0,
	NP2KBD_CONTROL_V1_STATE_READY = 1,
	NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED = 2,
	NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED = 3,
	NP2KBD_CONTROL_V1_STATE_FAIL = 4
} np2kbd_control_v1_state;

typedef enum {
	NP2KBD_CONTROL_V1_FAILURE_NONE = 0,
	NP2KBD_CONTROL_V1_FAILURE_PRECONDITION_DATA_READY = 1,
	NP2KBD_CONTROL_V1_FAILURE_STATUS_OVERFLOW = 2,
	NP2KBD_CONTROL_V1_FAILURE_MAKE_MISMATCH = 3,
	NP2KBD_CONTROL_V1_FAILURE_BREAK_MISMATCH = 4
} np2kbd_control_v1_failure_reason;

typedef enum {
	NP2KBD_CONTROL_V1_PRE_PROTOCOL = 0,
	NP2KBD_CONTROL_V1_UNINITIALIZED,
	NP2KBD_CONTROL_V1_READY,
	NP2KBD_CONTROL_V1_MAKE_OBSERVED,
	NP2KBD_CONTROL_V1_BREAK_OBSERVED,
	NP2KBD_CONTROL_V1_FAIL,
	/* A recognized state whose body/CRC is moving during publication. */
	NP2KBD_CONTROL_V1_TRANSIENT,
	NP2KBD_CONTROL_V1_INVALID
} np2kbd_control_v1_observation;

typedef struct {
	np2kbd_control_v1_observation observation;
	np2kbd_control_v1_state state;
	uint8_t expected_make;
	uint8_t expected_break;
	uint8_t observed_make;
	uint8_t observed_break;
	uint16_t failure_reason;
} np2kbd_control_v1_result;

np2kbd_control_v1_observation np2kbd_control_v1_parse(
		const uint8_t *snapshot, size_t snapshot_size,
		np2kbd_control_v1_result *result);

typedef enum {
	NP2KBD_CONTROL_V1_TRACK_INVALID = 0,
	NP2KBD_CONTROL_V1_TRACK_ACCEPTED,
	NP2KBD_CONTROL_V1_TRACK_TRANSIENT
} np2kbd_control_v1_track_result;

typedef struct {
	uint8_t have_state;
	uint8_t terminal;
	np2kbd_control_v1_state state;
	uint8_t accepted_snapshot[NP2KBD_CONTROL_V1_SIZE];
} np2kbd_control_v1_tracker;

void np2kbd_control_v1_tracker_init(np2kbd_control_v1_tracker *tracker);
np2kbd_control_v1_track_result np2kbd_control_v1_tracker_observe(
		np2kbd_control_v1_tracker *tracker,
		const uint8_t *snapshot, size_t snapshot_size);

#endif /* NP2KBD_CONTROL_V1_PARSER_H */
