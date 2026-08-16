#include <stdio.h>
#include <string.h>

#include "result_v1_parser.h"

static unsigned failures;

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void put_u16(uint8_t *snapshot, size_t offset, uint16_t value)
{
	snapshot[offset] = (uint8_t)value;
	snapshot[offset + 1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *snapshot, size_t offset, uint32_t value)
{
	snapshot[offset] = (uint8_t)value;
	snapshot[offset + 1] = (uint8_t)(value >> 8);
	snapshot[offset + 2] = (uint8_t)(value >> 16);
	snapshot[offset + 3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_reference(const uint8_t *bytes, size_t size)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t offset;
	unsigned bit;

	for (offset = 0; offset < size; ++offset) {
		crc ^= bytes[offset];
		for (bit = 0; bit < 8; ++bit) {
			crc = (crc & UINT32_C(1)) ?
				((crc >> 1) ^ UINT32_C(0xedb88320)) : (crc >> 1);
		}
	}
	return crc ^ UINT32_C(0xffffffff);
}

static void write_crc(uint8_t *snapshot)
{
	put_u32(snapshot, NP2_RESULT_V1_CRC_OFFSET,
		crc32_reference(snapshot, NP2_RESULT_V1_CRC_END));
}

static void make_header(uint8_t *snapshot)
{
	memset(snapshot, 0, NP2_RESULT_V1_SIZE);
	memcpy(snapshot + NP2_RESULT_V1_MAGIC_OFFSET, NP2_RESULT_V1_MAGIC, 4);
	put_u16(snapshot, NP2_RESULT_V1_VERSION_OFFSET, NP2_RESULT_V1_VERSION);
	put_u16(snapshot, NP2_RESULT_V1_HEADER_SIZE_OFFSET, NP2_RESULT_V1_HEADER_SIZE);
	put_u16(snapshot, NP2_RESULT_V1_BLOCK_SIZE_OFFSET, NP2_RESULT_V1_BLOCK_SIZE);
	put_u16(snapshot, NP2_RESULT_V1_FLAGS_OFFSET, NP2_RESULT_V1_FLAGS);
	put_u32(snapshot, NP2_RESULT_V1_SUITE_ID_OFFSET, NP2_RESULT_V1_SUITE_ID);
	put_u32(snapshot, NP2_RESULT_V1_BUILD_ID_OFFSET, NP2_RESULT_V1_BUILD_ID);
	put_u16(snapshot, NP2_RESULT_V1_TOTAL_COUNT_OFFSET, NP2_RESULT_V1_TOTAL_COUNT);
	put_u16(snapshot, NP2_RESULT_V1_FIRST_FAILED_ID_OFFSET,
		NP2_RESULT_V1_NO_FAILED_ID);
}

static void make_pass(uint8_t *snapshot)
{
	make_header(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_COMPLETED_COUNT_OFFSET, 13);
	put_u16(snapshot, NP2_RESULT_V1_PASSED_COUNT_OFFSET, 13);
	put_u16(snapshot, NP2_RESULT_V1_FAILED_COUNT_OFFSET, 0);
	put_u16(snapshot, NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET, 0);
	write_crc(snapshot);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = NP2_RESULT_V1_STATE_PASS;
}

static void make_fail(uint8_t *snapshot, uint16_t failed_id, uint16_t passed)
{
	make_header(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_COMPLETED_COUNT_OFFSET,
		(uint16_t)(passed + 1));
	put_u16(snapshot, NP2_RESULT_V1_PASSED_COUNT_OFFSET, passed);
	put_u16(snapshot, NP2_RESULT_V1_FAILED_COUNT_OFFSET, 1);
	put_u16(snapshot, NP2_RESULT_V1_FIRST_FAILED_ID_OFFSET, failed_id);
	put_u16(snapshot, NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET, 4);
	memcpy(snapshot + NP2_RESULT_V1_DIAGNOSTIC_OFFSET, "STG1", 4);
	write_crc(snapshot);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = NP2_RESULT_V1_STATE_FAIL;
}

static np2_result_v1_result parse(const uint8_t *snapshot, size_t size)
{
	np2_result_v1_result result;

	(void)np2_result_v1_parse(snapshot, size, &result);
	return result;
}

static void test_preprotocol(void)
{
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;

	memset(snapshot, 0, sizeof(snapshot));
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_PRE_PROTOCOL,
		"all-zero snapshot is pre-protocol");

	memset(snapshot, 0xff, sizeof(snapshot));
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_PRE_PROTOCOL,
		"all-ff snapshot is pre-protocol");
}

