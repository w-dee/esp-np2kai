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
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
#include <cpumem.h>
#endif
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
#include <result_v1_parser.h>
#endif
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
#include <np2kbd_control_v1_parser.h>
#include <np2kbd_result_v1_parser.h>
#endif
#include <diskimage/fddfile.h>
#include <scrnmng.h>
}

#include "np2host/dosio_esp.h"
#include "np2_keyboard_input_bridge/keyboard_input_bridge.hpp"
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
#include "np2_keyboard_input/keyboard_input.hpp"
#include "np2_keyboard_validation/validation_controller.hpp"
#endif
#include "np2runtime/np2runtime.hpp"
#if defined(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
#include "p4_nano_audio86_guest_binding/p4_nano_audio86_guest_binding.hpp"
#endif
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
#include "p4_nano_usb_keyboard/producer.hpp"
#endif
#include "p4_nano_live_display_session/session.hpp"
#include "p4_nano_pc98_runtime/audio86_outer_lifecycle.hpp"
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
#if defined(P4_NANO_AUDIO86_OUTER_TIMEOUT_TEST)
constexpr TickType_t kAudio86CompletionTimeoutTicks =
    pdMS_TO_TICKS(100U) == 0U ? 1U : pdMS_TO_TICKS(100U);
#else
constexpr TickType_t kAudio86CompletionTimeoutTicks =
    pdMS_TO_TICKS(30000U) == 0U ? 1U : pdMS_TO_TICKS(30000U);
#endif
constexpr std::uint32_t kAudio86ProductionCompletionGuardMs = 30000U;
static_assert(kAudio86ProductionCompletionGuardMs == 30000U);
constexpr TickType_t kConsumerDelayTicks = 1U;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
constexpr std::uintptr_t kResultPhysicalAddress = 0x29000U;
#endif
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
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

#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
enum class UsbKeyboardProofState : std::uint8_t {
    WaitingReady,
    WaitingAction,
    WaitingMake,
    WaitingBreak,
    WaitingResult,
    Complete,
    Failed,
};

enum class UsbKeyboardProofFailure : std::uint8_t {
    None,
    ReadyTimeout,
    UsbUnavailable,
    InputBeforePrompt,
    MakeTimeout,
    MakeBeforeDrain,
    BreakTimeout,
    BreakBeforeDrain,
    ResultTimeout,
    ProtocolInvalid,
    GuestFail,
    CounterMismatch,
    RuntimeFatal,
};
#endif

#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
/* The producer owns static task/queue storage.  Keep the object itself alive
 * if a bounded teardown ever fails and a USB/HID task is still unwinding. */
p4_nano_usb_keyboard::Producer s_usb_keyboard{};
#endif

