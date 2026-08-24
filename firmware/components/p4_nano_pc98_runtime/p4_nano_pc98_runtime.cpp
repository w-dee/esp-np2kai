#include "p4_nano_pc98_runtime/p4_nano_pc98_runtime.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
#include <compiler.h>
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
#include <cpumem.h>
#endif
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
#include <result_v1_parser.h>
#endif
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
#include <np2kbd_control_v1_parser.h>
#include <np2kbd_result_v1_parser.h>
#endif
#include <diskimage/fddfile.h>
#include <scrnmng.h>
}

#include "np2host/dosio_esp.h"
#include "np2_keyboard_input_bridge/keyboard_input_bridge.hpp"
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
#include "np2_keyboard_input/keyboard_input.hpp"
#include "np2_keyboard_validation/validation_controller.hpp"
#endif
#include "np2runtime/np2runtime.hpp"
#include "p4_nano_live_display_session/session.hpp"
#include "p4_nano_pc98_runtime/runtime_contract.hpp"
#include "storage_fatfs/storage_fatfs.hpp"
#include "storage_sdmmc/storage_sdmmc.hpp"

namespace {

constexpr std::size_t kRuntimeStackBytes = 32768U;
constexpr UBaseType_t kRuntimePriority = tskIDLE_PRIORITY + 3U;
constexpr BaseType_t kRuntimeCore = 1;
constexpr TickType_t kStartupTimeoutTicks = pdMS_TO_TICKS(30000U) == 0U
                                                 ? 1U
                                                 : pdMS_TO_TICKS(30000U);
constexpr TickType_t kConsumerDelayTicks = 1U;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
constexpr std::uintptr_t kResultPhysicalAddress = 0x29000U;
#endif
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
constexpr std::uintptr_t kControlPhysicalAddress = 0x27fc0U;
#endif

enum class ValidationKind : std::uint8_t {
    None,
    Runtime,
    Keyboard,
};

enum class GuestCompletion : std::uint8_t {
    Unknown,
    Pass,
    Fail,
};

constexpr p4_nano_pc98_runtime::MediaConfig media_config_for(
    const ValidationKind validation_kind, const bool emu_backend) noexcept
{
    if (validation_kind == ValidationKind::Runtime) {
        return emu_backend ? p4_nano_pc98_runtime::validation_media_config()
                           : p4_nano_pc98_runtime::hardware_validation_media_config();
    }
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    if (validation_kind == ValidationKind::Keyboard) {
        return emu_backend
                   ? p4_nano_pc98_runtime::keyboard_validation_media_config()
                   : p4_nano_pc98_runtime::hardware_keyboard_validation_media_config();
    }
#else
    (void)emu_backend;
#endif
    return p4_nano_pc98_runtime::production_media_config();
}

struct Composition final {
    explicit Composition(const ValidationKind validation_kind,
                         const bool emu_backend) noexcept
        : validation(validation_kind != ValidationKind::None),
          kind(validation_kind),
          emu(emu_backend),
          media(media_config_for(validation_kind, emu_backend))
    {
    }

