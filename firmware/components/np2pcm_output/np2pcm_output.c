#include "np2pcm_output.h"

#include <limits.h>
#include <string.h>

static bool sink_result_valid(enum np2_pcm_sink_result result)
{
    return result == NP2_PCM_SINK_ACCEPTED ||
           result == NP2_PCM_SINK_RETRY ||
           result == NP2_PCM_SINK_FATAL;
}

static enum np2_pcm_output_status translate_result(
    struct np2_pcm_output_controller *controller,
    enum np2_pcm_sink_result result)
{
    if (!sink_result_valid(result)) {
        controller->state = NP2_PCM_OUTPUT_FAILED;
        return NP2_PCM_OUTPUT_FATAL;
    }
    if (result == NP2_PCM_SINK_RETRY) {
        return NP2_PCM_OUTPUT_RETRY;
    }
    if (result == NP2_PCM_SINK_FATAL) {
        controller->state = NP2_PCM_OUTPUT_FAILED;
        return NP2_PCM_OUTPUT_FATAL;
    }
    return NP2_PCM_OUTPUT_OK;
}

int np2_pcm_output_controller_init(
    struct np2_pcm_output_controller *controller,
    struct np2opngen_pcm_ring *ring,
    const struct np2_pcm_sink *sink)
{
    if (controller == NULL || ring == NULL || sink == NULL ||
        sink->start == NULL || sink->submit == NULL ||
        sink->finish == NULL || sink->abort == NULL) {
        return -1;
    }
    memset(controller, 0, sizeof(*controller));
    controller->ring = ring;
    controller->sink = *sink;
    controller->state = NP2_PCM_OUTPUT_INITIAL;
    return 0;
}

enum np2_pcm_output_status np2_pcm_output_start(
    struct np2_pcm_output_controller *controller)
{
    enum np2_pcm_output_status status;
    if (controller == NULL) {
        return NP2_PCM_OUTPUT_ARGUMENT;
    }
    if (controller->state != NP2_PCM_OUTPUT_INITIAL) {
        return NP2_PCM_OUTPUT_STATE_ERROR;
    }
    status = translate_result(controller,
                              controller->sink.start(controller->sink.opaque));
    if (status == NP2_PCM_OUTPUT_OK) {
        controller->state = NP2_PCM_OUTPUT_STARTED;
    }
    return status;
}

static bool slot_valid(const struct np2_pcm_output_controller *controller,
                       const struct np2opngen_pcm_ring_slot *slot)
{
    const bool full = slot->valid_frames ==
                      NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES;
    const bool partial = slot->valid_frames > 0U && !full;
    if ((!full && !partial) ||
        slot->valid_frames > NP2_OPNGEN_PCM_RING_QUANTUM_FRAMES ||
        slot->sequence != controller->expected_sequence ||
        slot->frame_offset != controller->expected_frame_offset) {
        return false;
    }
    if (full) {
        return slot->flags == 0U;
    }
    return slot->flags == NP2_OPNGEN_PCM_RING_FLAG_FINAL_PARTIAL;
}

enum np2_pcm_output_status np2_pcm_output_step(
    struct np2_pcm_output_controller *controller)
{
    const struct np2opngen_pcm_ring_slot *slot = NULL;
    struct np2_pcm_sink_view view;
    enum np2_pcm_sink_result result;
    int ring_status;
    uint64_t bytes;
    uint16_t valid_frames;

    if (controller == NULL) {
        return NP2_PCM_OUTPUT_ARGUMENT;
    }
    if (controller->state != NP2_PCM_OUTPUT_STARTED) {
        return NP2_PCM_OUTPUT_STATE_ERROR;
    }
    ring_status = np2opngen_pcm_ring_try_peek(controller->ring, &slot);
    if (ring_status == NP2_OPNGEN_PCM_RING_EMPTY) {
        return NP2_PCM_OUTPUT_EMPTY;
    }
    if (ring_status != NP2_OPNGEN_PCM_RING_OK || slot == NULL) {
        return NP2_PCM_OUTPUT_RING_ERROR;
    }
    if (!slot_valid(controller, slot)) {
        return NP2_PCM_OUTPUT_SLOT_ERROR;
    }
    view.pcm = slot->pcm;
    view.frame_offset = slot->frame_offset;
    view.sequence = slot->sequence;
    view.valid_frames = slot->valid_frames;
    view.flags = slot->flags;
    result = controller->sink.submit(controller->sink.opaque, &view);
    if (!sink_result_valid(result) || result == NP2_PCM_SINK_FATAL) {
        controller->state = NP2_PCM_OUTPUT_FAILED;
        return NP2_PCM_OUTPUT_FATAL;
    }
    if (result == NP2_PCM_SINK_RETRY) {
        return NP2_PCM_OUTPUT_RETRY;
    }
    valid_frames = slot->valid_frames;
    bytes = (uint64_t)valid_frames *
            NP2_OPNGEN_PCM_RING_BYTES_PER_FRAME;
    if (controller->accepted_frames > UINT64_MAX - valid_frames ||
        controller->accepted_bytes > UINT64_MAX - bytes) {
        controller->state = NP2_PCM_OUTPUT_FAILED;
        return NP2_PCM_OUTPUT_FATAL;
    }
    if (np2opngen_pcm_ring_consume(controller->ring) !=
        NP2_OPNGEN_PCM_RING_OK) {
        controller->state = NP2_PCM_OUTPUT_FAILED;
        return NP2_PCM_OUTPUT_RING_ERROR;
    }
    controller->expected_frame_offset += valid_frames;
    controller->accepted_frames += valid_frames;
    controller->accepted_bytes += bytes;
    controller->expected_sequence++;
    return NP2_PCM_OUTPUT_CONSUMED;
}

enum np2_pcm_output_status np2_pcm_output_finish(
    struct np2_pcm_output_controller *controller)
{
    enum np2_pcm_output_status status;
    if (controller == NULL) {
        return NP2_PCM_OUTPUT_ARGUMENT;
    }
    if (controller->state != NP2_PCM_OUTPUT_STARTED) {
        return NP2_PCM_OUTPUT_STATE_ERROR;
    }
    if (np2opngen_pcm_ring_occupancy(controller->ring) != 0U) {
        return NP2_PCM_OUTPUT_STATE_ERROR;
    }
    status = translate_result(controller,
                              controller->sink.finish(controller->sink.opaque));
    if (status == NP2_PCM_OUTPUT_OK) {
        controller->state = NP2_PCM_OUTPUT_FINISHED;
    }
    return status;
}

enum np2_pcm_output_status np2_pcm_output_abort(
    struct np2_pcm_output_controller *controller)
{
    enum np2_pcm_output_status status;
    if (controller == NULL) {
        return NP2_PCM_OUTPUT_ARGUMENT;
    }
    if (controller->state != NP2_PCM_OUTPUT_STARTED &&
        controller->state != NP2_PCM_OUTPUT_FAILED) {
        return NP2_PCM_OUTPUT_STATE_ERROR;
    }
    status = translate_result(controller,
                              controller->sink.abort(controller->sink.opaque));
    if (status == NP2_PCM_OUTPUT_OK) {
        controller->state = NP2_PCM_OUTPUT_ABORTED;
    }
    return status;
}