static void test_uninitialized_and_running(void)
{
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;

	memset(snapshot, 0, sizeof(snapshot));
	memcpy(snapshot + NP2_RESULT_V1_MAGIC_OFFSET, NP2_RESULT_V1_MAGIC, 4);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_UNINITIALIZED,
		"magic-only publication window is uninitialized");
	expect(result.completed_count == 0 && result.passed_count == 0 &&
			result.failed_count == 0 && result.diagnostic_length == 0,
		"uninitialized publication window leaves dynamic fields zero");

	memset(snapshot, 0, sizeof(snapshot));
	memcpy(snapshot + NP2_RESULT_V1_MAGIC_OFFSET, NP2_RESULT_V1_MAGIC, 4);
	put_u16(snapshot, NP2_RESULT_V1_VERSION_OFFSET, NP2_RESULT_V1_VERSION);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_UNINITIALIZED,
		"partial header publication window is uninitialized");

	make_header(snapshot);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = NP2_RESULT_V1_STATE_UNINITIALIZED;
	put_u16(snapshot, NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET, 65);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_UNINITIALIZED,
		"valid state zero does not require terminal validation");

	make_header(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_COMPLETED_COUNT_OFFSET, 7);
	put_u16(snapshot, NP2_RESULT_V1_PASSED_COUNT_OFFSET, 6);
	put_u16(snapshot, NP2_RESULT_V1_FAILED_COUNT_OFFSET, 1);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = NP2_RESULT_V1_STATE_RUNNING;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_RUNNING,
		"running accepts stale CRC and live body changes");

	memset(snapshot, 0, sizeof(snapshot));
	memcpy(snapshot + NP2_RESULT_V1_MAGIC_OFFSET, NP2_RESULT_V1_MAGIC, 4);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = NP2_RESULT_V1_STATE_RUNNING;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID,
		"partial header is invalid once running is committed");
}

static void test_pass_and_fail(void)
{
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;

	make_pass(snapshot);
	expect(crc32_reference(snapshot, NP2_RESULT_V1_CRC_END) == UINT32_C(0x58f5b827),
		"formal PASS body has the known CRC");
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_PASS, "valid PASS parses");
	expect(result.completed_count == 13 && result.passed_count == 13 &&
			result.failed_count == 0 &&
			result.first_failed_id == NP2_RESULT_V1_NO_FAILED_ID &&
			result.diagnostic_length == 0,
		"PASS fields are returned");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_FAIL, "valid FAIL parses");
	expect(result.completed_count == 3 && result.passed_count == 2 &&
			result.failed_count == 1 && result.first_failed_id == UINT16_C(0x0204) &&
			result.diagnostic_length == 4 && memcmp(result.diagnostic, "STG1", 4) == 0,
		"FAIL fields are returned");
}

static void test_fixed_header_and_state_errors(void)
{
	static const size_t offsets[] = {
		NP2_RESULT_V1_VERSION_OFFSET,
		NP2_RESULT_V1_HEADER_SIZE_OFFSET,
		NP2_RESULT_V1_BLOCK_SIZE_OFFSET,
		NP2_RESULT_V1_SUITE_ID_OFFSET,
		NP2_RESULT_V1_BUILD_ID_OFFSET,
		NP2_RESULT_V1_TOTAL_COUNT_OFFSET
	};
	static const uint32_t bad_values[] = {2, 31, 127, 0, 0, 12};
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;
	size_t index;

	make_pass(snapshot);
	for (index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
		make_pass(snapshot);
		if (index < 3 || index == 5) {
			put_u16(snapshot, offsets[index], (uint16_t)bad_values[index]);
		} else {
			put_u32(snapshot, offsets[index], bad_values[index]);
		}
		result = parse(snapshot, sizeof(snapshot));
		expect(result.observation == NP2_RESULT_V1_INVALID,
			"malformed fixed header is invalid");
	}

	make_pass(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_VERSION_OFFSET, 99);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID,
		"unsupported version is invalid");

	make_pass(snapshot);
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = 0xff;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID,
		"unknown state with protocol header is invalid");

	memset(snapshot, 0, sizeof(snapshot));
	snapshot[NP2_RESULT_V1_STATE_OFFSET] = 0xff;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_PRE_PROTOCOL,
		"unknown state without protocol magic is pre-protocol");
}

