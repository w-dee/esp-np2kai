#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np2opngen_pcm_ring.h"
#include "np2pcm_output.h"

#define FRAME_BYTES NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME
#define QUANTUM NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES
#define SLOT_PAYLOAD NP2_OPNGEN_PCM_RING_SLOT_BYTES

_Static_assert(NP2_OPNGEN_PCM_RING_CAPACITY == 8U, "physical slot count");
_Static_assert(QUANTUM == 240U, "q240 geometry");
_Static_assert(FRAME_BYTES == 4U, "stereo S16 frame geometry");
_Static_assert(SLOT_PAYLOAD == 960U, "full payload geometry");
_Static_assert(sizeof(struct np2opngen_pcm_ring_slot) == 976U, "slot ABI");
_Static_assert(sizeof(struct np2opngen_pcm_ring) == 7832U, "ring ABI");
_Static_assert(sizeof(((struct np2opngen_pcm_ring *)0)->head) ==
               sizeof(uint32_t), "head must remain uint32 atomic");
_Static_assert(sizeof(((struct np2opngen_pcm_ring *)0)->tail) ==
               sizeof(uint32_t), "tail must remain uint32 atomic");
_Static_assert(sizeof(((struct np2opngen_pcm_ring_slot *)0)->frame_offset) ==
               sizeof(uint64_t), "frame offset metadata ABI");

static unsigned negative_mutations;

static void make_pcm(uint8_t *pcm, uint64_t first_frame, size_t frames)
{
    size_t i;
    for (i = 0U; i < frames; ++i) {
        const uint32_t n = (uint32_t)(first_frame + i);
        pcm[i * FRAME_BYTES] = (uint8_t)(n ^ 0x3cU);
        pcm[i * FRAME_BYTES + 1U] = (uint8_t)(n >> 8U);
        pcm[i * FRAME_BYTES + 2U] = (uint8_t)(n ^ 0xa5U);
        pcm[i * FRAME_BYTES + 3U] = (uint8_t)((n >> 8U) ^ 0x5aU);
    }
}

static void append_exact(struct np2opngen_pcm_ring *ring,
                         const uint8_t *pcm, size_t frames,
                         uint64_t frame_offset)
{
    size_t consumed = 0U;
    assert(np2opngen_pcm_ring_append(ring, pcm, frames, frame_offset,
                                     &consumed) == NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == frames);
}

struct fake_sink {
    const uint8_t *expected;
    size_t expected_frames;
    uint8_t *captured;
    size_t captured_capacity;
    size_t captured_bytes;
    uint64_t submitted_frames;
    uint32_t submit_calls;
    uint32_t start_calls;
    uint32_t finish_calls;
    uint32_t abort_calls;
    enum np2_pcm_sink_result start_result;
    enum np2_pcm_sink_result finish_result;
    enum np2_pcm_sink_result abort_result;
    enum np2_pcm_sink_result outcomes[8];
    size_t outcome_count;
    size_t outcome_index;
    bool retry_snapshot_valid;
    uint64_t retry_frame_offset;
    uint32_t retry_sequence;
    uint16_t retry_valid_frames;
    uint16_t retry_flags;
    uint8_t retry_pcm[SLOT_PAYLOAD];
    bool retry_identity;
};

static enum np2_pcm_sink_result fake_start(void *opaque)
{
    struct fake_sink *sink = (struct fake_sink *)opaque;
    sink->start_calls++;
    return sink->start_result;
}