    bool validation;
    ValidationKind kind;
    bool emu;
    p4_nano_pc98_runtime::MediaConfig media;
    storage_sdmmc::SdmmcMountProvider sd_provider{};
    storage_fatfs::MountProvider persist_provider{};
    storage_fatfs::FatfsMountBackend *mount_backend = nullptr;
    p4_nano_live_display_session::Session session{};
    np2_keyboard_input_bridge::KeyboardInputBridge keyboard{};
    np2runtime::Runtime runtime{};
    SemaphoreHandle_t ready_semaphore = nullptr;
    SemaphoreHandle_t stopped_semaphore = nullptr;
    TaskHandle_t owner_task = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_done{false};
    std::atomic<GuestCompletion> guest_completion{GuestCompletion::Unknown};
    esp_err_t owner_result = ESP_FAIL;
    bool mounted = false;
    bool scrnmng_initialized = false;
    bool dosio_attached = false;
    bool fdd_attached = false;
    bool ready_signaled = false;
    bool keyboard_status_reported = false;
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    np2_keyboard_validation::ValidationController keyboard_validation{};
    np2kbd_control_v1_tracker control_tracker{};
    np2kbd_control_v1_state accepted_control_state =
        NP2KBD_CONTROL_V1_STATE_UNINITIALIZED;
    np2kbd_control_v1_result accepted_control{};
    np2_keyboard_validation::EnqueueOutcome pending_enqueue_outcome =
        np2_keyboard_validation::EnqueueOutcome::None;
    np2_keyboard_validation::CounterSnapshot proof_counters{};
    bool keyboard_ready_reported = false;
    bool keyboard_proof_terminal_reported = false;
#endif
};

void emit(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
void observe_guest_completion(Composition *composition) noexcept
{
    if (composition == nullptr || !composition->validation) {
        return;
    }

    /* The guest publishes CRC-protected result-v1 with state last.  A local
     * snapshot prevents the parser from observing a moving memory window;
     * the parser's CRC and reserved-field checks reject torn snapshots. */
    std::uint8_t snapshot[NP2_RESULT_V1_SIZE];
    std::memcpy(snapshot, mem + kResultPhysicalAddress, sizeof(snapshot));
    np2_result_v1_result parsed{};
    const np2_result_v1_observation observation = np2_result_v1_parse(
        snapshot, sizeof(snapshot), &parsed);
    GuestCompletion completion = GuestCompletion::Unknown;
    if (observation == NP2_RESULT_V1_PASS) {
        completion = GuestCompletion::Pass;
    } else if (observation == NP2_RESULT_V1_FAIL) {
        completion = GuestCompletion::Fail;
    }
    if (completion == GuestCompletion::Unknown) {
        return;
    }
    if (composition->guest_completion.exchange(completion,
                                               std::memory_order_acq_rel) ==
        completion) {
        return;
    }
    emit("P4_NANO_RUNTIME_GUEST_COMPLETION=%s completed=%u passed=%u "
         "failed=%u\n",
         completion == GuestCompletion::Pass ? "PASS" : "FAIL",
         static_cast<unsigned>(parsed.completed_count),
         static_cast<unsigned>(parsed.passed_count),
         static_cast<unsigned>(parsed.failed_count));
}
#endif

#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
np2_keyboard_validation::ControlState keyboard_control_state(
    const np2kbd_control_v1_state state) noexcept
{
    switch (state) {
    case NP2KBD_CONTROL_V1_STATE_READY:
        return np2_keyboard_validation::ControlState::Ready;
    case NP2KBD_CONTROL_V1_STATE_MAKE_OBSERVED:
        return np2_keyboard_validation::ControlState::MakeObserved;
    case NP2KBD_CONTROL_V1_STATE_BREAK_OBSERVED:
        return np2_keyboard_validation::ControlState::BreakObserved;
    case NP2KBD_CONTROL_V1_STATE_FAIL:
        return np2_keyboard_validation::ControlState::Fail;
    case NP2KBD_CONTROL_V1_STATE_UNINITIALIZED:
        return np2_keyboard_validation::ControlState::Uninitialized;
    }
    return np2_keyboard_validation::ControlState::Uninitialized;
}

np2_keyboard_validation::ResultObservation keyboard_result_observation(
    const np2kbd_result_v1_observation observation) noexcept
{
    switch (observation) {
    case NP2KBD_RESULT_V1_PRE_PROTOCOL:
        return np2_keyboard_validation::ResultObservation::PreProtocol;
    case NP2KBD_RESULT_V1_UNINITIALIZED:
        return np2_keyboard_validation::ResultObservation::Uninitialized;
    case NP2KBD_RESULT_V1_RUNNING:
        return np2_keyboard_validation::ResultObservation::Running;
    case NP2KBD_RESULT_V1_PASS:
        return np2_keyboard_validation::ResultObservation::Pass;
    case NP2KBD_RESULT_V1_FAIL:
        return np2_keyboard_validation::ResultObservation::Fail;
    case NP2KBD_RESULT_V1_INVALID:
        return np2_keyboard_validation::ResultObservation::Invalid;
    }
    return np2_keyboard_validation::ResultObservation::Invalid;
}

np2_keyboard_validation::CounterSnapshot keyboard_counters(
    const np2_keyboard_input_bridge::BridgeCounters &counters) noexcept
{
    return {counters.enqueued,
            counters.dequeued,
            counters.queue_overflows,
            counters.queue_rejected,
            counters.blocked_events,
            counters.recovery_discards,
            counters.ownership.press_injected,
            counters.ownership.release_injected,
            counters.ownership.duplicate_suppressed,
            counters.ownership.invalid_rejected,
            counters.ownership.source_capacity_failures,
            counters.ownership.source_disconnects,
            counters.ownership.global_recoveries,
            false};
}

bool keyboard_proof_counters_valid(const Composition *composition) noexcept
{
    if (composition == nullptr) {
        return false;
    }
    const auto &baseline = composition->keyboard_validation.baseline();
    const auto &proof = composition->proof_counters;
    const auto delta = [](const std::uint32_t current,
                          const std::uint32_t before,
                          const std::uint32_t expected) noexcept {
        return current >= before && current - before == expected;
    };
    return delta(proof.enqueued, baseline.enqueued, 2U) &&
           delta(proof.dequeued, baseline.dequeued, 2U) &&
           delta(proof.press_injected, baseline.press_injected, 1U) &&
           delta(proof.release_injected, baseline.release_injected, 1U) &&
           proof.queue_overflows == baseline.queue_overflows &&
           proof.queue_rejected == baseline.queue_rejected &&
           proof.blocked_events == baseline.blocked_events &&
           proof.recovery_discards == baseline.recovery_discards &&
           proof.duplicate_suppressed == baseline.duplicate_suppressed &&
           proof.invalid_rejected == baseline.invalid_rejected &&
           proof.source_capacity_failures ==
               baseline.source_capacity_failures &&
           proof.source_disconnects == baseline.source_disconnects &&
           proof.global_recoveries == baseline.global_recoveries &&
           !proof.quarantined;
}

void snapshot_keyboard_protocol(
    Composition *composition,
    np2_keyboard_validation::InputSnapshot *sample) noexcept
{
    if (composition == nullptr || sample == nullptr) {
        return;
    }

    std::uint8_t control_snapshot[NP2KBD_CONTROL_V1_SIZE];
    std::memcpy(control_snapshot, mem + kControlPhysicalAddress,
                sizeof(control_snapshot));
    const auto tracked = np2kbd_control_v1_tracker_observe(
        &composition->control_tracker, control_snapshot,
        sizeof(control_snapshot));
    if (tracked == NP2KBD_CONTROL_V1_TRACK_INVALID) {
        sample->control_observation =
            np2_keyboard_validation::ControlObservation::Invalid;
    } else if (tracked == NP2KBD_CONTROL_V1_TRACK_TRANSIENT) {
        sample->control_observation =
            np2_keyboard_validation::ControlObservation::Transient;
    } else {
        np2kbd_control_v1_result parsed{};
        const auto parsed_observation = np2kbd_control_v1_parse(
            control_snapshot, sizeof(control_snapshot), &parsed);
        if (parsed_observation == NP2KBD_CONTROL_V1_INVALID ||
            parsed_observation == NP2KBD_CONTROL_V1_TRANSIENT ||
            parsed_observation == NP2KBD_CONTROL_V1_PRE_PROTOCOL ||
            parsed_observation == NP2KBD_CONTROL_V1_UNINITIALIZED) {
            sample->control_observation =
                np2_keyboard_validation::ControlObservation::Invalid;
        } else {
            composition->accepted_control_state = parsed.state;
            composition->accepted_control = parsed;
            sample->control_observation =
                np2_keyboard_validation::ControlObservation::Accepted;
        }
    }
    sample->control_state = keyboard_control_state(
        composition->accepted_control_state);
    sample->observed_make = composition->accepted_control.observed_make;
    sample->observed_break = composition->accepted_control.observed_break;
    sample->failure_reason = composition->accepted_control.failure_reason;

    std::uint8_t result_snapshot[NP2KBD_RESULT_V1_SIZE];
    std::memcpy(result_snapshot, mem + kResultPhysicalAddress,
                sizeof(result_snapshot));
    np2kbd_result_v1_result result{};
    const auto result_observation = np2kbd_result_v1_parse(
        result_snapshot, sizeof(result_snapshot), &result);
    sample->result_observation =
        keyboard_result_observation(result_observation);
}

void emit_keyboard_proof_failure(Composition *composition,
                                const np2_keyboard_validation::StepResult &step)
{
    if (composition == nullptr ||
        composition->keyboard_proof_terminal_reported) {
        return;
    }
    composition->keyboard_proof_terminal_reported = true;
    emit("P4_NANO_KEYBOARD_PROOF_RESULT=FAIL reason=%s\n",
         np2_keyboard_validation::to_string(step.failure_reason));
}

bool owner_iteration_keyboard(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return false;
    }
    if (composition->stop_requested.load(std::memory_order_acquire)) {
        if (composition->keyboard_validation.state() !=
                np2_keyboard_validation::State::Complete &&
            composition->keyboard_validation.state() !=
                np2_keyboard_validation::State::Failed) {
            const auto failure = composition->keyboard_validation.fail(
                np2_keyboard_validation::FailureReason::GlobalTimeout);
            emit_keyboard_proof_failure(composition, failure);
        }
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }
    if (composition->runtime.failure()) {
        const auto failure = composition->keyboard_validation.fail(
            np2_keyboard_validation::FailureReason::RuntimeFatal);
        emit_keyboard_proof_failure(composition, failure);
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }

    /* This callback is the owner-task boundary.  The protocol snapshots are
     * copied before draining, and a command created below is therefore
     * necessarily drained on the next owner iteration. */
    np2_keyboard_validation::InputSnapshot sample{};
    sample.now_us = static_cast<std::uint64_t>(esp_timer_get_time());
    sample.enqueue_outcome = composition->pending_enqueue_outcome;
    composition->pending_enqueue_outcome =
        np2_keyboard_validation::EnqueueOutcome::None;
    snapshot_keyboard_protocol(composition, &sample);

    /* A committed guest terminal failure is authoritative before the bridge
     * drain.  This keeps the owner-iteration sequence deterministic even if
     * a stale producer command is still waiting in the bounded queue. */
    np2_keyboard_validation::FailureReason pre_drain_failure =
        np2_keyboard_validation::FailureReason::None;
    if (sample.control_observation ==
        np2_keyboard_validation::ControlObservation::Invalid) {
        pre_drain_failure =
            np2_keyboard_validation::FailureReason::ControlInvalid;
    } else if (sample.control_observation ==
                   np2_keyboard_validation::ControlObservation::Accepted &&
               sample.control_state ==
                   np2_keyboard_validation::ControlState::Fail) {
        pre_drain_failure =
            np2_keyboard_validation::FailureReason::GuestControlFail;
    } else if (sample.result_observation ==
               np2_keyboard_validation::ResultObservation::Invalid) {
        pre_drain_failure = np2_keyboard_validation::FailureReason::ResultInvalid;
    } else if (sample.result_observation ==
               np2_keyboard_validation::ResultObservation::Fail) {
        pre_drain_failure =
            np2_keyboard_validation::FailureReason::GuestResultFail;
    }
    if (pre_drain_failure != np2_keyboard_validation::FailureReason::None) {
        const auto failure =
            composition->keyboard_validation.fail(pre_drain_failure);
        emit_keyboard_proof_failure(composition, failure);
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }

    const auto input_result = composition->keyboard.owner_iteration();
    if (input_result ==
            np2_keyboard_input_bridge::OwnerIterationResult::Recovered ||
        input_result ==
            np2_keyboard_input_bridge::OwnerIterationResult::Quarantined) {
        if (!composition->keyboard_status_reported) {
            composition->keyboard_status_reported = true;
            const auto counters = composition->keyboard.counters();
            emit("P4_NANO_KEYBOARD=QUARANTINED queue_overflows=%" PRIu32
                 " recoveries=%" PRIu32 "\n",
                 counters.queue_overflows,
                 counters.ownership.global_recoveries);
        }
    }
    const auto bridge_counters = composition->keyboard.counters();
    sample.counters = keyboard_counters(bridge_counters);
    sample.counters.quarantined = composition->keyboard.quarantined();

    const auto before = composition->keyboard_validation.state();
    const auto step = composition->keyboard_validation.observe(sample);
    if (sample.control_observation ==
            np2_keyboard_validation::ControlObservation::Accepted &&
        sample.control_state == np2_keyboard_validation::ControlState::Ready &&
        !composition->keyboard_ready_reported) {
        composition->keyboard_ready_reported = true;
        emit("P4_NANO_KEYBOARD_PROOF_STATE=READY\n");
    }
    if (before == np2_keyboard_validation::State::PressQueued &&
        step.state == np2_keyboard_validation::State::WaitingMake) {
        emit("P4_NANO_KEYBOARD_PROOF_EVENT=PRESS_DRAINED\n");
    }
    if (before == np2_keyboard_validation::State::ReleaseQueued &&
        step.state == np2_keyboard_validation::State::WaitingBreak) {
        emit("P4_NANO_KEYBOARD_PROOF_EVENT=RELEASE_DRAINED\n");
    }

    if (step.action == np2_keyboard_validation::Action::EnqueuePress ||
        step.action == np2_keyboard_validation::Action::EnqueueRelease) {
        if (step.action == np2_keyboard_validation::Action::EnqueueRelease) {
            emit("P4_NANO_KEYBOARD_PROOF_STATE=MAKE_OBSERVED byte=0x%02x\n",
                 sample.observed_make);
        }
        const np2_keyboard_input::Event event{
            np2_keyboard_input::kSyntheticSource,
            np2_keyboard_input::Key::A,
            step.action == np2_keyboard_validation::Action::EnqueuePress
                ? np2_keyboard_input::Action::Press
                : np2_keyboard_input::Action::Release};
        const auto enqueue_result = composition->keyboard.enqueue(event);
        if (enqueue_result == np2_keyboard_input_bridge::EnqueueResult::Enqueued) {
            composition->pending_enqueue_outcome =
                np2_keyboard_validation::EnqueueOutcome::Enqueued;
            emit("P4_NANO_KEYBOARD_PROOF_EVENT=%s\n",
                 step.action == np2_keyboard_validation::Action::EnqueuePress
                     ? "PRESS_ENQUEUED"
                     : "RELEASE_ENQUEUED");
        } else {
            composition->pending_enqueue_outcome =
                np2_keyboard_validation::EnqueueOutcome::Failed;
        }
    }

    if (before == np2_keyboard_validation::State::WaitingBreak &&
        step.state == np2_keyboard_validation::State::WaitingResult) {
        emit("P4_NANO_KEYBOARD_PROOF_STATE=BREAK_OBSERVED byte=0x%02x\n",
             sample.observed_break);
    }
    if (step.action == np2_keyboard_validation::Action::Complete) {
        composition->proof_counters =
            composition->keyboard_validation.proof_counters();
        emit("P4_NANO_KEYBOARD_PROOF_COUNTERS enqueued=%" PRIu32
             " dequeued=%" PRIu32 " press=%" PRIu32
             " release=%" PRIu32 " queue_overflows=%" PRIu32
             " rejected=%" PRIu32 " blocked=%" PRIu32
             " duplicate=%" PRIu32 " invalid=%" PRIu32
             " source_capacity=%" PRIu32 " recoveries=%" PRIu32
             " quarantined=%u\n",
             composition->proof_counters.enqueued,
             composition->proof_counters.dequeued,
             composition->proof_counters.press_injected,
             composition->proof_counters.release_injected,
             composition->proof_counters.queue_overflows,
             composition->proof_counters.queue_rejected,
             composition->proof_counters.blocked_events,
             composition->proof_counters.duplicate_suppressed,
             composition->proof_counters.invalid_rejected,
             composition->proof_counters.source_capacity_failures,
             composition->proof_counters.global_recoveries,
             composition->proof_counters.quarantined ? 1U : 0U);
        emit("P4_NANO_KEYBOARD_PROOF_RESULT=PASS make=0x%02x break=0x%02x\n",
             sample.observed_make, sample.observed_break);
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }
    if (step.action == np2_keyboard_validation::Action::Fail) {
        emit_keyboard_proof_failure(composition, step);
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }
    return false;
}
#endif

bool owner_iteration(void *context) noexcept
{
    auto *composition = static_cast<Composition *>(context);
    if (composition == nullptr) {
        return false;
    }
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    return owner_iteration_keyboard(composition);
#else
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    observe_guest_completion(composition);
    const GuestCompletion guest_completion =
        composition->guest_completion.load(std::memory_order_acquire);
    const bool guest_terminal =
        guest_completion != GuestCompletion::Unknown;
#else
    constexpr bool guest_terminal = false;
#endif
    if (composition->stop_requested.load(std::memory_order_acquire) ||
        guest_terminal || composition->runtime.failure()) {
        /* Runtime invokes this boundary before pccore_term(), including its
         * fatal cleanup path.  Release keyboard state before core cleanup,
         * then detach display publication and request the stop. */
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
        return true;
    }

    const auto input_result = composition->keyboard.owner_iteration();
    if ((input_result ==
             np2_keyboard_input_bridge::OwnerIterationResult::Recovered ||
         input_result ==
             np2_keyboard_input_bridge::OwnerIterationResult::Quarantined) &&
        !composition->keyboard_status_reported) {
        composition->keyboard_status_reported = true;
        const auto counters = composition->keyboard.counters();
        emit("P4_NANO_KEYBOARD=QUARANTINED queue_overflows=%" PRIu32
             " recoveries=%" PRIu32 "\n",
             counters.queue_overflows,
             counters.ownership.global_recoveries);
    }
    return false;
#endif
}

void signal_ready(Composition *composition, esp_err_t result) noexcept
{
    if (composition == nullptr || composition->ready_signaled) {
        return;
    }
    composition->owner_result = result;
    composition->ready_signaled = true;
    if (composition->ready_semaphore != nullptr) {
        (void)xSemaphoreGive(composition->ready_semaphore);
    }
}

void cleanup_after_owner_join(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return;
    }

