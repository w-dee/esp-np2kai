#include "np2kbd_control_v1_parser.h"

#include <np2_crc32.h>

#include <string.h>

static uint16_t read_u16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] |
			((uint32_t)bytes[1] << 8) |
			((uint32_t)bytes[2] << 16) |
			((uint32_t)bytes[3] << 24);
}

static void set_result(np2kbd_control_v1_result *result,
		np2kbd_control_v1_observation observation,
		np2kbd_control_v1_state state)
{
	memset(result, 0, sizeof(*result));
	result->observation = observation;
	result->state = state;
}

static int has_magic(const uint8_t *snapshot)
{
	return memcmp(snapshot, "NP2K", 4) == 0;
}

static int fixed_header_valid(const uint8_t *snapshot)
{
	return read_u16(snapshot + NP2KBD_CONTROL_V1_VERSION_OFFSET) ==
			NP2KBD_CONTROL_V1_VERSION &&
			read_u16(snapshot + NP2KBD_CONTROL_V1_HEADER_SIZE_OFFSET) ==
			NP2KBD_CONTROL_V1_HEADER_SIZE &&
			read_u16(snapshot + NP2KBD_CONTROL_V1_BLOCK_SIZE_OFFSET) ==
			NP2KBD_CONTROL_V1_BLOCK_SIZE &&
			read_u16(snapshot + NP2KBD_CONTROL_V1_FLAGS_OFFSET) ==
			NP2KBD_CONTROL_V1_FLAGS &&
			read_u32(snapshot + NP2KBD_CONTROL_V1_SUITE_ID_OFFSET) ==
			NP2KBD_CONTROL_V1_SUITE_ID &&
			read_u32(snapshot + NP2KBD_CONTROL_V1_BUILD_ID_OFFSET) ==
			NP2KBD_CONTROL_V1_BUILD_ID &&
			snapshot[NP2KBD_CONTROL_V1_EXPECTED_MAKE_OFFSET] ==
			NP2KBD_CONTROL_V1_EXPECTED_MAKE &&
			snapshot[NP2KBD_CONTROL_V1_EXPECTED_BREAK_OFFSET] ==
			NP2KBD_CONTROL_V1_EXPECTED_BREAK;
}

static int zeros(const uint8_t *bytes, size_t size)
{
	size_t index;
	for (index = 0; index < size; ++index) {
		if (bytes[index] != 0) {
			return 0;
		}
	}
	return 1;
}

static int body_valid(const uint8_t *snapshot)
{
	const uint32_t stored = read_u32(snapshot + NP2KBD_CONTROL_V1_CRC_OFFSET);
	return zeros(snapshot + NP2KBD_CONTROL_V1_RESERVED_BODY_OFFSET,
			NP2KBD_CONTROL_V1_RESERVED_BODY_SIZE) &&
			zeros(snapshot + NP2KBD_CONTROL_V1_RESERVED_TAIL_OFFSET,
			NP2KBD_CONTROL_V1_RESERVED_TAIL_SIZE) &&
			stored == np2_crc32_iso_hdlc(snapshot, NP2KBD_CONTROL_V1_CRC_END);
}

static np2kbd_control_v1_observation state_observation(
		np2kbd_control_v1_state state)
{
	switch (state) {
		case NP2KBD_CONTROL_V1_STATE_UNINITIALIZED:
			return NP2KBD_CONTROL_V1_UNINITIALIZED;
		case NP2KBD_CONTROL_V1_STATE_READY:
			return NP2KBD_CONTROL_V1_READY;
		case NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED:
			return NP2KBD_CONTROL_V1_MAKE_OBSERVED;
		case NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED:
			return NP2KBD_CONTROL_V1_BREAK_OBSERVED;
		case NP2KBD_CONTROL_V1_STATE_FAIL:
			return NP2KBD_CONTROL_V1_FAIL;
		default:
			return NP2KBD_CONTROL_V1_INVALID;
	}
}

