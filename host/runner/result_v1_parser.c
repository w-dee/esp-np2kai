#include "result_v1_parser.h"

#include <string.h>

static uint16_t read_u16le(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32le(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
			((uint32_t)bytes[1] << 8) |
			((uint32_t)bytes[2] << 16) |
			((uint32_t)bytes[3] << 24);
}

static uint32_t crc32_iso_hdlc(const uint8_t *bytes, size_t size)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t offset;
	unsigned bit;

	for (offset = 0; offset < size; ++offset) {
		crc ^= bytes[offset];
		for (bit = 0; bit < 8; ++bit) {
			if (crc & UINT32_C(1)) {
				crc = (crc >> 1) ^ UINT32_C(0xedb88320);
			} else {
				crc >>= 1;
			}
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static int has_magic(const uint8_t *snapshot)
{
	return snapshot[0] == (uint8_t)'N' &&
			snapshot[1] == (uint8_t)'P' &&
			snapshot[2] == (uint8_t)'2' &&
			snapshot[3] == (uint8_t)'T';
}

static int fixed_header_is_valid(const uint8_t *snapshot)
{
	return read_u16le(snapshot + NP2_RESULT_V1_VERSION_OFFSET) == NP2_RESULT_V1_VERSION &&
			read_u16le(snapshot + NP2_RESULT_V1_HEADER_SIZE_OFFSET) == NP2_RESULT_V1_HEADER_SIZE &&
			read_u16le(snapshot + NP2_RESULT_V1_BLOCK_SIZE_OFFSET) == NP2_RESULT_V1_BLOCK_SIZE &&
			read_u16le(snapshot + NP2_RESULT_V1_FLAGS_OFFSET) == NP2_RESULT_V1_FLAGS &&
			read_u32le(snapshot + NP2_RESULT_V1_SUITE_ID_OFFSET) == NP2_RESULT_V1_SUITE_ID &&
			read_u32le(snapshot + NP2_RESULT_V1_BUILD_ID_OFFSET) == NP2_RESULT_V1_BUILD_ID &&
			read_u16le(snapshot + NP2_RESULT_V1_TOTAL_COUNT_OFFSET) == NP2_RESULT_V1_TOTAL_COUNT;
}

static void read_dynamic_fields(const uint8_t *snapshot, np2_result_v1_result *result)
{
	result->completed_count = read_u16le(snapshot + NP2_RESULT_V1_COMPLETED_COUNT_OFFSET);
	result->passed_count = read_u16le(snapshot + NP2_RESULT_V1_PASSED_COUNT_OFFSET);
	result->failed_count = read_u16le(snapshot + NP2_RESULT_V1_FAILED_COUNT_OFFSET);
	result->first_failed_id = read_u16le(snapshot + NP2_RESULT_V1_FIRST_FAILED_ID_OFFSET);
	result->diagnostic_length = read_u16le(snapshot + NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET);
	memcpy(result->diagnostic, snapshot + NP2_RESULT_V1_DIAGNOSTIC_OFFSET,
			sizeof(result->diagnostic));
}

static int bytes_are_zero(const uint8_t *bytes, size_t size)
{
	size_t index;

	for (index = 0; index < size; ++index) {
		if (bytes[index] != 0) {
			return 0;
		}
	}
	return 1;
}

static int active_test_id(uint16_t id)
{
	static const uint16_t ids[] = {
		UINT16_C(0x0101), UINT16_C(0x0201), UINT16_C(0x0203),
		UINT16_C(0x0204), UINT16_C(0x0205), UINT16_C(0x0301),
		UINT16_C(0x0401), UINT16_C(0x0502), UINT16_C(0x0602),
		UINT16_C(0x0701), UINT16_C(0x0801), UINT16_C(0x0901),
		UINT16_C(0x0b01)
	};
	size_t index;

	for (index = 0; index < sizeof(ids) / sizeof(ids[0]); ++index) {
		if (ids[index] == id) {
			return 1;
		}
	}
	return 0;
}

static int terminal_common_is_valid(const uint8_t *snapshot,
		const np2_result_v1_result *result)
{
	uint32_t stored_crc;

	if (result->diagnostic_length > NP2_RESULT_V1_DIAGNOSTIC_SIZE ||
			!bytes_are_zero(result->diagnostic + result->diagnostic_length,
				NP2_RESULT_V1_DIAGNOSTIC_SIZE - result->diagnostic_length) ||
			!bytes_are_zero(snapshot + NP2_RESULT_V1_RESERVED_BODY_OFFSET,
				NP2_RESULT_V1_RESERVED_BODY_SIZE) ||
			!bytes_are_zero(snapshot + NP2_RESULT_V1_RESERVED_TAIL_OFFSET,
				NP2_RESULT_V1_RESERVED_TAIL_SIZE)) {
		return 0;
	}

	stored_crc = read_u32le(snapshot + NP2_RESULT_V1_CRC_OFFSET);
	return stored_crc == crc32_iso_hdlc(snapshot, NP2_RESULT_V1_CRC_END);
}

static int pass_is_valid(const np2_result_v1_result *result)
{
	return result->completed_count == NP2_RESULT_V1_TOTAL_COUNT &&
			result->passed_count == NP2_RESULT_V1_TOTAL_COUNT &&
			result->failed_count == 0 &&
			result->first_failed_id == NP2_RESULT_V1_NO_FAILED_ID &&
			result->diagnostic_length == 0;
}

static int fail_is_valid(const np2_result_v1_result *result)
{
	return result->failed_count == 1 &&
			result->passed_count < NP2_RESULT_V1_TOTAL_COUNT &&
			result->completed_count == (uint16_t)(result->passed_count + 1) &&
			result->completed_count <= NP2_RESULT_V1_TOTAL_COUNT &&
			active_test_id(result->first_failed_id) &&
			result->diagnostic_length == 4 &&
			memcmp(result->diagnostic, "STG1", 4) == 0;
}

np2_result_v1_observation np2_result_v1_parse(
	const uint8_t *snapshot,
	size_t snapshot_size,
	np2_result_v1_result *result)
{
	uint8_t state;

	if (result == NULL) {
		return NP2_RESULT_V1_INVALID;
	}
	memset(result, 0, sizeof(*result));
	result->observation = NP2_RESULT_V1_INVALID;

	if (snapshot == NULL || snapshot_size != NP2_RESULT_V1_SIZE) {
		return result->observation;
	}
	if (!has_magic(snapshot)) {
		result->observation = NP2_RESULT_V1_PRE_PROTOCOL;
		return result->observation;
	}
	state = snapshot[NP2_RESULT_V1_STATE_OFFSET];
	if (state == NP2_RESULT_V1_STATE_UNINITIALIZED) {
		result->observation = NP2_RESULT_V1_UNINITIALIZED;
		return result->observation;
	}
	if (state != NP2_RESULT_V1_STATE_RUNNING &&
			state != NP2_RESULT_V1_STATE_PASS &&
			state != NP2_RESULT_V1_STATE_FAIL) {
		return result->observation;
	}
	if (!fixed_header_is_valid(snapshot)) {
		return result->observation;
	}

	read_dynamic_fields(snapshot, result);
	switch (state) {
		case NP2_RESULT_V1_STATE_RUNNING:
			result->observation = NP2_RESULT_V1_RUNNING;
			break;
		case NP2_RESULT_V1_STATE_PASS:
			if (terminal_common_is_valid(snapshot, result) && pass_is_valid(result)) {
				result->observation = NP2_RESULT_V1_PASS;
			}
			break;
		case NP2_RESULT_V1_STATE_FAIL:
			if (terminal_common_is_valid(snapshot, result) && fail_is_valid(result)) {
				result->observation = NP2_RESULT_V1_FAIL;
			}
			break;
		default:
			break;
	}
	return result->observation;
}
