/*
 * SPDX-FileCopyrightText: 2026 esp-np2kai contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef P4_NANO_AUDIO86_PHYSICAL_SINK_H
#define P4_NANO_AUDIO86_PHYSICAL_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np2pcm_output.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4_NANO_AUDIO86_PHYSICAL_FRAMES_PER_UNIT 240U
#define P4_NANO_AUDIO86_PHYSICAL_BYTES_PER_FRAME 4U
#define P4_NANO_AUDIO86_PHYSICAL_UNIT_BYTES 960U
#define P4_NANO_AUDIO86_PHYSICAL_DMA_DESCRIPTORS 4U
#define P4_NANO_AUDIO86_PHYSICAL_DRAIN_TIMEOUT_MS 40U

struct p4_nano_audio86_physical_sink;
struct p4_nano_audio86_callback_gate;

enum p4_nano_audio86_consumer_service_phase {
    P4_NANO_AUDIO86_CONSUMER_PHASE_NONE = 0,
    P4_NANO_AUDIO86_CONSUMER_PHASE_START_ENABLE,
    P4_NANO_AUDIO86_CONSUMER_PHASE_CODEC_UNMUTE,
    P4_NANO_AUDIO86_CONSUMER_PHASE_DOWNSTREAM_SUBMIT,
    P4_NANO_AUDIO86_CONSUMER_PHASE_POST_ACCEPT_EVIDENCE,
    P4_NANO_AUDIO86_CONSUMER_PHASE_WAIT_EOF,
    P4_NANO_AUDIO86_CONSUMER_PHASE_FINISH,
};

/* Task-owned reason for the PCM consumer's current scheduling state.  This is
 * deliberately separate from service phase: phase describes the last service
 * operation, while wait reason describes why the task is about to block. */
enum p4_nano_audio86_consumer_wait_reason {
    P4_NANO_AUDIO86_CONSUMER_WAIT_RUNNABLE = 0,
    P4_NANO_AUDIO86_CONSUMER_WAIT_RETRY_EOF,
    P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_RING_EMPTY,
    P4_NANO_AUDIO86_CONSUMER_WAIT_PCM_PREFILL,
    P4_NANO_AUDIO86_CONSUMER_WAIT_FINISH_OR_TERMINAL,
};

enum p4_nano_audio86_notify_result {
    P4_NANO_AUDIO86_NOTIFY_NONE = 0,
    P4_NANO_AUDIO86_NOTIFY_ATTEMPTED = 1U << 0,
    P4_NANO_AUDIO86_NOTIFY_HIGHER_PRIORITY_WOKEN = 1U << 1,
};

enum p4_nano_audio86_consumer_progress_point {
    P4_NANO_AUDIO86_PROGRESS_PUBLISH_ONLY = 0,
    P4_NANO_AUDIO86_PROGRESS_STEP_ENTER,
    P4_NANO_AUDIO86_PROGRESS_SUBMIT_RETURN,
    P4_NANO_AUDIO86_PROGRESS_STEP_EXIT,
    P4_NANO_AUDIO86_PROGRESS_RUNNING_ACCEPTED,
};

enum p4_nano_audio86_physical_io_result {
    P4_NANO_AUDIO86_PHYSICAL_IO_OK = 0,
    P4_NANO_AUDIO86_PHYSICAL_IO_TIMEOUT,
    P4_NANO_AUDIO86_PHYSICAL_IO_ERROR,
};

/* Internal board/backend seam.  It models ESP-IDF's exact copy result: an
 * operation reports both its status and the number of bytes already copied. */
struct p4_nano_audio86_physical_backend {
    int (*prepare)(void *opaque,
                   struct p4_nano_audio86_callback_gate *callback_gate,
                   uint32_t generation);
    enum p4_nano_audio86_physical_io_result (*preload)(
        void *opaque, const uint8_t *pcm, size_t bytes, size_t *bytes_loaded);
    int (*enable_stream)(void *opaque);
    void (*get_startup_durations)(void *opaque,
                                  uint32_t *enable_stream_us,
                                  uint32_t *codec_unmute_us);
    enum p4_nano_audio86_physical_io_result (*write)(
        void *opaque, const uint8_t *pcm, size_t bytes,
        size_t *bytes_written, uint32_t timeout_ms);
    int (*mute)(void *opaque);
    int (*pa_low)(void *opaque);
    int (*disable)(void *opaque);
    int (*unregister_callbacks)(void *opaque);
    uint64_t (*now_ms)(void *opaque);
    void (*wait_hint)(void *opaque, uint32_t timeout_ms);
    uint32_t (*notify_waiter)(void *opaque, bool from_isr);
    void (*release)(void *opaque);
    void *opaque;
};