np2kbd_control_v1_observation np2kbd_control_v1_parse(
		const uint8_t *snapshot, size_t snapshot_size,
		np2kbd_control_v1_result *result)
{
	np2kbd_control_v1_state state;
	np2kbd_control_v1_observation observation;

	if (result == NULL) {
		return NP2KBD_CONTROL_V1_INVALID;
	}
	set_result(result, NP2KBD_CONTROL_V1_INVALID,
		NP2KBD_CONTROL_V1_STATE_UNINITIALIZED);
	if (snapshot == NULL || snapshot_size != NP2KBD_CONTROL_V1_SIZE) {
		return result->observation;
	}
	if (!has_magic(snapshot)) {
		result->observation = NP2KBD_CONTROL_V1_PRE_PROTOCOL;
		return result->observation;
	}
	state = (np2kbd_control_v1_state)snapshot[NP2KBD_CONTROL_V1_STATE_OFFSET];
	observation = state_observation(state);
	if (observation == NP2KBD_CONTROL_V1_INVALID || !fixed_header_valid(snapshot)) {
		return result->observation;
	}
	result->state = state;
	result->expected_make = snapshot[NP2KBD_CONTROL_V1_EXPECTED_MAKE_OFFSET];
	result->expected_break = snapshot[NP2KBD_CONTROL_V1_EXPECTED_BREAK_OFFSET];
	result->observed_make = snapshot[NP2KBD_CONTROL_V1_OBSERVED_MAKE_OFFSET];
	result->observed_break = snapshot[NP2KBD_CONTROL_V1_OBSERVED_BREAK_OFFSET];
	result->failure_reason = read_u16(snapshot + NP2KBD_CONTROL_V1_FAILURE_REASON_OFFSET);
	if (state == NP2KBD_CONTROL_V1_STATE_UNINITIALIZED) {
		result->observation = body_valid(snapshot) ? observation :
			(zeros(snapshot, NP2KBD_CONTROL_V1_SIZE) ? observation :
			NP2KBD_CONTROL_V1_INVALID);
		return result->observation;
	}
	if (!body_valid(snapshot)) {
		result->observation = NP2KBD_CONTROL_V1_TRANSIENT;
		return result->observation;
	}
	if ((state == NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED &&
			(result->observed_make != NP2KBD_CONTROL_V1_EXPECTED_MAKE ||
			 result->observed_break != 0 || result->failure_reason !=
			 NP2KBD_CONTROL_V1_FAILURE_NONE)) ||
		(state == NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED &&
			(result->observed_make != NP2KBD_CONTROL_V1_EXPECTED_MAKE ||
			 result->observed_break != NP2KBD_CONTROL_V1_EXPECTED_BREAK ||
			 result->failure_reason != NP2KBD_CONTROL_V1_FAILURE_NONE)) ||
		(state == NP2KBD_CONTROL_V1_STATE_READY &&
			(result->observed_make != 0 || result->observed_break != 0 ||
			 result->failure_reason != NP2KBD_CONTROL_V1_FAILURE_NONE)) ||
		(state == NP2KBD_CONTROL_V1_STATE_FAIL &&
			(result->failure_reason < NP2KBD_CONTROL_V1_FAILURE_PRECONDITION_DATA_READY ||
			 result->failure_reason > NP2KBD_CONTROL_V1_FAILURE_BREAK_MISMATCH))) {
		result->observation = NP2KBD_CONTROL_V1_INVALID;
		return result->observation;
	}
	result->observation = observation;
	return observation;
}

void np2kbd_control_v1_tracker_init(np2kbd_control_v1_tracker *tracker)
{
	if (tracker != NULL) {
		memset(tracker, 0, sizeof(*tracker));
	}
}