constexpr p4_nano_pc98_runtime::MediaConfig media_config_for(
    const ValidationKind validation_kind, const bool emu_backend) noexcept
{
    if (validation_kind == ValidationKind::Runtime) {
        return emu_backend ? p4_nano_pc98_runtime::validation_media_config()
                           : p4_nano_pc98_runtime::hardware_validation_media_config();
    }
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
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
    bool audio86_real_guest = false;
    ValidationKind kind;
    bool emu;
    p4_nano_pc98_runtime::MediaConfig media;
    storage_sdmmc::SdmmcMountProvider sd_provider{};
    storage_fatfs::MountProvider persist_provider{};
    storage_fatfs::FatfsMountBackend *mount_backend = nullptr;
    p4_nano_live_display_session::Session session{};
    np2_keyboard_input_bridge::KeyboardInputBridge keyboard{};
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    p4_nano_usb_keyboard::Producer *usb_keyboard = &s_usb_keyboard;
#endif
    np2runtime::Runtime runtime{};
    SemaphoreHandle_t ready_semaphore = nullptr;
    SemaphoreHandle_t stopped_semaphore = nullptr;
    TaskHandle_t owner_task = nullptr;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> owner_done{false};
    std::atomic<GuestCompletion> guest_completion{GuestCompletion::Unknown};
    p4_nano_pc98_runtime::audio86_outer::Lifecycle audio86_lifecycle{};
    esp_err_t owner_result = ESP_FAIL;
    bool mounted = false;
    bool scrnmng_initialized = false;
    bool dosio_attached = false;
    bool fdd_attached = false;
    bool ready_signaled = false;
    bool keyboard_status_reported = false;
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
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
    bool keyboard_control_invalid_reported = false;
#endif
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    UsbKeyboardProofState usb_proof_state = UsbKeyboardProofState::WaitingReady;
    UsbKeyboardProofFailure usb_proof_failure = UsbKeyboardProofFailure::None;
    p4_nano_usb_keyboard::Counters usb_start_counters{};
    p4_nano_usb_keyboard::Counters usb_proof_counters{};
    np2_keyboard_input_bridge::BridgeCounters usb_start_bridge_counters{};
    np2_keyboard_input_bridge::BridgeCounters usb_proof_bridge_counters{};
    std::uint64_t usb_proof_deadline_us = 0U;
    bool usb_proof_ready_reported = false;
    bool usb_proof_terminal_reported = false;
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

const char *keyboard_control_observation_name(
    const np2kbd_control_v1_observation observation) noexcept
{
    switch (observation) {
    case NP2KBD_CONTROL_V1_PRE_PROTOCOL:
        return "PRE_PROTOCOL";
    case NP2KBD_CONTROL_V1_UNINITIALIZED:
        return "UNINITIALIZED";
    case NP2KBD_CONTROL_V1_READY:
        return "READY";
    case NP2KBD_CONTROL_V1_MAKE_OBSERVED:
        return "MAKE_OBSERVED";
    case NP2KBD_CONTROL_V1_BREAK_OBSERVED:
        return "BREAK_OBSERVED";
    case NP2KBD_CONTROL_V1_FAIL:
        return "FAIL";
    case NP2KBD_CONTROL_V1_TRANSIENT:
        return "TRANSIENT";
    case NP2KBD_CONTROL_V1_INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
}

void emit_keyboard_control_invalid(
    Composition *composition, const std::uint8_t have_state,
    const np2kbd_control_v1_state tracker_state,
    const std::uint8_t *snapshot,
    const np2kbd_control_v1_observation parsed_observation) noexcept
{
    if (composition == nullptr || snapshot == nullptr ||
        composition->keyboard_control_invalid_reported) {
        return;
    }
    composition->keyboard_control_invalid_reported = true;

    char encoded[NP2KBD_CONTROL_V1_SIZE * 2U + 1U]{};
    constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t index = 0U; index < NP2KBD_CONTROL_V1_SIZE; ++index) {
        encoded[index * 2U] = kHex[snapshot[index] >> 4U];
        encoded[index * 2U + 1U] = kHex[snapshot[index] & 0x0fU];
    }
    emit("P4_NANO_KEYBOARD_CONTROL_INVALID have_state=%u "
         "tracker_state=%u raw_state=0x%02x parse=%s snapshot=%s\n",
         static_cast<unsigned>(have_state),
         static_cast<unsigned>(tracker_state),
         static_cast<unsigned>(snapshot[NP2KBD_CONTROL_V1_STATE_OFFSET]),
         keyboard_control_observation_name(parsed_observation), encoded);
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

[[maybe_unused]] bool keyboard_proof_counters_valid(
    const Composition *composition) noexcept
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
    const auto tracker_have_state = composition->control_tracker.have_state;
    const auto tracker_state = composition->control_tracker.state;
    const auto tracked = np2kbd_control_v1_tracker_observe(
        &composition->control_tracker, control_snapshot,
        sizeof(control_snapshot));
    if (tracked == NP2KBD_CONTROL_V1_TRACK_INVALID) {
        np2kbd_control_v1_result parsed{};
        const auto parsed_observation = np2kbd_control_v1_parse(
            control_snapshot, sizeof(control_snapshot), &parsed);
        emit_keyboard_control_invalid(composition, tracker_have_state,
                                       tracker_state, control_snapshot,
                                       parsed_observation);
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

#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
const char *usb_proof_failure_name(
    const UsbKeyboardProofFailure failure) noexcept
{
    switch (failure) {
    case UsbKeyboardProofFailure::None:
        return "NONE";
    case UsbKeyboardProofFailure::ReadyTimeout:
        return "READY_TIMEOUT";
    case UsbKeyboardProofFailure::UsbUnavailable:
        return "USB_UNAVAILABLE";
    case UsbKeyboardProofFailure::InputBeforePrompt:
        return "INPUT_BEFORE_PROMPT";
    case UsbKeyboardProofFailure::MakeTimeout:
        return "MAKE_TIMEOUT";
    case UsbKeyboardProofFailure::MakeBeforeDrain:
        return "MAKE_BEFORE_DRAIN";
    case UsbKeyboardProofFailure::BreakTimeout:
        return "BREAK_TIMEOUT";
    case UsbKeyboardProofFailure::BreakBeforeDrain:
        return "BREAK_BEFORE_DRAIN";
    case UsbKeyboardProofFailure::ResultTimeout:
        return "RESULT_TIMEOUT";
    case UsbKeyboardProofFailure::ProtocolInvalid:
        return "PROTOCOL_INVALID";
    case UsbKeyboardProofFailure::GuestFail:
        return "GUEST_FAIL";
    case UsbKeyboardProofFailure::CounterMismatch:
        return "COUNTER_MISMATCH";
    case UsbKeyboardProofFailure::RuntimeFatal:
        return "RUNTIME_FATAL";
    }
    return "UNKNOWN";
}

std::uint32_t counter_delta(const std::uint32_t current,
                            const std::uint32_t baseline) noexcept
{
    return current >= baseline ? current - baseline : UINT32_MAX;
}

bool usb_proof_errors_unchanged(
    const p4_nano_usb_keyboard::Counters &current,
    const p4_nano_usb_keyboard::Counters &baseline) noexcept
{
    return current.reports_error_usage == baseline.reports_error_usage &&
           current.reports_malformed == baseline.reports_malformed &&
           current.unsupported_usages == baseline.unsupported_usages &&
           current.internal_queue_overflows ==
               baseline.internal_queue_overflows &&
           current.enqueue_full == baseline.enqueue_full &&
           current.enqueue_quarantined == baseline.enqueue_quarantined &&
           current.second_keyboard_rejected ==
               baseline.second_keyboard_rejected &&
           current.transfer_errors == baseline.transfer_errors &&
           current.producer_fatal == baseline.producer_fatal;
}

bool bridge_proof_errors_unchanged(
    const np2_keyboard_input_bridge::BridgeCounters &current,
    const np2_keyboard_input_bridge::BridgeCounters &baseline) noexcept
{
    return current.queue_overflows == baseline.queue_overflows &&
           current.queue_rejected == baseline.queue_rejected &&
           current.blocked_events == baseline.blocked_events &&
           current.recovery_discards == baseline.recovery_discards &&
           current.ownership.duplicate_suppressed ==
               baseline.ownership.duplicate_suppressed &&
           current.ownership.invalid_rejected ==
               baseline.ownership.invalid_rejected &&
           current.ownership.source_capacity_failures ==
               baseline.ownership.source_capacity_failures &&
           current.ownership.source_disconnects ==
               baseline.ownership.source_disconnects &&
           current.ownership.global_recoveries ==
               baseline.ownership.global_recoveries;
}

void emit_usb_proof_failure(Composition *composition,
                            const UsbKeyboardProofFailure failure) noexcept
{
    if (composition == nullptr || composition->usb_proof_terminal_reported) {
        return;
    }
    composition->usb_proof_terminal_reported = true;
    composition->usb_proof_failure = failure;
    composition->usb_proof_state = UsbKeyboardProofState::Failed;
    emit("P4_NANO_USB_KEYBOARD_PROOF_RESULT=FAIL reason=%s\n",
         usb_proof_failure_name(failure));
}

bool usb_proof_counters_valid(const Composition *composition,
                              const p4_nano_usb_keyboard::Counters &usb,
                              const np2_keyboard_input_bridge::BridgeCounters
                                  &bridge) noexcept
{
    if (composition == nullptr || !usb_proof_errors_unchanged(
                                      usb, composition->usb_proof_counters) ||
        !bridge_proof_errors_unchanged(
            bridge, composition->usb_proof_bridge_counters) ||
        composition->keyboard.quarantined()) {
        return false;
    }
    const auto &usb_base = composition->usb_proof_counters;
    const auto &bridge_base = composition->usb_proof_bridge_counters;
    return counter_delta(usb.reports_received, usb_base.reports_received) > 0U &&
           counter_delta(usb.neutral_events_generated,
                         usb_base.neutral_events_generated) >= 2U &&
           counter_delta(usb.neutral_events_enqueued,
                         usb_base.neutral_events_enqueued) >= 2U &&
           counter_delta(bridge.enqueued, bridge_base.enqueued) >= 2U &&
           counter_delta(bridge.dequeued, bridge_base.dequeued) >= 2U &&
           counter_delta(bridge.ownership.press_injected,
                         bridge_base.ownership.press_injected) >= 1U &&
           counter_delta(bridge.ownership.release_injected,
                         bridge_base.ownership.release_injected) >= 1U;
}

bool owner_iteration_usb_keyboard(Composition *composition) noexcept
{
    if (composition == nullptr || composition->usb_keyboard == nullptr) {
        return false;
    }
    const auto fail_and_stop = [&](const UsbKeyboardProofFailure failure) {
        emit_usb_proof_failure(composition, failure);
        composition->usb_keyboard->request_stop();
        composition->keyboard.shutdown();
        composition->session.detach_source();
        (void)composition->runtime.request_stop();
    };
    if (composition->stop_requested.load(std::memory_order_acquire) ||
        composition->runtime.failure()) {
        if (composition->usb_proof_state != UsbKeyboardProofState::Complete &&
            composition->usb_proof_state != UsbKeyboardProofState::Failed) {
            fail_and_stop(composition->runtime.failure()
                                             ? UsbKeyboardProofFailure::RuntimeFatal
                                             : UsbKeyboardProofFailure::ResultTimeout);
        }
        return true;
    }

    np2_keyboard_validation::InputSnapshot sample{};
    sample.now_us = static_cast<std::uint64_t>(esp_timer_get_time());
    snapshot_keyboard_protocol(composition, &sample);
    const auto input_result = composition->keyboard.owner_iteration();
    if (input_result ==
            np2_keyboard_input_bridge::OwnerIterationResult::Recovered ||
        input_result ==
            np2_keyboard_input_bridge::OwnerIterationResult::Quarantined) {
        fail_and_stop(UsbKeyboardProofFailure::CounterMismatch);
        return true;
    }
    const auto bridge = composition->keyboard.counters();
    const auto usb = composition->usb_keyboard->counters();
    if (composition->usb_keyboard->state() ==
            p4_nano_usb_keyboard::State::Disabled ||
        composition->usb_keyboard->state() ==
            p4_nano_usb_keyboard::State::TeardownFailed) {
        fail_and_stop(UsbKeyboardProofFailure::UsbUnavailable);
        return true;
    }
    const auto usb_start_neutral = counter_delta(
        usb.neutral_events_generated,
        composition->usb_start_counters.neutral_events_generated);
    if (usb.producer_fatal > composition->usb_start_counters.producer_fatal ||
        usb.internal_queue_overflows >
            composition->usb_start_counters.internal_queue_overflows ||
        usb.enqueue_full > composition->usb_start_counters.enqueue_full ||
        usb.enqueue_quarantined >
            composition->usb_start_counters.enqueue_quarantined) {
        fail_and_stop(UsbKeyboardProofFailure::UsbUnavailable);
        return true;
    }

    if (sample.control_observation ==
            np2_keyboard_validation::ControlObservation::Invalid ||
        sample.result_observation ==
            np2_keyboard_validation::ResultObservation::Invalid) {
        fail_and_stop(UsbKeyboardProofFailure::ProtocolInvalid);
        return true;
    }
    if (sample.control_observation ==
            np2_keyboard_validation::ControlObservation::Accepted &&
        (sample.control_state == np2_keyboard_validation::ControlState::Fail ||
         sample.result_observation ==
             np2_keyboard_validation::ResultObservation::Fail)) {
        fail_and_stop(UsbKeyboardProofFailure::GuestFail);
        return true;
    }

    if (composition->usb_proof_state == UsbKeyboardProofState::WaitingReady) {
        if (usb_start_neutral > 0U ||
            counter_delta(usb.neutral_events_enqueued,
                          composition->usb_start_counters.neutral_events_enqueued) >
                0U) {
            fail_and_stop(UsbKeyboardProofFailure::InputBeforePrompt);
            return true;
        }
        if (sample.now_us >= composition->usb_proof_deadline_us) {
            fail_and_stop(UsbKeyboardProofFailure::ReadyTimeout);
            return true;
        }
        if (sample.control_observation ==
                np2_keyboard_validation::ControlObservation::Accepted &&
            sample.control_state == np2_keyboard_validation::ControlState::Ready &&
            composition->usb_keyboard->state() ==
                p4_nano_usb_keyboard::State::Ready &&
            composition->usb_keyboard->device_connected()) {
            composition->usb_proof_counters = usb;
            composition->usb_proof_bridge_counters = bridge;
            composition->usb_proof_state = UsbKeyboardProofState::WaitingAction;
            composition->usb_proof_deadline_us = sample.now_us + 60'000'000U;
            composition->usb_proof_ready_reported = true;
            emit("P4_NANO_USB_KEYBOARD_PROOF_STATE=READY\n");
            emit("P4_NANO_USB_KEYBOARD_PROOF_ACTION=PRESS_AND_RELEASE_A\n");
        }
        return false;
    }

    const auto usb_events = counter_delta(
        usb.neutral_events_generated,
        composition->usb_proof_counters.neutral_events_generated);
    const auto bridge_press = counter_delta(
        bridge.ownership.press_injected,
        composition->usb_proof_bridge_counters.ownership.press_injected);
    const auto bridge_release = counter_delta(
        bridge.ownership.release_injected,
        composition->usb_proof_bridge_counters.ownership.release_injected);
    if (sample.now_us >= composition->usb_proof_deadline_us) {
        const auto timeout_failure =
            composition->usb_proof_state == UsbKeyboardProofState::WaitingBreak
                ? UsbKeyboardProofFailure::BreakTimeout
                : composition->usb_proof_state ==
                          UsbKeyboardProofState::WaitingResult
                      ? UsbKeyboardProofFailure::ResultTimeout
                      : UsbKeyboardProofFailure::MakeTimeout;
        fail_and_stop(timeout_failure);
        return true;
    }
    if (composition->usb_proof_state == UsbKeyboardProofState::WaitingAction &&
        usb_events > 0U) {
        composition->usb_proof_state = UsbKeyboardProofState::WaitingMake;
        composition->usb_proof_deadline_us = sample.now_us + 5'000'000U;
    }
    if (composition->usb_proof_state == UsbKeyboardProofState::WaitingMake) {
        if (sample.control_observation ==
                np2_keyboard_validation::ControlObservation::Accepted &&
            sample.control_state == np2_keyboard_validation::ControlState::MakeObserved) {
            if (bridge_press < 1U) {
                fail_and_stop(UsbKeyboardProofFailure::MakeBeforeDrain);
                return true;
            }
            emit("P4_NANO_USB_KEYBOARD_PROOF_STATE=MAKE_OBSERVED byte=0x%02x\n",
                 sample.observed_make);
            composition->usb_proof_state = UsbKeyboardProofState::WaitingBreak;
            composition->usb_proof_deadline_us = sample.now_us + 5'000'000U;
        }
        return false;
    }
    if (composition->usb_proof_state == UsbKeyboardProofState::WaitingBreak) {
        if (sample.control_observation ==
                np2_keyboard_validation::ControlObservation::Accepted &&
            sample.control_state == np2_keyboard_validation::ControlState::BreakObserved) {
            if (bridge_release < 1U || usb_events < 2U) {
                fail_and_stop(UsbKeyboardProofFailure::BreakBeforeDrain);
                return true;
            }
            emit("P4_NANO_USB_KEYBOARD_PROOF_STATE=BREAK_OBSERVED byte=0x%02x\n",
                 sample.observed_break);
            composition->usb_proof_state = UsbKeyboardProofState::WaitingResult;
            composition->usb_proof_deadline_us = sample.now_us + 5'000'000U;
        }
        return false;
    }
    if (composition->usb_proof_state == UsbKeyboardProofState::WaitingResult) {
        if (sample.result_observation ==
            np2_keyboard_validation::ResultObservation::Pass) {
            if (!usb_proof_counters_valid(composition, usb, bridge)) {
                fail_and_stop(UsbKeyboardProofFailure::CounterMismatch);
                return true;
            }
            composition->usb_proof_state = UsbKeyboardProofState::Complete;
            composition->usb_proof_terminal_reported = true;
            emit("P4_NANO_USB_KEYBOARD_PROOF_COUNTERS reports_received=%" PRIu32
                 " neutral_events_generated=%" PRIu32
                 " neutral_events_enqueued=%" PRIu32
                 " bridge_enqueued=%" PRIu32 " bridge_dequeued=%" PRIu32
                 " press=%" PRIu32 " release=%" PRIu32
                 " queue_overflows=%" PRIu32 " rejected=%" PRIu32
                 " blocked=%" PRIu32 " recoveries=%" PRIu32
                 " quarantined=%u\n",
                 counter_delta(usb.reports_received,
                               composition->usb_proof_counters.reports_received),
                 counter_delta(usb.neutral_events_generated,
                               composition->usb_proof_counters.neutral_events_generated),
                 counter_delta(usb.neutral_events_enqueued,
                               composition->usb_proof_counters.neutral_events_enqueued),
                 counter_delta(bridge.enqueued,
                               composition->usb_proof_bridge_counters.enqueued),
                 counter_delta(bridge.dequeued,
                               composition->usb_proof_bridge_counters.dequeued),
                 bridge_press, bridge_release, bridge.queue_overflows,
                 bridge.queue_rejected, bridge.blocked_events,
                 bridge.ownership.global_recoveries,
                 composition->keyboard.quarantined() ? 1U : 0U);
            emit("P4_NANO_USB_KEYBOARD_PROOF_RESULT=PASS make=0x%02x break=0x%02x\n",
                 sample.observed_make, sample.observed_break);
            composition->keyboard.shutdown();
            composition->session.detach_source();
            (void)composition->runtime.request_stop();
            return true;
        }
        return false;
    }
    return false;
}
#endif

[[maybe_unused]] void emit_keyboard_proof_failure(
    Composition *composition,
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

[[maybe_unused]] bool owner_iteration_keyboard(Composition *composition) noexcept
{
    if (composition == nullptr) {
        return false;
    }
    if (composition->stop_requested.load(std::memory_order_acquire)) {
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
        composition->usb_keyboard->request_stop();
#endif
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
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
        composition->usb_keyboard->request_stop();
#endif
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
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    return owner_iteration_usb_keyboard(composition);
#elif defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
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
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
        composition->usb_keyboard->request_stop();
#endif
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

bool signal_audio86_ready(Composition *composition, esp_err_t result) noexcept
{
    if (composition == nullptr || composition->ready_signaled)
        return false;
    const bool published = composition->audio86_lifecycle.publish_startup(
        result, result == ESP_OK);
    if (!published)
        return false;
    composition->ready_signaled = true;
    if (composition->ready_semaphore != nullptr)
        (void)xSemaphoreGive(composition->ready_semaphore);
    return true;
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

#if defined(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
    if (composition->audio86_real_guest) {
        /* This is intentionally the existing p4_nano_pc98 task body.  The
         * profile changes its bounded guest workload, not producer ownership,
         * affinity, priority, stack, or terminal-index-0 protocol. */
        p4_nano_audio86_guest_binding::publish_owner_phase(
            p4_nano_audio86_guest_binding::OwnerPhase::RuntimeInit);
        const np2runtime::Result runtime_init =
            composition->runtime.initialize(
                p4_nano_pc98_runtime::kFdd0OnlyEquipment);
        if (runtime_init != np2runtime::Result::Ok) {
            emit("P4_AUDIO86_REAL_GUEST_RESULT=FAIL reason=RUNTIME_INIT_FAILED\n");
            (void)signal_audio86_ready(composition, ESP_FAIL);
            (void)composition->audio86_lifecycle.publish_completion(
                ESP_FAIL, false);
        } else {
            p4_nano_audio86_guest_binding::publish_owner_phase(
                p4_nano_audio86_guest_binding::OwnerPhase::Ready);
            if (signal_audio86_ready(composition, ESP_OK)) {
#if defined(P4_NANO_AUDIO86_OUTER_TIMEOUT_TEST)
                emit("P4_AUDIO86_OUTER_TIMEOUT_TEST=READY_THEN_STALL\n");
                for (;;)
                    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#else
                const esp_err_t completion_result =
                    p4_nano_audio86_guest_binding::run_on_pc98_task(
                        composition->owner_task, &composition->runtime);
                /* Runtime owns the sole production lifecycle and therefore
                 * serializes pccore_term after service quiescence. */
                p4_nano_audio86_guest_binding::publish_owner_phase(
                    p4_nano_audio86_guest_binding::OwnerPhase::RuntimeCleanup);
                (void)composition->runtime.request_stop();
                (void)composition->runtime.run();
                (void)composition->audio86_lifecycle.publish_completion(
                    completion_result, completion_result == ESP_OK);
#endif
            } else {
                /* READY timed out first.  Do not enter the inner workload. */
                (void)composition->runtime.request_stop();
                (void)composition->runtime.run();
            }
        }
    } else {
#endif

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
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    composition->usb_start_counters = composition->usb_keyboard->counters();
    composition->usb_start_bridge_counters = composition->keyboard.counters();
    composition->usb_proof_state = UsbKeyboardProofState::WaitingReady;
    composition->usb_proof_deadline_us =
        static_cast<std::uint64_t>(esp_timer_get_time()) + 30'000'000U;
#endif

    signal_ready(composition, ESP_OK);
    emit("P4_NANO_RUNTIME_CORE=RUNNING\n");
    if (!composition->validation) {
        emit("P4_NANO_RUNTIME_RESULT=RUNNING\n");
    }
    (void)composition->runtime.run(owner_iteration, composition);
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    composition->usb_keyboard->request_stop();
#endif
    composition->keyboard.set_core_active(false);
    if (composition->runtime.failure()) {
        emit("P4_NANO_RUNTIME_RESULT=FAIL reason=RUNTIME_FATAL\n");
        composition->owner_result = ESP_FAIL;
    } else {
        composition->owner_result = ESP_OK;
    }

#if defined(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
    }
#endif
done:
    composition->keyboard.set_core_active(false);
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    composition->usb_keyboard->request_stop();
#endif
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
    if (composition->audio86_real_guest) {
        p4_nano_audio86_guest_binding::publish_owner_phase(
            p4_nano_audio86_guest_binding::OwnerPhase::Complete);
        p4_nano_audio86_guest_binding::publish_owner_phase(
            p4_nano_audio86_guest_binding::OwnerPhase::Parked);
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

#if defined(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
void emit_audio86_formal_terminal_once(
    Composition *composition,
    p4_nano_pc98_runtime::audio86_outer::TerminalOwner owner,
    bool complete) noexcept
{
#if defined(P4_NANO_AUDIO86_SUSTAINED_PHYSICAL_PROFILE) || \
    defined(P4_NANO_AUDIO86_OUTER_TIMEOUT_TEST)
    if (composition != nullptr &&
        composition->audio86_lifecycle.claim_terminal(owner)) {
        emit("P4_AUDIO86_PHYSICAL_5D3_S1_TERMINAL=%s\n",
             complete ? "COMPLETE" : "FAILED");
    }
#else
    (void)composition;
    (void)owner;
    (void)complete;
#endif
}

void emit_audio86_timeout(Composition *composition, bool ready_timeout) noexcept
{
    if (composition == nullptr ||
        !composition->audio86_lifecycle.claim_timeout_snapshot())
        return;
    p4_nano_audio86_guest_binding::mark_outer_timeout();
    const auto snapshot =
        p4_nano_audio86_guest_binding::timeout_diagnostic_snapshot();
    const auto outer_state = composition->audio86_lifecycle.outer_state.load(
        std::memory_order_acquire);
    const auto startup_state = composition->audio86_lifecycle.startup_state.load(
        std::memory_order_acquire);
    const auto startup_result =
        composition->audio86_lifecycle.startup_result.load(
            std::memory_order_acquire);
    const auto completion_state =
        composition->audio86_lifecycle.completion_state.load(
            std::memory_order_acquire);
    const auto completion_result =
        composition->audio86_lifecycle.completion_result.load(
            std::memory_order_acquire);
    emit("P4_AUDIO86_OUTER_TIMEOUT class=%s inner_result=INDETERMINATE"
         " outer_state=%" PRIu32 " startup_state=%" PRIu32
         " startup_result=%" PRId32 " completion_state=%" PRIu32
         " completion_result=%" PRId32 " guard_ms=%" PRIu32 "\n",
         ready_timeout ? "OUTER_READY_TIMEOUT" : "OUTER_COMPLETION_TIMEOUT",
         outer_state, startup_state, startup_result, completion_state,
         completion_result, kAudio86ProductionCompletionGuardMs);
    emit("P4_AUDIO86_TIMEOUT_SNAPSHOT coherence=%s owner_phase=%" PRIu32
         " service_observable=%" PRIu32 " service_state=%" PRIu32
         " failure_category=%" PRIu32 " failure_origin=%" PRIu32
         " failure_subcode=%" PRIu32 " failure_sequence=%" PRIu32
         " cleanup=%" PRIu32 " guest_frame=%" PRIu64
         " published_horizon=%" PRIu64 " rendered=%" PRIu64
         " accepted=%" PRIu64 " producer_done=%" PRIu32
         " guest_attached=%" PRIu32 " sink_reachable=%" PRIu32
         " event_occupancy=%" PRIu32 " byte_occupancy=%" PRIu32
         " q240_occupancy=%" PRIu32 " q240_produced=%" PRIu32
         " q240_submitted=%" PRIu32 " output_state=%" PRIu32
         " worker_wait_reason=%" PRIu32 " reset_seen=%" PRIu32
         " reset_ordinal=%" PRIu32 " reset_ack=%" PRIu32
         " terminal_armed=%" PRIu32 " terminal_published=%" PRIu32
         " terminal_observed=%" PRIu32 " terminal_pcm_ready=%" PRIu32
         " physical_sink_state=%" PRIu32
         " physical_sticky_error=%" PRIu32 " physical_qovf=%" PRIu32
         " callback_active=%" PRIu32 " callback_in_flight=%" PRIu32 "\n",
         snapshot.snapshot_coherent != 0U ? "PASS" : "FAILED",
         snapshot.owner_phase, snapshot.service_observable,
         snapshot.live_service_state, snapshot.failure_category,
         snapshot.failure_origin, snapshot.failure_subcode,
         snapshot.failure_sequence, snapshot.cleanup_state,
         snapshot.guest_authoritative_frame,
         snapshot.latest_published_horizon, snapshot.rendered_frames,
         snapshot.accepted_frames, snapshot.producer_done,
         snapshot.guest_attached, snapshot.sink_reachable,
         snapshot.event_ring_occupancy, snapshot.byte_ring_occupancy,
         snapshot.q240_occupancy, snapshot.q240_produced,
         snapshot.q240_submitted, snapshot.output_state,
         snapshot.worker_wait_reason, snapshot.reset_seen,
         snapshot.reset_ordinal, snapshot.reset_ack,
         snapshot.terminal_armed, snapshot.terminal_horizon_published,
         snapshot.terminal_horizon_observed, snapshot.terminal_pcm_ready,
         snapshot.physical_sink_state, snapshot.physical_sticky_error,
         snapshot.physical_qovf, snapshot.callback_active,
         snapshot.callback_in_flight);
    const auto &owner = snapshot.owner_progress;
    emit("P4_AUDIO86_TIMEOUT_OWNER_PROGRESS coherence=%s history_depth=%zu"
         " history_count=%" PRIu32 " subphase=%s progress_pattern=%s"
         " checkpoint_calls=%" PRIu32 " checkpoint_success=%" PRIu32
         " checkpoint_retries=%" PRIu32
         " checkpoint_last_enter_us=%" PRIu64
         " checkpoint_last_exit_us=%" PRIu64
         " checkpoint_max_duration_us=%" PRIu64
         " checkpoint_last_result=%" PRIu32
         " checkpoint_last_frame=%" PRIu64 "\n",
         owner.coherent != 0U ? "PASS" : "FAILED",
         p4_nano_audio86_guest_binding::kOwnerCheckpointHistoryDepth,
         owner.checkpoint_count,
         p4_nano_audio86_guest_binding::owner_subphase_name(owner.subphase),
         p4_nano_audio86_guest_binding::owner_progress_pattern_name(
             owner.pattern),
         owner.checkpoint_call_count, owner.checkpoint_success_count,
         snapshot.checkpoint_retry_count, owner.checkpoint_last_enter_us,
         owner.checkpoint_last_exit_us, owner.checkpoint_max_duration_us,
         owner.checkpoint_last_result, owner.checkpoint_last_frame);
    emit("P4_AUDIO86_TIMEOUT_OWNER_BACKPRESSURE transaction_active=%" PRIu32
         " reserved_event_slots=%" PRIu32 " reserved_byte_count=%" PRIu32
         " horizon_owned=%" PRIu32 " horizon_mailbox_state=%" PRIu32
         " transaction_waiting=%" PRIu32
         " progress_checkpoint_retrying=%" PRIu32
         " current_checkpoint_retry_count=%" PRIu32
         " max_checkpoint_retry_count=%" PRIu32 "\n",
         snapshot.transaction_active, snapshot.reserved_event_slots,
         snapshot.reserved_byte_count, snapshot.horizon_owned,
         snapshot.horizon_mailbox_state, snapshot.transaction_waiting,
         snapshot.progress_checkpoint_retrying,
         snapshot.current_checkpoint_retry_count,
         snapshot.max_checkpoint_retry_count);
    for (std::size_t index = 0U;
         index < p4_nano_audio86_guest_binding::kOwnerCheckpointHistoryDepth;
         ++index) {
        const bool available = index < owner.checkpoint_count &&
                               owner.history[index].valid != 0U;
        const auto &record = owner.history[index];
        const uint64_t unavailable_u64 = UINT64_MAX;
        const uint32_t unavailable_u32 = UINT32_MAX;
        const uint16_t unavailable_u16 = UINT16_MAX;
        const uint64_t io_ordinal = available
            ? record.last_guest_io_ordinal : unavailable_u64;
        const uint64_t expected_next_io =
            io_ordinal == unavailable_u64 ? unavailable_u64 : io_ordinal + 1U;
        emit("P4_AUDIO86_TIMEOUT_CHECKPOINT index=%zu valid=%u"
             " sequence=%" PRIu32 " wall_us=%" PRIu64
             " guest_cycle=%" PRIu64 " guest_frame=%" PRIu64
             " cs=0x%04" PRIx16 " ip=0x%04" PRIx16
             " flags=0x%04" PRIx16 " if=%u cx=%" PRIu16
             " bp=%" PRIu16 " hlt=%u next_opcode=0x%02x"
             " last_io_ordinal=%" PRIu64
             " expected_next_io_ordinal=%" PRIu64
             " last_io_port=0x%04" PRIx16
             " last_io_frame=%" PRIu64 " last_io_cycle=%" PRIu64
             " timer_a_running=%u timer_b_running=%u opna_status=0x%02x"
             " pic_pending=0x%04" PRIx16 " pic_mask=0x%04" PRIx16
             " next_nevent_id=%" PRIu32 " next_nevent_remaining=%" PRId32
             " published_horizon=%" PRIu64 " rendered_frame=%" PRIu64
             "\n",
             index, available ? 1U : 0U,
             available ? record.checkpoint_sequence : unavailable_u32,
             available ? record.wall_time_us : unavailable_u64,
             available ? record.guest_cycle : unavailable_u64,
             available ? record.guest_frame : unavailable_u64,
             available ? record.cs : unavailable_u16,
             available ? record.ip : unavailable_u16,
             available ? record.flags : unavailable_u16,
             static_cast<unsigned>(available ? record.interrupt_enabled
                                             : UINT8_MAX),
             available ? record.cx : unavailable_u16,
             available ? record.bp : unavailable_u16,
             static_cast<unsigned>(available ? record.hlt : UINT8_MAX),
             static_cast<unsigned>(available ? record.next_opcode
                                             : UINT8_MAX),
             io_ordinal, expected_next_io,
             available ? record.last_guest_io_port : unavailable_u16,
             available ? record.last_guest_io_frame : unavailable_u64,
             available ? record.last_guest_io_cycle : unavailable_u64,
             static_cast<unsigned>(available ? record.timer_a_running
                                             : UINT8_MAX),
             static_cast<unsigned>(available ? record.timer_b_running
                                             : UINT8_MAX),
             static_cast<unsigned>(available ? record.opna_status
                                             : UINT8_MAX),
             available ? record.pic_pending : unavailable_u16,
             available ? record.pic_mask : unavailable_u16,
             available ? record.next_nevent_id : unavailable_u32,
             available ? record.next_nevent_remaining : INT32_MIN,
             available ? record.published_horizon : unavailable_u64,
             available ? record.rendered_frame : unavailable_u64);
    }
    for (std::size_t index = 0U;
         index < p4_nano_audio86_guest_binding::kOwnerCheckpointIntervalCount;
         ++index) {
        const auto &interval = owner.intervals[index];
        const bool available = interval.valid != 0U;
        emit("P4_AUDIO86_TIMEOUT_PROGRESS_DELTA index=%zu valid=%u"
             " from_sequence=%" PRIu32 " to_sequence=%" PRIu32
             " delta_wall_us=%" PRIu64 " delta_guest_cycles=%" PRIu64
             " delta_guest_frames=%" PRIu64
             " guest_cycles_per_second=%" PRIu64
             " guest_frames_per_second=%" PRIu64 "\n",
             index, available ? 1U : 0U,
             available ? interval.from_sequence : UINT32_MAX,
             available ? interval.to_sequence : UINT32_MAX,
             available ? interval.delta_wall_us : UINT64_MAX,
             available ? interval.delta_guest_cycles : UINT64_MAX,
             available ? interval.delta_guest_frames : UINT64_MAX,
             available ? interval.guest_cycles_per_second : UINT64_MAX,
             available ? interval.guest_frames_per_second : UINT64_MAX);
    }
    const int stop_result =
        p4_nano_audio86_guest_binding::timeout_request_async_stop();
    emit("P4_AUDIO86_TIMEOUT_STOP attempted=%s result=%d quiescence=UNPROVEN"
         " owner_deleted=NO resources_reclaimed=NO\n",
         snapshot.service_observable != 0U ? "YES" : "NO", stop_result);
    emit("P4_AUDIO86_REAL_GUEST_RESULT=FAIL reason=%s"
         " inner_result=INDETERMINATE\n",
         ready_timeout ? "OUTER_READY_TIMEOUT" : "OUTER_COMPLETION_TIMEOUT");
    emit_audio86_formal_terminal_once(
        composition,
        ready_timeout
            ? p4_nano_pc98_runtime::audio86_outer::TerminalOwner::ReadyTimeout
            : p4_nano_pc98_runtime::audio86_outer::TerminalOwner::CompletionTimeout,
        false);
}
#endif

esp_err_t run_composition(const ValidationKind validation_kind,
                          const bool emu_backend) noexcept
{
    const bool validation_profile = validation_kind != ValidationKind::None;
#if defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    /* The keyboard validation firmware is a one-shot application entry.  Keep
     * its large composition in static storage so the main task stack remains
     * the production-sized 3584 bytes; the owner task still receives the same
     * composition for the full validation lifetime. */
    static Composition composition(validation_kind, emu_backend);
#else
    Composition composition(validation_kind, emu_backend);
#endif
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

#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    if (!composition.stop_requested.load(std::memory_order_acquire)) {
        (void)composition.usb_keyboard->start(composition.keyboard);
    }
#endif

    bool visible_reported = false;
#if defined(P4_NANO_RUNTIME_VALIDATION_PROFILE) || \
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    const std::int64_t validation_deadline =
        esp_timer_get_time() +
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
        120LL * 1000LL * 1000LL;
#else
        30LL * 1000LL * 1000LL;
#endif
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

    bool usb_stop_clean = true;
#if defined(P4_NANO_REAL_RUNTIME_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    usb_stop_clean = composition.usb_keyboard->stop() ==
                     p4_nano_usb_keyboard::StopResult::Clean;
    if (!usb_stop_clean) {
        emit("P4_NANO_USB_KEYBOARD_RESULT=FAIL reason=TEARDOWN_FAILED\n");
        composition.owner_result = ESP_FAIL;
    }
#endif

    cleanup_after_owner_join(&composition);

    np2_dosio_stats stats{};
    np2_dosio_stats_get(&stats);
    const auto &counters = composition.session.counters();
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_kind == ValidationKind::Keyboard &&
        composition.usb_proof_state == UsbKeyboardProofState::Complete &&
        !composition.session.failed() && usb_stop_clean &&
        stats.read_bytes > 0U && counters.submitted > 0U &&
        counters.transformed > 0U && counters.released > 0U;
#elif defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_kind == ValidationKind::Runtime &&
        composition.guest_completion.load(std::memory_order_acquire) ==
            GuestCompletion::Pass &&
        !composition.session.failed() &&
        usb_stop_clean &&
        stats.read_bytes > 0U && counters.submitted > 0U &&
        counters.transformed > 0U && counters.released > 0U;
#elif defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE)
    const bool validation_pass =
        validation_kind == ValidationKind::Keyboard &&
        composition.keyboard_validation.state() ==
            np2_keyboard_validation::State::Complete &&
        keyboard_proof_counters_valid(&composition) &&
        !composition.session.failed() &&
        usb_stop_clean &&
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
#if defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
    if (validation_profile) {
        emit("P4_NANO_USB_KEYBOARD_VALIDATION_RESULT=%s\n",
             validation_pass ? "PASS" : "FAIL");
    }
#elif defined(P4_NANO_RUNTIME_VALIDATION_PROFILE)
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
    defined(P4_NANO_KEYBOARD_VALIDATION_PROFILE) || \
    defined(P4_NANO_USB_KEYBOARD_VALIDATION_PROFILE)
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

esp_err_t run_usb_keyboard_validation() noexcept
{
    return run_composition(ValidationKind::Keyboard, false);
}

esp_err_t run_audio86_real_guest() noexcept
{
#if !defined(P4_NANO_AUDIO86_REAL_GUEST_PROFILE)
    return ESP_ERR_NOT_SUPPORTED;
#else
    /* Keep the substantial composition off the main-task stack.  This path
     * intentionally bypasses media/display setup: it is a headless canonical
     * i286 guest profile, not the normal PC-98 runtime composition. */
    static Composition composition(ValidationKind::None, false);
    composition.audio86_real_guest = true;
    composition.ready_signaled = false;
    composition.owner_task = nullptr;
    composition.owner_done.store(false, std::memory_order_relaxed);
    composition.audio86_lifecycle.reset(ESP_FAIL);
    p4_nano_audio86_guest_binding::timeout_diagnostic_reset();
    composition.ready_semaphore = xSemaphoreCreateBinary();
    composition.stopped_semaphore = xSemaphoreCreateBinary();
    if (composition.ready_semaphore == nullptr || composition.stopped_semaphore == nullptr) {
        destroy_sync(&composition);
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t task_result = xTaskCreatePinnedToCore(
        owner_task, "p4_nano_pc98", kRuntimeStackBytes, &composition,
        kRuntimePriority, &composition.owner_task, kRuntimeCore);
    if (task_result != pdPASS) {
        destroy_sync(&composition);
        return ESP_ERR_NO_MEM;
    }
    composition.audio86_lifecycle.begin_ready_wait();
    if (xSemaphoreTake(composition.ready_semaphore, kStartupTimeoutTicks) !=
        pdTRUE) {
        (void)composition.audio86_lifecycle.mark_startup_timeout(
            ESP_ERR_TIMEOUT);
        emit_audio86_timeout(&composition, true);
        return ESP_ERR_TIMEOUT;
    }
    const auto startup_state =
        static_cast<p4_nano_pc98_runtime::audio86_outer::StartupState>(
            composition.audio86_lifecycle.startup_state.load(
                std::memory_order_acquire));
    composition.audio86_lifecycle.begin_completion_wait();
    if (xSemaphoreTake(composition.stopped_semaphore,
                       kAudio86CompletionTimeoutTicks) != pdTRUE) {
        (void)composition.audio86_lifecycle.mark_completion_timeout(
            ESP_ERR_TIMEOUT);
        emit_audio86_timeout(&composition, false);
        return ESP_ERR_TIMEOUT;
    }
    const bool owner_done =
        composition.owner_done.load(std::memory_order_acquire);
    if (!composition.audio86_lifecycle.owner_delete_allowed(true,
                                                             owner_done)) {
        (void)composition.audio86_lifecycle.mark_completion_timeout(
            ESP_ERR_TIMEOUT);
        emit_audio86_timeout(&composition, false);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result =
        static_cast<esp_err_t>(composition.audio86_lifecycle.completion_result
                                   .load(std::memory_order_acquire));
    composition.audio86_lifecycle.mark_complete();
    if (composition.owner_task != nullptr) {
        vTaskDelete(composition.owner_task);
        composition.owner_task = nullptr;
    }
    destroy_sync(&composition);
    const bool success =
        startup_state ==
            p4_nano_pc98_runtime::audio86_outer::StartupState::Ready &&
        result == ESP_OK;
    emit_audio86_formal_terminal_once(
        &composition,
        success
            ? p4_nano_pc98_runtime::audio86_outer::TerminalOwner::InnerComplete
            : p4_nano_pc98_runtime::audio86_outer::TerminalOwner::InnerFailed,
        success);
    return success ? ESP_OK : result;
#endif
}

} // namespace p4_nano_pc98_runtime