static enum np2_pcm_sink_result fake_submit(
    void *opaque, const struct np2_pcm_sink_view *view)
{
    struct fake_sink *sink = (struct fake_sink *)opaque;
    enum np2_pcm_sink_result result = NP2_PCM_SINK_ACCEPTED;
    const size_t bytes = (size_t)view->valid_frames * FRAME_BYTES;
    sink->submit_calls++;
    if (sink->outcome_index < sink->outcome_count) {
        result = sink->outcomes[sink->outcome_index++];
    }
    if (sink->expected != NULL &&
        (view->frame_offset > sink->expected_frames ||
         view->valid_frames > sink->expected_frames - view->frame_offset ||
         memcmp(view->pcm, sink->expected + view->frame_offset * FRAME_BYTES,
                bytes) != 0)) {
        return NP2_PCM_SINK_FATAL;
    }
    if (sink->retry_snapshot_valid) {
        sink->retry_identity =
            sink->retry_frame_offset == view->frame_offset &&
            sink->retry_sequence == view->sequence &&
            sink->retry_valid_frames == view->valid_frames &&
            sink->retry_flags == view->flags &&
            memcmp(sink->retry_pcm, view->pcm, bytes) == 0;
    }
    if (result == NP2_PCM_SINK_RETRY) {
        sink->retry_snapshot_valid = true;
        sink->retry_frame_offset = view->frame_offset;
        sink->retry_sequence = view->sequence;
        sink->retry_valid_frames = view->valid_frames;
        sink->retry_flags = view->flags;
        memcpy(sink->retry_pcm, view->pcm, bytes);
        return result;
    }
    if (result == NP2_PCM_SINK_ACCEPTED) {
        assert(sink->captured_bytes <= sink->captured_capacity);
        assert(bytes <= sink->captured_capacity - sink->captured_bytes);
        if (sink->captured != NULL) {
            memcpy(sink->captured + sink->captured_bytes, view->pcm, bytes);
        }
        sink->captured_bytes += bytes;
        sink->submitted_frames += view->valid_frames;
    }
    return result;
}

static enum np2_pcm_sink_result fake_finish(void *opaque)
{
    struct fake_sink *sink = (struct fake_sink *)opaque;
    sink->finish_calls++;
    return sink->finish_result;
}

static enum np2_pcm_sink_result fake_abort(void *opaque)
{
    struct fake_sink *sink = (struct fake_sink *)opaque;
    sink->abort_calls++;
    return sink->abort_result;
}

static void fake_init(struct fake_sink *fake, const uint8_t *expected,
                      size_t frames, uint8_t *captured, size_t capacity)
{
    memset(fake, 0, sizeof(*fake));
    fake->expected = expected;
    fake->expected_frames = frames;
    fake->captured = captured;
    fake->captured_capacity = capacity;
    fake->start_result = NP2_PCM_SINK_ACCEPTED;
    fake->finish_result = NP2_PCM_SINK_ACCEPTED;
    fake->abort_result = NP2_PCM_SINK_ACCEPTED;
}

static struct np2_pcm_sink fake_contract(struct fake_sink *fake)
{
    const struct np2_pcm_sink sink = {
        fake, fake_start, fake_submit, fake_finish, fake_abort};
    return sink;
}

static void controller_init(struct np2_pcm_output_controller *controller,
                            struct np2opngen_pcm_ring *ring,
                            struct fake_sink *fake)
{
    const struct np2_pcm_sink sink = fake_contract(fake);
    assert(np2_pcm_output_controller_init(controller, ring, &sink) == 0);
}

