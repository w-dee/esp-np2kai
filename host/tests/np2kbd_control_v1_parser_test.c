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
	np2kbd_control_v1_result result;

	/* A: erased/all-FF storage is pre-protocol, not an invalid state. */
	np2kbd_control_v1_tracker_init(&tracker);
	memset(p, 0xff, sizeof(p));
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_PRE_PROTOCOL, "all-FF pre-protocol");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "all-FF precommit transient");

	/* B: arbitrary non-magic bytes, including an out-of-domain raw state,
	 * remain retryable before the first accepted state. */
	np2kbd_control_v1_tracker_init(&tracker);
	memset(p, 0x5a, sizeof(p)); p[60] = 0xfe;
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_PRE_PROTOCOL, "random pre-protocol");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "random precommit transient");

	/* C: a partial magic/header publication is also transient. */
	np2kbd_control_v1_tracker_init(&tracker);
	memset(p, 0x5a, sizeof(p)); memcpy(p, "NP2", 3); p[60] = 0xfe;
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_PRE_PROTOCOL, "partial magic pre-protocol");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "partial magic precommit transient");

	/* D: recognizable magic with an incomplete header/body is not fatal
	 * while the first state is still unpublished. */
	np2kbd_control_v1_tracker_init(&tracker);
	memset(p, 0xff, sizeof(p)); memcpy(p, "NP2K", 4); p[60] = NP2KBD_CONTROL_V1_STATE_READY;
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_INVALID, "incomplete header invalid stateless");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "incomplete header precommit transient");

	/* E: a complete READY candidate with a stale CRC is transient. */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_READY); p[22] = NP2KBD_CONTROL_V1_EXPECTED_MAKE;
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_TRANSIENT, "stale READY CRC transient stateless");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "stale READY CRC precommit transient");

	/* F: READY is the first ordinary accepted publication. */
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	np2kbd_control_v1_tracker_init(&tracker);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "ready accepted");

	/* K: the next body may be fully valid while the old READY state byte is
	 * still visible; this is a publication race, not an invalid transition. */
	p[22] = NP2KBD_CONTROL_V1_EXPECTED_MAKE;
	crc(p);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_INVALID, "READY body semantic mismatch stateless");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "ready changed body valid CRC transient");

	/* The stale-CRC version of the same race is transient too. */
	p[60] = NP2KBD_CONTROL_V1_STATE_READY;
	p[56] ^= 0x01;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "ready changed body stale CRC transient");

	/* MAKE and BREAK are invalid as the first accepted state. */
	/* H */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_MAKE_OBSERVED, "make parses first");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "make first invalid");
	/* I */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_BREAK_OBSERVED, "break parses first");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "break first invalid");

	/* G: FAIL is allowed as a first accepted terminal publication. */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_FAIL);
	put16(p, 24, NP2KBD_CONTROL_V1_FAILURE_MAKE_MISMATCH); crc(p);
	expect(np2kbd_control_v1_parse(p, sizeof(p), &result) == NP2KBD_CONTROL_V1_FAIL, "fail parses first");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "fail first accepted");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "fail terminal identical accepted");

	/* Existing post-READY strict transition checks. */
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	np2kbd_control_v1_tracker_init(&tracker);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "ready accepted");
	p[22] = NP2KBD_CONTROL_V1_EXPECTED_MAKE;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "ready changed body stale CRC transient");
	crc(p);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "ready changed body valid CRC transient");
	p[60] = NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "make accepted");
	p[23] = NP2KBD_CONTROL_V1_EXPECTED_BREAK;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "make changed body stale CRC transient");
	crc(p);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "make changed body valid CRC transient");
	p[60] = NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "break accepted");
	put16(p, 24, NP2KBD_CONTROL_V1_FAILURE_MAKE_MISMATCH); crc(p);
	p[60] = NP2KBD_CONTROL_V1_STATE_FAIL;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "terminal state change immutable");
	p[22] = 0;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "terminal immutable");

	/* J: once READY is accepted, an out-of-domain raw state is invalid. */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "post-READY setup accepted");
	memset(p, 0xff, sizeof(p));
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "post-READY all-FF invalid");

	/* A terminal FAIL may be published while a nonterminal state remains
	 * visible; the body change is transient until FAIL is committed. */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "fail path ready accepted");
	p[22] = NP2KBD_CONTROL_V1_EXPECTED_MAKE; crc(p);
	p[60] = NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "fail path make accepted");
	put16(p, 24, NP2KBD_CONTROL_V1_FAILURE_MAKE_MISMATCH); crc(p);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_TRANSIENT, "fail body valid CRC old state transient");
	p[60] = NP2KBD_CONTROL_V1_STATE_FAIL;
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "fail committed accepted");
	p[22] = 0; crc(p);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "fail terminal immutable");

	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "backward rejected");
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "skipped state rejected");

	/* L: terminal BREAK remains immutable, including an identical replay. */
	np2kbd_control_v1_tracker_init(&tracker);
	control(p, NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED);
	/* Reach BREAK through the required READY -> MAKE -> BREAK sequence. */
	control(p, NP2KBD_CONTROL_V1_STATE_READY);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "terminal path ready accepted");
	control(p, NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "terminal path make accepted");
	control(p, NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "terminal path break accepted");
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_ACCEPTED, "terminal path identical accepted");
	p[22] = 0; crc(p);
	expect(np2kbd_control_v1_tracker_observe(&tracker, p, sizeof(p)) == NP2KBD_CONTROL_V1_TRACK_INVALID, "terminal path mutation invalid");
}

int main(void) { test_parser(); test_tracker(); return failures != 0; }