enum p4_nano_audio86_physical_state {
    P4_NANO_AUDIO86_PHYSICAL_INITIAL = 0,
    P4_NANO_AUDIO86_PHYSICAL_PREPARED_ACCEPTING,
    P4_NANO_AUDIO86_PHYSICAL_STARTING,
    P4_NANO_AUDIO86_PHYSICAL_RUNNING,
    P4_NANO_AUDIO86_PHYSICAL_DRAINING,
    P4_NANO_AUDIO86_PHYSICAL_FAILED,
    P4_NANO_AUDIO86_PHYSICAL_ABORTING,
    P4_NANO_AUDIO86_PHYSICAL_QUIESCENT,
};

struct p4_nano_audio86_physical_telemetry {
    uint64_t semantic_accepted_frames;
    uint64_t semantic_accepted_bytes;
    uint64_t physical_units_copied;
    uint64_t physical_bytes_copied;
    uint64_t physical_padding_frames;
    uint64_t submit_attempts;
    uint64_t retry_count;
    uint64_t drain_duration_ms;
    uint64_t accepted_pending_drain_frames;
    uint64_t physically_drained_frames;
    uint64_t physically_discarded_accepted_frames;
    uint32_t full_units;
    uint32_t final_partial_units;
    uint32_t final_valid_frames;
    uint32_t tx_eof_epoch;
    uint32_t drain_snapshot_epoch;
    uint32_t drain_completion_epoch;
    uint32_t quiescent_eof_epoch;
    uint32_t registered_generation;
    uint32_t generation;
    uint32_t stale_callback_count;
    uint32_t running_queue_overflow_count;
    uint32_t draining_queue_overflow_count;
    uint32_t callback_refcount;
    uint32_t preloaded_units;
    uint32_t enable_stream_duration_us;
    uint32_t codec_unmute_duration_us;
    uint32_t startup_durations_valid;
    uint32_t first_active_qovf_latched;
    uint32_t first_qovf_state;
    uint32_t first_qovf_eof_epoch;
    uint32_t first_qovf_phase;
    uint32_t first_qovf_current_sequence;
    uint32_t first_qovf_published_sequence;
    uint32_t first_qovf_last_step_enter_us;
    uint32_t first_qovf_last_submit_return_us;
    uint32_t first_qovf_last_step_exit_us;
    uint32_t first_qovf_last_running_accepted_us;
    uint32_t first_qovf_wait_reason;
    uint32_t first_qovf_consumer_next_sequence;
    uint32_t first_qovf_next_published_sequence;
    uint32_t first_qovf_ring_occupancy;
    uint32_t first_qovf_production_done;
    uint32_t first_qovf_rendered_frames;
    uint32_t first_qovf_eof_notify_count;
    uint32_t first_qovf_hpwoken_true_count;
    uint32_t first_qovf_retry_wait_enter_count;
    uint32_t first_qovf_retry_wait_resume_count;
    uint32_t first_qovf_ring_wait_enter_count;
    uint32_t first_qovf_ring_wait_resume_count;
    uint32_t first_qovf_last_wait_enter_us;
    uint32_t first_qovf_last_wait_resume_us;
    uint32_t first_qovf_last_resume_reason;
    uint32_t first_qovf_last_resume_sequence;
    uint32_t first_qovf_observed;
    uint32_t first_qovf_observed_us;
    enum p4_nano_audio86_physical_state state;
    bool prepare_completed;
    bool pa_initial_low;
    bool codec_initialized_muted;
    bool i2s_initialized;
    bool muted_warmup_completed;
    bool callbacks_registered;
    bool stream_started;
    bool codec_unmute_completed;
    bool finish_completed;
    bool codec_final_muted;
    bool pa_final_low;
    bool i2s_enabled;
    bool i2s_created;
    bool sticky_error;
    bool callbacks_active;
};

