#include "np2kbd_result_v1_parser.h"

#include <np2_crc32.h>

#include <stdio.h>
#include <string.h>

static unsigned failures;
static void expect(int condition, const char *message) { if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; } }
static void put16(uint8_t *p, size_t o, uint16_t v) { p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, size_t o, uint32_t v) { p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); p[o + 2] = (uint8_t)(v >> 16); p[o + 3] = (uint8_t)(v >> 24); }
static void header(uint8_t *p)
{
	memset(p, 0, 128); memcpy(p, "NP2T", 4); put16(p, 4, 1); put16(p, 6, 32); put16(p, 8, 128);
	put32(p, 12, NP2KBD_RESULT_V1_SUITE_ID); put32(p, 16, NP2KBD_RESULT_V1_BUILD_ID); put16(p, 20, 1); put16(p, 28, 0xffff);
}
static void finish(uint8_t *p) { put32(p, 120, np2_crc32_iso_hdlc(p, 120)); }

int main(void)
{
	uint8_t p[128]; np2kbd_result_v1_result result;
	memset(p, 0, sizeof(p)); expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_PRE_PROTOCOL, "pre protocol");
	/* RUNNING only commits the fixed header.  Both a partially written body
	 * with a stale CRC and a complete new body with a fresh CRC remain live. */
	header(p); p[124] = 1;
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_RUNNING, "running stale CRC");
	put16(p, 22, 1); put16(p, 24, 1);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_RUNNING, "running partial body");
	finish(p);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_RUNNING, "running complete body valid CRC");
	/* The same body becomes authoritative only after PASS is written. */
	p[124] = 2;
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_PASS, "pass 1/1");

	/* Repeat the live publication sequence for a terminal FAIL body. */
	header(p); p[124] = 1;
	put16(p, 22, 1); put16(p, 26, 1); put16(p, 28, NP2KBD_RESULT_V1_TEST_ID);
	put16(p, 30, 4); memcpy(p + 32, "KBD1", 4);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_RUNNING, "running partial fail body");
	finish(p);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_RUNNING, "running complete fail body valid CRC");
	p[124] = 3;
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_FAIL, "fail KBD1");

	/* Once terminal, integrity and semantic checks are mandatory. */
	p[120] ^= 1;
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_INVALID, "terminal bad CRC");
	header(p); put16(p, 22, 1); put16(p, 24, 1); p[124] = 2; p[96] = 1; finish(p);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_INVALID, "terminal reserved byte");
	header(p); put16(p, 22, 2); put16(p, 24, 1); p[124] = 2; finish(p);
	expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_INVALID, "terminal malformed counts");
	p[12] ^= 1; finish(p); expect(np2kbd_result_v1_parse(p, sizeof(p), &result) == NP2KBD_RESULT_V1_INVALID, "wrong suite rejected");
	return failures != 0;
}