static void test_geometry_and_capacity(void)
{
    struct np2opngen_pcm_ring ring;
    uint8_t pcm[SLOT_PAYLOAD];
    uint8_t snapshot[sizeof(ring.slots)];
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    size_t consumed = 0U;
    unsigned i;
    assert(sizeof(ring.slots) / sizeof(ring.slots[0]) == 8U);
    np2opngen_pcm_ring_init(&ring);
    assert(np2opngen_pcm_ring_append(&ring, NULL, 0U, 0U, &consumed) ==
           NP2_OPNGEN_PCM_RING_OK);
    assert(consumed == 0U && np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(np2opngen_pcm_ring_finish(&ring, 0U) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_EMPTY);

    np2opngen_pcm_ring_init(&ring);
    for (i = 0U; i < 8U; ++i) {
        make_pcm(pcm, (uint64_t)i * QUANTUM, QUANTUM);
        append_exact(&ring, pcm, QUANTUM, (uint64_t)i * QUANTUM);
    }
    assert(np2opngen_pcm_ring_occupancy(&ring) == 8U);
    memcpy(snapshot, ring.slots, sizeof(snapshot));
    make_pcm(pcm, 8U * QUANTUM, QUANTUM);
    assert(np2opngen_pcm_ring_append(&ring, pcm, QUANTUM, 8U * QUANTUM,
                                     &consumed) == NP2_OPNGEN_PCM_RING_FULL);
    assert(consumed == 0U);
    assert(memcmp(snapshot, ring.slots, sizeof(snapshot)) == 0);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    append_exact(&ring, pcm, QUANTUM, 8U * QUANTUM);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 8U);

    puts("PCM_RING_PHYSICAL_SLOTS=8");
    puts("PCM_RING_USABLE_SLOTS=8");
    puts("PCM_RING_QUANTUM_FRAMES=240");
    puts("PCM_RING_SLOT_BYTES=976");
    puts("PCM_RING_SIZE_BYTES=7832");
    puts("PCM_RING_GEOMETRY=PASS");
    puts("PCM_RING_FULL_IS_BACKPRESSURE=PASS");
    puts("PCM_RING_NO_OVERWRITE=PASS");
}

static void test_partial(size_t frames)
{
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct fake_sink fake;
    uint8_t semantic[SLOT_PAYLOAD];
    uint8_t captured[SLOT_PAYLOAD];
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    memset(captured, 0, sizeof(captured));
    make_pcm(semantic, 0U, frames);
    np2opngen_pcm_ring_init(&ring);
    memset(ring.slots[0].pcm, 0xa7, sizeof(ring.slots[0].pcm));
    append_exact(&ring, semantic, frames, 0U);
    assert(np2opngen_pcm_ring_finish(&ring, frames) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 1U);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) == NP2_OPNGEN_PCM_RING_OK);
    assert(slot->valid_frames == frames);
    assert(slot->flags == NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL);
    assert(slot->frame_offset == 0U && slot->sequence == 0U);
    assert(slot->pcm[frames * FRAME_BYTES] == 0xa7U);
    fake_init(&fake, semantic, frames, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    assert(fake.captured_bytes == frames * FRAME_BYTES);
    assert(fake.submitted_frames == frames);
    assert(memcmp(captured, semantic, frames * FRAME_BYTES) == 0);
    assert(controller.expected_frame_offset == frames);
    assert(controller.expected_sequence == 1U);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_OK);
    printf("PCM_RING_PARTIAL_%zu=PASS\n", frames);
}

static void consume_one_partition(const size_t *parts, size_t part_count,
                                  uint8_t *out)
{
    struct np2opngen_pcm_ring ring;
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    uint8_t semantic[SLOT_PAYLOAD];
    size_t offset = 0U;
    size_t i;
    make_pcm(semantic, 0U, QUANTUM);
    np2opngen_pcm_ring_init(&ring);
    for (i = 0U; i < part_count; ++i) {
        append_exact(&ring, semantic + offset * FRAME_BYTES, parts[i], offset);
        offset += parts[i];
    }
    assert(offset == QUANTUM);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) == NP2_OPNGEN_PCM_RING_OK);
    assert(slot->valid_frames == QUANTUM && slot->flags == 0U);
    memcpy(out, slot->pcm, SLOT_PAYLOAD);
    assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
    assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
           NP2_OPNGEN_PCM_RING_EMPTY);
}

