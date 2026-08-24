#include "np2kbd_result_v1_parser.h"

#include <np2_crc32.h>

#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)(p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int zero(const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; ++i) if (p[i]) return 0; return 1; }

np2kbd_result_v1_observation np2kbd_result_v1_parse(
		const uint8_t *snapshot, size_t snapshot_size,
		np2kbd_result_v1_result *result)
{
	uint8_t state;
	if (result == NULL) return NP2KBD_RESULT_V1_INVALID;
	memset(result, 0, sizeof(*result));
	result->observation = NP2KBD_RESULT_V1_INVALID;
	if (snapshot == NULL || snapshot_size != NP2KBD_RESULT_V1_SIZE) return result->observation;
	if (memcmp(snapshot, "NP2T", 4) != 0) { result->observation = NP2KBD_RESULT_V1_PRE_PROTOCOL; return result->observation; }
	state = snapshot[NP2KBD_RESULT_V1_STATE_OFFSET];
	if (state == 0) { result->observation = NP2KBD_RESULT_V1_UNINITIALIZED; return result->observation; }
	if (state != 1 && state != 2 && state != 3) return result->observation;
	if (u16(snapshot + 4) != NP2KBD_RESULT_V1_VERSION || u16(snapshot + 6) != NP2KBD_RESULT_V1_HEADER_SIZE ||
		u16(snapshot + 8) != NP2KBD_RESULT_V1_BLOCK_SIZE || u16(snapshot + 10) != 0 ||
		u32(snapshot + 12) != NP2KBD_RESULT_V1_SUITE_ID || u32(snapshot + 16) != NP2KBD_RESULT_V1_BUILD_ID ||
		u16(snapshot + 20) != NP2KBD_RESULT_V1_TOTAL_COUNT) return result->observation;
	result->completed_count = u16(snapshot + 22);
	result->passed_count = u16(snapshot + 24);
	result->failed_count = u16(snapshot + 26);
	result->first_failed_id = u16(snapshot + 28);
	result->diagnostic_length = u16(snapshot + 30);
	memcpy(result->diagnostic, snapshot + 32, sizeof(result->diagnostic));
	if (result->diagnostic_length > sizeof(result->diagnostic) ||
		!zero(result->diagnostic + result->diagnostic_length, sizeof(result->diagnostic) - result->diagnostic_length) ||
		!zero(snapshot + 96, 24) || !zero(snapshot + 125, 3) ||
		u32(snapshot + NP2KBD_RESULT_V1_CRC_OFFSET) != np2_crc32_iso_hdlc(snapshot, NP2KBD_RESULT_V1_CRC_END)) return result->observation;
	if (state == 1) { result->observation = NP2KBD_RESULT_V1_RUNNING; return result->observation; }
	if (state == 2 && result->completed_count == 1 && result->passed_count == 1 && result->failed_count == 0 &&
		result->first_failed_id == NP2KBD_RESULT_V1_NO_FAILED_ID && result->diagnostic_length == 0) {
		result->observation = NP2KBD_RESULT_V1_PASS; return result->observation;
	}
	if (state == 3 && result->completed_count == 1 && result->passed_count == 0 && result->failed_count == 1 &&
		result->first_failed_id == NP2KBD_RESULT_V1_TEST_ID && result->diagnostic_length == 4 &&
		memcmp(result->diagnostic, "KBD1", 4) == 0) result->observation = NP2KBD_RESULT_V1_FAIL;
	return result->observation;
}
