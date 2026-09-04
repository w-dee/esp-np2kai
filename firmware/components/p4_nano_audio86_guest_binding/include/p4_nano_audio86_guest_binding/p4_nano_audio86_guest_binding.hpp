#pragma once

#include <cstdint>
#include <climits>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p4_nano_audio86_guest_binding/p4_nano_audio86_owner_progress_diagnostics.hpp"

namespace np2runtime {
class Runtime;
}

namespace p4_nano_audio86_guest_binding {

enum class OwnerPhase : std::uint32_t {
    Created = 0U,
    RuntimeInit,
    Ready,
    ServiceInit,
    FixtureConfigure,
    ServiceStart,
    GuestAttach,
    GuestExec,
    TerminalArm,
    ResetWait,
    ProducerComplete,
    ServiceJoin,
    ServiceDestroy,
    RuntimeCleanup,
    Complete,
    Parked,
};

struct TimeoutDiagnosticSnapshot {
    std::uint32_t owner_phase;
    std::uint32_t service_observable;
    std::uint32_t snapshot_coherent;
    std::uint32_t live_service_state;
    std::uint32_t failure_category;
    std::uint32_t failure_origin;
    std::uint32_t failure_subcode;
    std::uint32_t failure_sequence;
    std::uint32_t cleanup_state;
    std::uint64_t guest_authoritative_frame;
    std::uint64_t latest_published_horizon;
    std::uint64_t rendered_frames;
    std::uint64_t accepted_frames;
    std::uint32_t producer_done;
    std::uint32_t guest_attached;
    std::uint32_t sink_reachable;
    std::uint32_t event_ring_occupancy;
    std::uint32_t byte_ring_occupancy;
    std::uint32_t q240_occupancy;
    std::uint32_t q240_produced;
    std::uint32_t q240_submitted;
    std::uint32_t output_state;
    std::uint32_t worker_wait_reason;
    std::uint32_t reset_seen;
    std::uint32_t reset_ordinal;
    std::uint32_t reset_ack;
    std::uint32_t terminal_armed;
    std::uint32_t terminal_horizon_published;
    std::uint32_t terminal_horizon_observed;
    std::uint32_t terminal_pcm_ready;
    std::uint32_t physical_sink_state;
    std::uint32_t physical_sticky_error;
    std::uint32_t physical_qovf;
    std::uint32_t callback_active;
    std::uint32_t callback_in_flight;
    OwnerProgressSnapshot owner_progress;
    std::uint32_t transaction_active;
    std::uint32_t reserved_event_slots;
    std::uint32_t reserved_byte_count;
    std::uint32_t horizon_owned;
    std::uint32_t horizon_mailbox_state;
    std::uint32_t transaction_waiting;
    std::uint32_t progress_checkpoint_retrying;
    std::uint32_t current_checkpoint_retry_count;
    std::uint32_t max_checkpoint_retry_count;
    std::uint32_t checkpoint_retry_count;
};

constexpr std::uint32_t kTimeoutDiagnosticUnavailable = UINT32_MAX;

void timeout_diagnostic_reset() noexcept;
void publish_owner_phase(OwnerPhase phase) noexcept;
TimeoutDiagnosticSnapshot timeout_diagnostic_snapshot() noexcept;
int timeout_request_async_stop() noexcept;
void mark_outer_timeout() noexcept;
bool outer_timeout_latched() noexcept;

/* Runs only on the existing p4_nano_pc98 owner task.  It never creates a
 * producer task: Core 1 guest ownership remains with that task. */
esp_err_t run_on_pc98_task(TaskHandle_t producer,
                           np2runtime::Runtime *lifecycle_runtime) noexcept;

} // namespace p4_nano_audio86_guest_binding