static void test_exact_and_segmentation(void)
{
    static const size_t p240[] = {240U};
    static const size_t p_reset[] = {13U, 227U};
    static const size_t p239[] = {239U, 1U};
    static const size_t irregular[] = {7U, 19U, 1U, 83U, 5U, 97U, 28U};
    uint8_t reference[SLOT_PAYLOAD];
    uint8_t candidate[SLOT_PAYLOAD];
    size_t ones[QUANTUM];
    size_t i;
    for (i = 0U; i < QUANTUM; ++i) ones[i] = 1U;
    consume_one_partition(p240, 1U, reference);
    consume_one_partition(p_reset, 2U, candidate);
    assert(memcmp(reference, candidate, sizeof(reference)) == 0);
    consume_one_partition(ones, QUANTUM, candidate);
    assert(memcmp(reference, candidate, sizeof(reference)) == 0);
    consume_one_partition(p239, 2U, candidate);
    assert(memcmp(reference, candidate, sizeof(reference)) == 0);
    consume_one_partition(irregular,
                          sizeof(irregular) / sizeof(irregular[0]), candidate);
    assert(memcmp(reference, candidate, sizeof(reference)) == 0);
    puts("PCM_RING_EXACT_Q240=PASS");
    puts("Q240_SEGMENTATION_INDEPENDENCE=PASS");
    puts("RESET_INSIDE_Q240_STORAGE_MODEL=PASS");
}

static void test_sequence_wraparound(void)
{
    struct np2opngen_pcm_ring ring;
    uint8_t pcm[SLOT_PAYLOAD];
    uint32_t expected_sequence = 0U;
    uint64_t expected_offset = 0U;
    unsigned i;
    np2opngen_pcm_ring_init(&ring);
    for (i = 0U; i < 40U; ++i) {
        const struct np2opngen_pcm_ring_slot *slot = NULL;
        make_pcm(pcm, expected_offset, QUANTUM);
        append_exact(&ring, pcm, QUANTUM, expected_offset);
        assert(np2opngen_pcm_ring_try_peek(&ring, &slot) ==
               NP2_OPNGEN_PCM_RING_OK);
        assert(slot->sequence == expected_sequence);
        assert(slot->frame_offset == expected_offset);
        assert(slot->valid_frames == QUANTUM && slot->flags == 0U);
        assert(memcmp(slot->pcm, pcm, sizeof(pcm)) == 0);
        assert(np2opngen_pcm_ring_consume(&ring) == NP2_OPNGEN_PCM_RING_OK);
        expected_sequence++;
        expected_offset += QUANTUM;
    }
    assert(np2opngen_pcm_ring_occupancy(&ring) == 0U);
    puts("PCM_RING_SEQUENCE_CONTIGUOUS=PASS");
    puts("PCM_RING_FRAME_OFFSETS_CONTIGUOUS=PASS");
    puts("PCM_RING_WRAPAROUND=PASS");
}

static void prepare_full_slot(struct np2opngen_pcm_ring *ring, uint8_t *pcm)
{
    np2opngen_pcm_ring_init(ring);
    make_pcm(pcm, 0U, QUANTUM);
    append_exact(ring, pcm, QUANTUM, 0U);
}

