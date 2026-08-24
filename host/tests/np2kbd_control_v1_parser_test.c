#include "np2kbd_control_v1_parser.h"

#include <np2_crc32.h>

#include <stdio.h>
#include <string.h>

static unsigned failures;
static void expect(int condition, const char *message) { if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; } }
static void put16(uint8_t *p, size_t o, uint16_t v) { p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, size_t o, uint32_t v) { p[o] = (uint8_t)v; p[o + 1] = (uint8_t)(v >> 8); p[o + 2] = (uint8_t)(v >> 16); p[o + 3] = (uint8_t)(v >> 24); }
static void crc(uint8_t *p) { put32(p, 56, np2_crc32_iso_hdlc(p, 56)); }

static void control(uint8_t *p, uint8_t state)
{
	memset(p, 0, 64);
	memcpy(p, "NP2K", 4); put16(p, 4, 1); put16(p, 6, 26); put16(p, 8, 64);
	put32(p, 12, NP2KBD_CONTROL_V1_SUITE_ID); put32(p, 16, NP2KBD_CONTROL_V1_BUILD_ID);
	p[20] = 0x1d; p[21] = 0x9d;
	p[22] = state >= NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED ? 0x1d : 0;
	p[23] = state >= NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED ? 0x9d : 0;
	crc(p); p[60] = state;
}

static void test_parser(void)
{
	uint8_t p[64]; np2kbd_control_v1_result result;
	memset(p, 0, sizeof(p));
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_PRE_PROTOCOL, "zero pre-protocol");
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_READY, "ready parses");
	p[22] = 0x1e;
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_TRANSIENT, "bad CRC is transient");
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	p[22] = 0x1e; crc(p);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_INVALID, "wrong make is invalid");
	control(p, NP2KBD_CONTROL_V1_STATE_FAIL);
	put16(p, 24, NP2KBD_CONTROL_V1_FAILURE_MAKE_MISMATCH); crc(p);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_FAIL, "fail parses");
}

static void test_tracker(void)
{
	uint8_t p[64]; np2kbd_control_v1_tracker tracker;
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	np2kbd_control_v1_tracker_init(&tracker);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "ready accepted");
	p[22] = 0x1e;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "same-state partial accepted");
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "make accepted");
	p[23] = 0x9e;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "make same-state partial accepted");
	control(p, NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "break accepted");
	p[22] = 0;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "terminal immutable");
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "backward rejected");
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "skipped state rejected");
}

int main(void) { test_parser(); test_tracker(); return failures != 0; }
