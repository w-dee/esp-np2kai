#ifndef NP2_PCM_OUTPUT_H
#define NP2_PCM_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np2opngen_pcm_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

enum np2_pcm_sink_result {
    NP2_PCM_SINK_ACCEPTED = 0,
    NP2_PCM_SINK_RETRY,
    NP2_PCM_SINK_FATAL,
};

struct np2_pcm_sink_view {
    const uint8_t *pcm;
    uint64_t frame_offset;
    uint32_t sequence;
    uint16_t valid_frames;
    uint16_t flags;
};

struct np2_pcm_sink {
    void *opaque;
    enum np2_pcm_sink_result (*start)(void *opaque);
    enum np2_pcm_sink_result (*submit)(
        void *opaque, const struct np2_pcm_sink_view *view);
    enum np2_pcm_sink_result (*finish)(void *opaque);
    enum np2_pcm_sink_result (*abort)(void *opaque);
};

enum np2_pcm_output_state {
    NP2_PCM_OUTPUT_INITIAL = 0,
    NP2_PCM_OUTPUT_STARTED,
    NP2_PCM_OUTPUT_FAILED,
    NP2_PCM_OUTPUT_FINISHED,
    NP2_PCM_OUTPUT_ABORTED,
};

enum np2_pcm_output_status {
    NP2_PCM_OUTPUT_OK = 0,
    NP2_PCM_OUTPUT_CONSUMED,
    NP2_PCM_OUTPUT_EMPTY,
    NP2_PCM_OUTPUT_RETRY,
    NP2_PCM_OUTPUT_FATAL,
    NP2_PCM_OUTPUT_ARGUMENT,
    NP2_PCM_OUTPUT_STATE_ERROR,
    NP2_PCM_OUTPUT_SLOT_ERROR,
    NP2_PCM_OUTPUT_RING_ERROR,
};

struct np2_pcm_output_controller {
    struct np2opngen_pcm_ring *ring;
    struct np2_pcm_sink sink;
    enum np2_pcm_output_state state;
    uint64_t expected_frame_offset;
    uint64_t accepted_frames;
    uint64_t accepted_bytes;
    uint32_t expected_sequence;
};

int np2_pcm_output_controller_init(
    struct np2_pcm_output_controller *controller,
    struct np2opngen_pcm_ring *ring,
    const struct np2_pcm_sink *sink);

enum np2_pcm_output_status np2_pcm_output_start(
    struct np2_pcm_output_controller *controller);

enum np2_pcm_output_status np2_pcm_output_step(
    struct np2_pcm_output_controller *controller);

enum np2_pcm_output_status np2_pcm_output_finish(
    struct np2_pcm_output_controller *controller);

enum np2_pcm_output_status np2_pcm_output_abort(
    struct np2_pcm_output_controller *controller);

#ifdef __cplusplus
}
#endif

#endif /* NP2_PCM_OUTPUT_H */