static void test_terminal_integrity(void)
{
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;

	make_pass(snapshot);
	snapshot[0x20] ^= 1;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "covered-byte CRC failure is invalid");

	make_pass(snapshot);
	snapshot[NP2_RESULT_V1_CRC_OFFSET] ^= 1;
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "stored CRC failure is invalid");

	make_pass(snapshot);
	snapshot[NP2_RESULT_V1_RESERVED_BODY_OFFSET] = 1;
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "reserved body is validated");

	make_pass(snapshot);
	snapshot[NP2_RESULT_V1_RESERVED_TAIL_OFFSET] = 1;
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "reserved tail is validated");

	make_pass(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_COMPLETED_COUNT_OFFSET, 12);
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "PASS counter mismatch is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	put_u16(snapshot, NP2_RESULT_V1_FAILED_COUNT_OFFSET, 2);
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "FAIL counter mismatch is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	put_u16(snapshot, NP2_RESULT_V1_FIRST_FAILED_ID_OFFSET, 13);
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "unknown FAIL ID is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	put_u16(snapshot, NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET, 65);
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "oversized diagnostic is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	snapshot[NP2_RESULT_V1_DIAGNOSTIC_OFFSET] = 'X';
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "wrong diagnostic marker is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	snapshot[NP2_RESULT_V1_DIAGNOSTIC_OFFSET + 4] = 1;
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID,
		"nonzero FAIL diagnostic padding is invalid");

	make_fail(snapshot, UINT16_C(0x0204), 2);
	put_u16(snapshot, NP2_RESULT_V1_COMPLETED_COUNT_OFFSET, 2);
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID,
		"inconsistent FAIL counters are invalid");

	make_pass(snapshot);
	put_u16(snapshot, NP2_RESULT_V1_DIAGNOSTIC_LENGTH_OFFSET, 1);
	snapshot[NP2_RESULT_V1_DIAGNOSTIC_OFFSET] = 'X';
	write_crc(snapshot);
	result = parse(snapshot, sizeof(snapshot));
	expect(result.observation == NP2_RESULT_V1_INVALID, "PASS diagnostic is invalid");
}

static void test_size_and_output_reset(void)
{
	uint8_t snapshot[NP2_RESULT_V1_SIZE];
	np2_result_v1_result result;

	make_pass(snapshot);
	(void)np2_result_v1_parse(snapshot, sizeof(snapshot), &result);
	result = parse(snapshot, 0);
	expect(result.observation == NP2_RESULT_V1_INVALID, "zero-size input is invalid");
	expect(result.completed_count == 0 && result.diagnostic_length == 0 &&
			result.diagnostic[0] == 0,
		"invalid parse clears output");
	result = parse(snapshot, 127);
	expect(result.observation == NP2_RESULT_V1_INVALID, "short input is invalid");
	result = parse(snapshot, 129);
	expect(result.observation == NP2_RESULT_V1_INVALID, "long input is invalid");
	result = parse(NULL, NP2_RESULT_V1_SIZE);
	expect(result.observation == NP2_RESULT_V1_INVALID, "NULL input is invalid");

	make_pass(snapshot);
	(void)np2_result_v1_parse(snapshot, sizeof(snapshot), &result);
	memset(snapshot, 0, sizeof(snapshot));
	(void)np2_result_v1_parse(snapshot, sizeof(snapshot), &result);
	expect(result.observation == NP2_RESULT_V1_PRE_PROTOCOL &&
			result.completed_count == 0 && result.passed_count == 0 &&
			result.failed_count == 0 && result.diagnostic_length == 0 &&
			result.diagnostic[0] == 0,
		"pre-protocol parse clears prior terminal fields");

	expect(np2_result_v1_parse(snapshot, sizeof(snapshot), NULL) ==
			NP2_RESULT_V1_INVALID, "NULL output is invalid");
}

int main(void)
{
	test_preprotocol();
	test_uninitialized_and_running();
	test_pass_and_fail();
	test_fixed_header_and_state_errors();
	test_terminal_integrity();
	test_size_and_output_reset();
	if (failures != 0) {
		fprintf(stderr, "%u parser test(s) failed\n", failures);
		return 1;
	}
	puts("result-v1 parser tests passed");
	return 0;
}