    /* Runtime's observer normally detaches the hook before stopping.  This
     * fallback covers initialization/fatal paths and is also the first step
     * after the owner task has joined. */
    composition->session.detach_source();

    /* The owner task has joined, and Runtime::run() has already returned, so
     * pccore_term() no longer uses the drive. */
    if (composition->fdd_attached) {
        (void)fdd_eject(composition->media.fdd_unit);
        composition->fdd_attached = false;
    }
    if (composition->scrnmng_initialized) {
        scrnmng_shutdown();
        composition->scrnmng_initialized = false;
    }
    if (composition->dosio_attached) {
        np2_dosio_detach_vfs_file();
        composition->dosio_attached = false;
    }
}

void stop_runtime_after_setup_failure(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return;
    }
    composition->keyboard.shutdown();
    (void)composition->runtime.request_stop();
    (void)composition->runtime.run();
}

void owner_task(void *context)
{
    auto *composition = static_cast<Composition *>(context);
    if (composition == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    const bool keyboard_initialized = composition->keyboard.initialize();
    const np2runtime::Result runtime_init =
        keyboard_initialized
            ? composition->runtime.initialize(
                  p4_nano_pc98_runtime::kFdd0OnlyEquipment)
            : np2runtime::Result::InitializationFailed;
    if (!keyboard_initialized) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=KEYBOARD_INIT_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        goto done;
    }
    if (runtime_init != np2runtime::Result::Ok) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=RUNTIME_INIT_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->keyboard.set_core_active(true);

    if (!scrnmng_initialize()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=SCRNMNG_INIT_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        stop_runtime_after_setup_failure(composition);
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->scrnmng_initialized = true;

    if (composition->session.attach_source() != ESP_OK) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_PUBLISH_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        stop_runtime_after_setup_failure(composition);
        composition->owner_result = ESP_FAIL;
        goto done;
    }

    if (!np2_dosio_attach_vfs_file(composition->media.logical_path.data(),
                                   composition->media.physical_path.data())) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DOSIO_MAP_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        stop_runtime_after_setup_failure(composition);
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->dosio_attached = true;
    np2_dosio_stats_reset();
    emit("P4_NANO_RUNTIME_DOSIO=READY logical=%s physical=%s\n",
         composition->media.logical_path.data(),
         composition->media.physical_path.data());

    if (fdd_set(composition->media.fdd_unit,
                composition->media.logical_path.data(), FTYPE_NONE, 1) !=
            SUCCESS ||
        !fdd_diskready(composition->media.fdd_unit)) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=FDD_ATTACH_FAILED\n");
        signal_ready(composition, ESP_FAIL);
        stop_runtime_after_setup_failure(composition);
        composition->owner_result = ESP_FAIL;
        goto done;
    }
    composition->fdd_attached = true;
    emit("P4_NANO_RUNTIME_FDD0=ATTACHED type=autodetect readonly=1 fddequip=0x%02x\n",
         p4_nano_pc98_runtime::kFdd0OnlyEquipment);