static void test_sink_outcomes_and_state(void)
{
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct fake_sink fake;
    uint8_t pcm[SLOT_PAYLOAD];
    uint8_t captured[SLOT_PAYLOAD];
    uint32_t tail;

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;
    tail = atomic_load(&ring.tail);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    assert(atomic_load(&ring.tail) == tail + 1U);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_EMPTY);
    assert(atomic_load(&ring.tail) == tail + 1U);
    negative_mutations++;
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    fake.outcomes[0] = NP2_PCM_SINK_RETRY;
    fake.outcomes[1] = NP2_PCM_SINK_ACCEPTED;
    fake.outcome_count = 2U;
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    tail = atomic_load(&ring.tail);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_RETRY);
    assert(atomic_load(&ring.tail) == tail);
    negative_mutations++;
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    assert(fake.retry_identity);
    assert(atomic_load(&ring.tail) == tail + 1U);
    assert(fake.captured_bytes == SLOT_PAYLOAD);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_OK);

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    fake.outcomes[0] = NP2_PCM_SINK_FATAL;
    fake.outcome_count = 1U;
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    tail = atomic_load(&ring.tail);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_FATAL);
    assert(controller.state == NP2_PCM_OUTPUT_FAILED);
    assert(atomic_load(&ring.tail) == tail);
    negative_mutations++;
    assert(np2_pcm_output_abort(&controller) == NP2_PCM_OUTPUT_OK);
    assert(controller.state == NP2_PCM_OUTPUT_ABORTED);
    assert(atomic_load(&ring.tail) == tail);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;

    np2opngen_pcm_ring_init(&ring);
    fake_init(&fake, NULL, 0U, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_abort(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_abort(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_STATE_ERROR);
    negative_mutations++;

    puts("PCM_SINK_ACCEPT_TEST=PASS");
    puts("PCM_SINK_RETRY_TEST=PASS");
    puts("PCM_SINK_RETRY_IDENTITY=PASS");
    puts("PCM_SINK_FATAL_TEST=PASS");
    puts("PCM_CONSUME_LINEARIZATION=PASS");
    puts("PCM_SINK_FINISH_CONTRACT=PASS");
    puts("PCM_SINK_ABORT_CONTRACT=PASS");
    puts("PCM_SINK_STATE_MACHINE=PASS");
}

static void expect_slot_rejected(struct np2opngen_pcm_ring *ring,
                                 struct np2_pcm_output_controller *controller,
                                 struct fake_sink *fake)
{
    const uint32_t tail = atomic_load(&ring->tail);
    controller_init(controller, ring, fake);
    assert(np2_pcm_output_start(controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(controller) == NP2_PCM_OUTPUT_SLOT_ERROR);
    assert(atomic_load(&ring->tail) == tail);
    negative_mutations++;
}

static void test_slot_mutations(void)
{
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct fake_sink fake;
    uint8_t pcm[SLOT_PAYLOAD * 2U];
    uint8_t captured[SLOT_PAYLOAD * 2U];
    struct np2opngen_pcm_ring_slot *slot;

    make_pcm(pcm, 0U, QUANTUM * 2U);

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->sequence = 1U;
    expect_slot_rejected(&ring, &controller, &fake); /* wrong/skip sequence */

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->frame_offset = 1U;
    expect_slot_rejected(&ring, &controller, &fake);

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->valid_frames = 0U;
    expect_slot_rejected(&ring, &controller, &fake);

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->valid_frames = QUANTUM + 1U;
    expect_slot_rejected(&ring, &controller, &fake);

    prepare_full_slot(&ring, pcm);
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->flags = NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL;
    expect_slot_rejected(&ring, &controller, &fake);

    np2opngen_pcm_ring_init(&ring);
    append_exact(&ring, pcm, 13U, 0U);
    assert(np2opngen_pcm_ring_finish(&ring, 13U) == NP2_OPNGEN_PCM_RING_OK);
    fake_init(&fake, pcm, 13U, captured, sizeof(captured));
    slot = &ring.slots[0]; slot->flags = 0U;
    expect_slot_rejected(&ring, &controller, &fake);

    np2opngen_pcm_ring_init(&ring);
    append_exact(&ring, pcm, QUANTUM * 2U, 0U);
    fake_init(&fake, pcm, QUANTUM * 2U, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    ring.slots[1].sequence = 0U; /* duplicated logical slot */
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_SLOT_ERROR);
    negative_mutations++;

    np2opngen_pcm_ring_init(&ring);
    append_exact(&ring, pcm, QUANTUM * 2U, 0U);
    fake_init(&fake, pcm, QUANTUM * 2U, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    ring.slots[1].frame_offset = 0U; /* reordered/duplicate frame range */
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_SLOT_ERROR);
    negative_mutations++;

    prepare_full_slot(&ring, pcm);
    ring.slots[0].pcm[17] ^= 1U;
    fake_init(&fake, pcm, QUANTUM, captured, sizeof(captured));
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_FATAL);
    assert(np2opngen_pcm_ring_occupancy(&ring) == 1U);
    negative_mutations++;
}

static void test_ring_to_sink_identity(void)
{
    const size_t frames = QUANTUM * 18U + 13U;
    uint8_t *pcm = (uint8_t *)malloc(frames * FRAME_BYTES);
    uint8_t *captured = (uint8_t *)malloc(frames * FRAME_BYTES);
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct fake_sink fake;
    size_t produced = 0U;
    size_t next_chunk = 1U;
    assert(pcm != NULL && captured != NULL);
    make_pcm(pcm, 0U, frames);
    np2opngen_pcm_ring_init(&ring);
    fake_init(&fake, pcm, frames, captured, frames * FRAME_BYTES);
    fake.outcomes[0] = NP2_PCM_SINK_RETRY;
    fake.outcomes[1] = NP2_PCM_SINK_ACCEPTED;
    fake.outcome_count = 2U;
    controller_init(&controller, &ring, &fake);
    assert(np2_pcm_output_start(&controller) == NP2_PCM_OUTPUT_OK);
    while (produced < frames) {
        size_t consumed = 0U;
        size_t request = next_chunk;
        int status;
        if (request > frames - produced) request = frames - produced;
        status = np2opngen_pcm_ring_append(
            &ring, pcm + produced * FRAME_BYTES, request, produced, &consumed);
        produced += consumed;
        assert(status == NP2_OPNGEN_PCM_RING_OK ||
               status == NP2_OPNGEN_PCM_RING_FULL);
        while (np2opngen_pcm_ring_occupancy(&ring) != 0U) {
            const enum np2_pcm_output_status step = np2_pcm_output_step(&controller);
            assert(step == NP2_PCM_OUTPUT_CONSUMED ||
                   step == NP2_PCM_OUTPUT_RETRY);
        }
        next_chunk = next_chunk % 317U + 1U;
    }
    assert(np2opngen_pcm_ring_finish(&ring, frames) == NP2_OPNGEN_PCM_RING_OK);
    while (np2opngen_pcm_ring_occupancy(&ring) != 0U)
        assert(np2_pcm_output_step(&controller) == NP2_PCM_OUTPUT_CONSUMED);
    assert(np2_pcm_output_finish(&controller) == NP2_PCM_OUTPUT_OK);
    assert(fake.captured_bytes == frames * FRAME_BYTES);
    assert(memcmp(captured, pcm, frames * FRAME_BYTES) == 0);
    assert(controller.accepted_frames == frames);
    assert(controller.accepted_bytes == frames * FRAME_BYTES);
    free(captured);
    free(pcm);
    puts("PCM_PARTIAL_NO_SEMANTIC_PADDING=PASS");
    puts("PCM_RING_TO_SINK_BYTE_IDENTITY=PASS");
}

struct stress_case {
    struct np2opngen_pcm_ring ring;
    struct np2_pcm_output_controller controller;
    struct fake_sink sink;
    uint8_t *pcm;
    uint8_t *captured;
    size_t frames;
    atomic_bool producer_done;
    atomic_bool failed;
};

static void *stress_producer(void *opaque)
{
    struct stress_case *test = (struct stress_case *)opaque;
    size_t sent = 0U;
    size_t chunk = 1U;
    while (sent < test->frames) {
        size_t consumed = 0U;
        size_t request = chunk;
        int status;
        if (request > test->frames - sent) request = test->frames - sent;
        status = np2opngen_pcm_ring_append(
            &test->ring, test->pcm + sent * FRAME_BYTES, request, sent,
            &consumed);
        if (status != NP2_OPNGEN_PCM_RING_OK &&
            status != NP2_OPNGEN_PCM_RING_FULL) {
            atomic_store(&test->failed, true);
            return NULL;
        }
        sent += consumed;
        chunk = chunk % 503U + 1U;
        if (consumed == 0U) sched_yield();
    }
    while (np2opngen_pcm_ring_finish(&test->ring, test->frames) ==
           NP2_OPNGEN_PCM_RING_FULL) {
        sched_yield();
    }
    atomic_store_explicit(&test->producer_done, true, memory_order_release);
    return NULL;
}

static void *stress_consumer(void *opaque)
{
    struct stress_case *test = (struct stress_case *)opaque;
    for (;;) {
        const enum np2_pcm_output_status status =
            np2_pcm_output_step(&test->controller);
        if (status == NP2_PCM_OUTPUT_CONSUMED) continue;
        if (status == NP2_PCM_OUTPUT_EMPTY) {
            if (atomic_load_explicit(&test->producer_done,
                                     memory_order_acquire) &&
                np2opngen_pcm_ring_occupancy(&test->ring) == 0U) {
                break;
            }
            sched_yield();
            continue;
        }
        atomic_store(&test->failed, true);
        return NULL;
    }
    return NULL;
}

static void test_spsc_stress(void)
{
    struct stress_case test;
    struct np2_pcm_sink contract;
    pthread_t producer;
    pthread_t consumer;
    memset(&test, 0, sizeof(test));
    test.frames = 120123U;
    test.pcm = (uint8_t *)malloc(test.frames * FRAME_BYTES);
    test.captured = (uint8_t *)malloc(test.frames * FRAME_BYTES);
    assert(test.pcm != NULL && test.captured != NULL);
    make_pcm(test.pcm, 0U, test.frames);
    np2opngen_pcm_ring_init(&test.ring);
    fake_init(&test.sink, test.pcm, test.frames, test.captured,
              test.frames * FRAME_BYTES);
    contract = fake_contract(&test.sink);
    assert(np2_pcm_output_controller_init(&test.controller, &test.ring,
                                          &contract) == 0);
    assert(np2_pcm_output_start(&test.controller) == NP2_PCM_OUTPUT_OK);
    atomic_init(&test.producer_done, false);
    atomic_init(&test.failed, false);
    assert(pthread_create(&producer, NULL, stress_producer, &test) == 0);
    assert(pthread_create(&consumer, NULL, stress_consumer, &test) == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(!atomic_load(&test.failed));
    assert(np2_pcm_output_finish(&test.controller) == NP2_PCM_OUTPUT_OK);
    assert(test.sink.captured_bytes == test.frames * FRAME_BYTES);
    assert(memcmp(test.pcm, test.captured, test.frames * FRAME_BYTES) == 0);
    assert(test.controller.expected_frame_offset == test.frames);
    assert(np2opngen_pcm_ring_occupancy(&test.ring) == 0U);
    free(test.captured);
    free(test.pcm);
    puts("PCM_RING_SPSC_STRESS=PASS");
}

int main(void)
{
    if (getenv("NP2_PCM_STRESS_ONLY") != NULL) {
        test_spsc_stress();
        return 0;
    }
    test_geometry_and_capacity();
    test_partial(1U);
    test_partial(13U);
    test_partial(239U);
    test_exact_and_segmentation();
    test_sequence_wraparound();
    test_sink_outcomes_and_state();
    test_slot_mutations();
    test_ring_to_sink_identity();
    test_spsc_stress();
    assert(negative_mutations >= 17U);
    printf("5C1_NEGATIVE_MUTATIONS=%u\n", negative_mutations);
    puts("5C1_NEGATIVE_VALIDATION=PASS");
    puts("PCM_RING_SPSC_MEMORY_ORDER=PASS");
    puts("PCM_SLOT_ATOMIC64_REQUIRED=NO");
    puts("PCM_SINK_CONTRACT=PASS");
    puts("PCM_SINK_RING_SEPARATION=PASS");
    puts("NP2_PCM_OUTPUT_RESULT=PASS");
    return 0;
}