int p4_nano_audio86_physical_sink_create(
    struct p4_nano_audio86_physical_sink **out,
    const struct p4_nano_audio86_physical_backend *backend);

/* Destroy is fail-closed: it refuses to reclaim a live/non-quiescent sink. */
int p4_nano_audio86_physical_sink_destroy(
    struct p4_nano_audio86_physical_sink *sink);

struct np2_pcm_sink p4_nano_audio86_physical_sink_interface(
    struct p4_nano_audio86_physical_sink *sink);

uint32_t p4_nano_audio86_physical_sink_retry_snapshot(
    const struct p4_nano_audio86_physical_sink *sink);
bool p4_nano_audio86_physical_sink_retry_ready(
    const struct p4_nano_audio86_physical_sink *sink, uint32_t snapshot);

/* The callback gate is the ESP-IDF user_data object.  Its entry routine takes
 * in-flight ownership before following the reclaimable sink pointer. */
void p4_nano_audio86_callback_gate_on_sent(
    struct p4_nano_audio86_callback_gate *gate);
void p4_nano_audio86_callback_gate_on_send_q_ovf(
    struct p4_nano_audio86_callback_gate *gate);

/* Task-context diagnostics use a single 32-bit atomic service word for the
 * phase/current/published sequence tuple.  Relative timestamps are separate
 * 32-bit atomics.  The ISR never calls a timer API. */
void p4_nano_audio86_callback_gate_set_service_phase(
    struct p4_nano_audio86_callback_gate *gate,
    enum p4_nano_audio86_consumer_service_phase phase);
void p4_nano_audio86_physical_sink_publish_consumer_progress(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_progress_point point,
    enum p4_nano_audio86_consumer_service_phase phase,
    uint32_t current_sequence, uint32_t published_sequence,
    uint32_t relative_us);
void p4_nano_audio86_physical_sink_observe_first_qovf(
    struct p4_nano_audio86_physical_sink *sink, uint32_t relative_us);
void p4_nano_audio86_physical_sink_publish_ring_context(
    struct p4_nano_audio86_physical_sink *sink, uint32_t rendered_frames,
    uint32_t next_published_sequence, uint32_t occupancy,
    bool production_done);
void p4_nano_audio86_physical_sink_publish_wait_enter(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_wait_reason reason,
    uint32_t consumer_next_sequence, uint32_t relative_us);
void p4_nano_audio86_physical_sink_publish_wait_resume(
    struct p4_nano_audio86_physical_sink *sink,
    enum p4_nano_audio86_consumer_wait_reason reason,
    uint32_t consumer_next_sequence, uint32_t relative_us);
void p4_nano_audio86_physical_sink_publish_runnable(
    struct p4_nano_audio86_physical_sink *sink,
    uint32_t consumer_next_sequence);
size_t p4_nano_audio86_physical_sink_diagnostic_storage_bytes(void);

void p4_nano_audio86_physical_sink_get_telemetry(
    const struct p4_nano_audio86_physical_sink *sink,
    struct p4_nano_audio86_physical_telemetry *telemetry);

#if defined(P4_NANO_AUDIO86_PHYSICAL_SINK_TESTING)
void p4_nano_audio86_physical_sink_test_set_callback_refcount(
    struct p4_nano_audio86_physical_sink *sink, uint32_t count);
void p4_nano_audio86_physical_sink_test_on_sent_generation(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation);
void p4_nano_audio86_physical_sink_test_on_send_q_ovf_generation(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation);
bool p4_nano_audio86_physical_sink_test_callback_enter(
    struct p4_nano_audio86_physical_sink *sink, uint32_t generation);
void p4_nano_audio86_physical_sink_test_callback_exit(
    struct p4_nano_audio86_physical_sink *sink);
void p4_nano_audio86_physical_sink_test_disarm_callbacks(
    struct p4_nano_audio86_physical_sink *sink);
#endif

#ifdef __cplusplus
}
#endif

#endif /* P4_NANO_AUDIO86_PHYSICAL_SINK_H */
