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
    enum p4_nano_audio86_physical_io_result (*write)(
        void *opaque, const uint8_t *pcm, size_t bytes,
        size_t *bytes_written, uint32_t timeout_ms);
    int (*mute)(void *opaque);
    int (*pa_low)(void *opaque);
    int (*disable)(void *opaque);
    int (*unregister_callbacks)(void *opaque);
    uint64_t (*now_ms)(void *opaque);
    void (*wait_hint)(void *opaque, uint32_t timeout_ms);
    void (*notify_waiter)(void *opaque, bool from_isr);
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
    uint64_t accepted_pending_drain_frames;
    uint64_t physically_drained_frames;
    uint64_t physically_discarded_accepted_frames;
    uint32_t tx_eof_epoch;
    uint32_t drain_snapshot_epoch;
    uint32_t drain_completion_epoch;
    uint32_t generation;
    uint32_t stale_callback_count;
    uint32_t running_queue_overflow_count;
    uint32_t draining_queue_overflow_count;
    uint32_t callback_refcount;
    uint32_t preloaded_units;
    enum p4_nano_audio86_physical_state state;
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
