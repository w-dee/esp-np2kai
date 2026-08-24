#ifndef NP2KBD_RESULT_V1_PARSER_H
#define NP2KBD_RESULT_V1_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define NP2KBD_RESULT_V1_SIZE 128u
#define NP2KBD_RESULT_V1_VERSION 1u
#define NP2KBD_RESULT_V1_HEADER_SIZE 32u
#define NP2KBD_RESULT_V1_BLOCK_SIZE 128u
#define NP2KBD_RESULT_V1_SUITE_ID UINT32_C(0x4e504b31)
#define NP2KBD_RESULT_V1_BUILD_ID UINT32_C(0x00010001)
#define NP2KBD_RESULT_V1_TOTAL_COUNT 1u
#define NP2KBD_RESULT_V1_TEST_ID UINT16_C(0x0c01)
#define NP2KBD_RESULT_V1_NO_FAILED_ID UINT16_C(0xffff)
#define NP2KBD_RESULT_V1_DIAGNOSTIC_SIZE 64u
#define NP2KBD_RESULT_V1_CRC_END 120u
#define NP2KBD_RESULT_V1_CRC_OFFSET 120u
#define NP2KBD_RESULT_V1_STATE_OFFSET 124u

typedef enum {
	NP2KBD_RESULT_V1_PRE_PROTOCOL = 0,
	NP2KBD_RESULT_V1_UNINITIALIZED,
	NP2KBD_RESULT_V1_RUNNING,
	NP2KBD_RESULT_V1_PASS,
	NP2KBD_RESULT_V1_FAIL,
	NP2KBD_RESULT_V1_INVALID
} np2kbd_result_v1_observation;

typedef struct {
	np2kbd_result_v1_observation observation;
	uint16_t completed_count;
	uint16_t passed_count;
	uint16_t failed_count;
	uint16_t first_failed_id;
	uint16_t diagnostic_length;
	uint8_t diagnostic[NP2KBD_RESULT_V1_DIAGNOSTIC_SIZE];
} np2kbd_result_v1_result;

np2kbd_result_v1_observation np2kbd_result_v1_parse(
		const uint8_t *snapshot, size_t snapshot_size,
		np2kbd_result_v1_result *result);

#endif /* NP2KBD_RESULT_V1_PARSER_H */