np2kbd_control_v1_track_result np2kbd_control_v1_tracker_observe(
		np2kbd_control_v1_tracker *tracker,
		const uint8_t *snapshot, size_t snapshot_size)
{
	np2kbd_control_v1_result parsed;
	np2kbd_control_v1_observation observation;
	np2kbd_control_v1_state raw_state;

	if (tracker == NULL || snapshot == NULL ||
		snapshot_size != NP2KBD_CONTROL_V1_SIZE) {
		return NP2KBD_CONTROL_V1_TRACK_INVALID;
	}
	raw_state = (np2kbd_control_v1_state)snapshot[NP2KBD_CONTROL_V1_STATE_OFFSET];
	/* State is the publication commit marker.  Do not parse the body before
	 * deciding whether this is a precommit or same-state publication race. */
	if (!tracker->have_state) {
		observation = np2kbd_control_v1_parse(snapshot, snapshot_size, &parsed);
		if (observation == NP2KBD_CONTROL_V1_READY ||
			observation == NP2KBD_CONTROL_V1_FAIL) {
			tracker->have_state = 1;
			tracker->state = raw_state;
			tracker->terminal = raw_state == NP2KBD_CONTROL_V1_STATE_FAIL;
			memcpy(tracker->accepted_snapshot, snapshot, snapshot_size);
			return NP2KBD_CONTROL_V1_TRACK_ACCEPTED;
		}
		if (observation == NP2KBD_CONTROL_V1_MAKE_OBSERVED ||
			observation == NP2KBD_CONTROL_V1_BREAK_OBSERVED) {
			/* A fully valid later state before READY is a protocol violation,
			 * not an incomplete publication. */
			return NP2KBD_CONTROL_V1_TRACK_INVALID;
		}
		/* PRE_PROTOCOL, UNINITIALIZED, TRANSIENT, and malformed/partial
		 * candidates remain retryable until the first committed state. */
		return NP2KBD_CONTROL_V1_TRACK_TRANSIENT;
	}
	if (raw_state > NP2KBD_CONTROL_V1_STATE_FAIL) {
		return NP2KBD_CONTROL_V1_TRACK_INVALID;
	}
	if (tracker->terminal) {
		return memcmp(tracker->accepted_snapshot, snapshot, snapshot_size) == 0 ?
			NP2KBD_CONTROL_V1_TRACK_ACCEPTED :
			NP2KBD_CONTROL_V1_TRACK_INVALID;
	}
	if (raw_state < tracker->state) {
		return NP2KBD_CONTROL_V1_TRACK_INVALID;
	}
	if (raw_state == tracker->state) {
		if (memcmp(tracker->accepted_snapshot, snapshot, snapshot_size) != 0) {
			/* A body change while the old nonterminal state is still visible
			 * is always a publication race, even with a self-consistent CRC
			 * or a semantically incomplete body. */
			return NP2KBD_CONTROL_V1_TRACK_TRANSIENT;
		}
		return NP2KBD_CONTROL_V1_TRACK_ACCEPTED;
	}
	observation = np2kbd_control_v1_parse(snapshot, snapshot_size, &parsed);
	if (raw_state == NP2KBD_CONTROL_V1_STATE_FAIL) {
		if (observation != NP2KBD_CONTROL_V1_FAIL) {
			return NP2KBD_CONTROL_V1_TRACK_INVALID;
		}
	} else if (raw_state != (np2kbd_control_v1_state)(tracker->state + 1) ||
		(observation != NP2KBD_CONTROL_V1_READY &&
		 observation != NP2KBD_CONTROL_V1_MAKE_OBSERVED &&
		 observation != NP2KBD_CONTROL_V1_BREAK_OBSERVED)) {
		return NP2KBD_CONTROL_V1_TRACK_INVALID;
	}
	memcpy(tracker->accepted_snapshot, snapshot, snapshot_size);
	tracker->state = raw_state;
	tracker->terminal = raw_state == NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED ||
		raw_state == NP2KBD_CONTROL_V1_STATE_FAIL;
	return NP2KBD_CONTROL_V1_TRACK_ACCEPTED;
}