#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    /* Start keyboard proof deadlines only after the guest's complete runtime,
     * display, DOSIO, and FDD setup has succeeded.  The next owner iteration
     * is therefore the first interval in which the guest can publish READY. */
    composition->keyboard_validation.begin(
        keyboard_counters(composition->keyboard.counters()),
        static_cast<std::uint64_t>(esp_timer_get_time()));
    np2kbd_control_v1_tracker_init(&composition->control_tracker);
#endif

    signal_ready(composition, ESP_OK);
    emit("P4_NANO_RUNTIME_CORE=RUNNING\n");
    if (!composition->validation) {
        emit("P4_NANO_RUNTIME_RESULT=RUNNING\n");
    }
    (void)composition->runtime.run(owner_iteration, composition);
    composition->keyboard.set_core_active(false);
    if (composition->runtime.failure()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=RUNTIME_FATAL\n");
        composition->owner_result = ESP_FAIL;
    } else {
        composition->owner_result = ESP_OK;
    }

done:
    composition->keyboard.set_core_active(false);
    composition->keyboard.shutdown();
    {
        const auto counters = composition->keyboard.counters();
        emit("P4_NANO_KEYBOARD_COUNTERS enqueued=%" PRIu32
             " dequeued=%" PRIu32 " queue_overflows=%" PRIu32
             " rejected=%" PRIu32 " blocked=%" PRIu32
             " press=%" PRIu32 " release=%" PRIu32
             " duplicate=%" PRIu32 " invalid=%" PRIu32
             " disconnects=%" PRIu32 " source_capacity=%" PRIu32
             " recovery_discards=%" PRIu32 " recoveries=%" PRIu32
             " quarantined=%u\n",
             counters.enqueued, counters.dequeued, counters.queue_overflows,
             counters.queue_rejected, counters.blocked_events,
             counters.ownership.press_injected,
             counters.ownership.release_injected,
             counters.ownership.duplicate_suppressed,
             counters.ownership.invalid_rejected,
             counters.ownership.source_disconnects,
             counters.ownership.source_capacity_failures,
             counters.recovery_discards,
             counters.ownership.global_recoveries,
             composition->keyboard.quarantined() ? 1U : 0U);
    }
    composition->owner_done.store(true, std::memory_order_release);
    if (composition->stopped_semaphore != nullptr) {
        (void)xSemaphoreGive(composition->stopped_semaphore);
    }
    /* The caller joins this task after the stopped semaphore; blocking here
     * keeps the task alive without a spin until vTaskDelete() is issued. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelete(nullptr);
}

bool media_exists(const p4_nano_pc98_runtime::MediaConfig &media) noexcept
{
    struct stat status{};
    return stat(media.physical_path.data(), &status) == 0 &&
           S_ISREG(status.st_mode) && status.st_size > 0;
}

void destroy_sync(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return;
    }
    if (composition->ready_semaphore != nullptr) {
        vSemaphoreDelete(composition->ready_semaphore);
        composition->ready_semaphore = nullptr;
    }
    if (composition->stopped_semaphore != nullptr) {
        vSemaphoreDelete(composition->stopped_semaphore);
        composition->stopped_semaphore = nullptr;
    }
}

esp_err_t run_composition(const ValidationKind validation_kind,
                          const bool emu_backend) noexcept
{
    const bool validation_profile = validation_kind != ValidationKind::None;
    Composition composition(validation_kind, emu_backend);
    const auto &media = composition.media;

#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    if (validation_profile && !emu_backend) {
        if (composition.sd_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_SD_MOUNT=FAIL reason=SD_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.sd_provider;
    } else if (validation_profile) {
        if (composition.persist_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_RESULT=FAIL reason=SPI_NOR_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.persist_provider;
    } else
#else
    (void)validation_profile;
    (void)emu_backend;
#endif
    {
        if (composition.sd_provider.mount() != ESP_OK) {
            emit("P4_NANO_RUNTIME_SD_MOUNT=FAIL reason=SD_MOUNT_FAILED\n");
            return ESP_FAIL;
        }
        composition.mount_backend = &composition.sd_provider;
    }
    composition.mounted = true;
    emit("P4_NANO_RUNTIME_SD_MOUNT=PASS mount=%s\n",
         composition.mount_backend == &composition.persist_provider
             ? storage_fatfs::kMountPath
             : storage_sdmmc::kMountPath);

    if (!media_exists(media)) {
        emit("P4_NANO_RUNTIME_MEDIA result=NOT_FOUND path=%s\n",
             media.physical_path.data());
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=BOOT_MEDIA_NOT_FOUND\n");
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NOT_FOUND;
    }
    emit("P4_NANO_RUNTIME_MEDIA result=FOUND path=%s\n",
         media.physical_path.data());

    if (composition.session.initialize() != ESP_OK ||
        !composition.session.native_framebuffer_external()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_INIT_FAILED\n");
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_FAIL;
    }
    composition.ready_semaphore = xSemaphoreCreateBinary();
    composition.stopped_semaphore = xSemaphoreCreateBinary();
    if (composition.ready_semaphore == nullptr ||
        composition.stopped_semaphore == nullptr) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_FAILED\n");
        destroy_sync(&composition);
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        owner_task, "p4_nano_pc98", kRuntimeStackBytes, &composition,
        kRuntimePriority, &composition.owner_task, kRuntimeCore);
    if (task_result != pdPASS) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_FAILED\n");
        destroy_sync(&composition);
        (void)composition.session.shutdown();
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(composition.ready_semaphore,
                       kStartupTimeoutTicks) != pdTRUE) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=TASK_START_TIMEOUT\n");
        composition.stop_requested.store(true, std::memory_order_release);
    }

    bool visible_reported = false;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    const std::int64_t validation_deadline =
        esp_timer_get_time() + 30LL * 1000LL * 1000LL;
#endif
    while (!composition.owner_done.load(std::memory_order_acquire)) {
        const auto consume = composition.session.consume_one();
        if (consume == p4_nano_live_display_session::ConsumeResult::Failed) {
            emit("P4_NANO_RUNTIME_RESULT=FAIL reason=DISPLAY_CONSUME_FAILED\n");
            composition.stop_requested.store(true, std::memory_order_release);
        }
        if (!visible_reported && composition.session.visible()) {
            visible_reported = true;
            emit("P4_NANO_RUNTIME_DISPLAY=VISIBLE\n");
        }

        np2_dosio_stats stats{};
        np2_dosio_stats_get(&stats);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
        if (validation_profile && esp_timer_get_time() >= validation_deadline) {
            emit("P4_NANO_%s_RESULT=FAIL reason=VALIDATION_TIMEOUT\n",
                 validation_kind == ValidationKind::Keyboard ? "KEYBOARD_VALIDATION"
                                                              : "RUNTIME");
            composition.stop_requested.store(true, std::memory_order_release);
        }
#endif
        vTaskDelay(kConsumerDelayTicks);
    }

    if (composition.stopped_semaphore != nullptr) {
        (void)xSemaphoreTake(composition.stopped_semaphore,
                             kStartupTimeoutTicks);
    }
    if (composition.owner_task != nullptr) {
        vTaskDelete(composition.owner_task);
        composition.owner_task = nullptr;
    }

    cleanup_after_owner_join(&composition);

    np2_dosio_stats stats{};
    np2_dosio_stats_get(&stats);
    const auto &counters = composition.session.counters();
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_kind == ValidationKind::Runtime &&
        composition.guest_completion.load(std::memory_order_acquire) ==
            GuestCompletion::Pass &&
        !composition.session.failed() &&
        stats.read_bytes > 0U && counters.submitted > 0U &&
        counters.transformed > 0U && counters.released > 0U;
#elif defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_kind == ValidationKind::Keyboard &&
        composition.keyboard_validation.state() ==
            np2_keyboard_validation::State::Complete &&
        keyboard_proof_counters_valid(&composition) &&
        !composition.session.failed() &&
        stats.read_bytes > 0U && counters.submitted > 0U &&
        counters.transformed > 0U && counters.released > 0U;
#endif

    (void)composition.session.shutdown();
    if (composition.mounted && composition.mount_backend != nullptr) {
        (void)composition.mount_backend->unmount();
        composition.mounted = false;
    }
    emit("P4_NANO_RUNTIME_DISK_READS opens=%" PRIu64 " calls=%" PRIu64
         " bytes=%" PRIu64 "\n",
         stats.open_count, stats.read_calls, stats.read_bytes);
    emit("P4_NANO_RUNTIME_SESSION submitted=%" PRIu32 " acquired=%" PRIu32
         " transformed=%" PRIu32 " released=%" PRIu32 " dropped=%" PRIu32
         "\n",
         counters.submitted, counters.acquired, counters.transformed,
         counters.released, counters.dropped);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    if (validation_profile) {
        emit("P4_NANO_RUNTIME_VALIDATION_RESULT=%s\n",
             validation_pass ? "PASS" : "FAIL");
    }
#elif defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    if (validation_profile) {
        emit("P4_NANO_KEYBOARD_VALIDATION_RESULT=%s\n",
             validation_pass ? "PASS" : "FAIL");
    }
#endif
    destroy_sync(&composition);
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    return validation_profile ? (validation_pass ? ESP_OK : ESP_FAIL)
                              : composition.owner_result;
#else
    return composition.owner_result;
#endif
}

} // namespace

namespace p4_nano_pc98_runtime {

esp_err_t run_production() noexcept
{
    return run_composition(ValidationKind::None, false);
}

esp_err_t run_validation() noexcept
{
    return run_composition(ValidationKind::Runtime,
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
                           true
#else
                           false
#endif
    );
}

esp_err_t run_keyboard_validation() noexcept
{
    return run_composition(ValidationKind::Keyboard,
#if defined(P4_NANO_RUNTIME_EMU_BACKEND)
                           true
#else
                           false
#endif
    );
}

} // namespace p4_nano_pc98_runtime
