#include "p4_nano_usb_keyboard/producer.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_boot_keyboard/hid_boot_keyboard.h"
#include "usb_hid_keyboard_adapter/adapter.hpp"

namespace p4_nano_usb_keyboard {
namespace {

constexpr TickType_t kStartupTimeout = pdMS_TO_TICKS(5000U) == 0U
                                           ? 1U
                                           : pdMS_TO_TICKS(5000U);
constexpr TickType_t kShutdownTimeout = pdMS_TO_TICKS(5000U) == 0U
                                            ? 1U
                                            : pdMS_TO_TICKS(5000U);
constexpr TickType_t kTaskPoll = pdMS_TO_TICKS(50U) == 0U
                                     ? 1U
                                     : pdMS_TO_TICKS(50U);
constexpr std::size_t kTaskStackWords =
    (kTaskStackBytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t);

struct Storage final {
    StaticQueue_t queue{};
    std::array<std::uint8_t, kRawQueueCapacity * sizeof(RawEvent)>
        queue_buffer{};
    StaticTask_t usb_task{};
    StaticTask_t hid_task{};
    StaticTask_t producer_task{};
    std::array<StackType_t, kTaskStackWords> usb_stack{};
    std::array<StackType_t, kTaskStackWords> hid_stack{};
    std::array<StackType_t, kTaskStackWords> producer_stack{};
    StaticSemaphore_t usb_ready{};
    StaticSemaphore_t usb_done{};
    StaticSemaphore_t hid_start{};
    StaticSemaphore_t hid_done{};
    StaticSemaphore_t producer_ready{};
    StaticSemaphore_t producer_done{};
};

Storage s_storage{};

static_assert(std::is_trivially_copyable_v<RawEvent>);
static_assert(kTaskStackWords * sizeof(StackType_t) >= kTaskStackBytes);

void marker(const char *line) noexcept
{
    std::printf("%s\n", line);
    std::fflush(stdout);
}

const char *bridge_result_name(
    const np2_keyboard_input_bridge::EnqueueResult result) noexcept
{
    switch (result) {
    case np2_keyboard_input_bridge::EnqueueResult::Enqueued:
        return "enqueued";
    case np2_keyboard_input_bridge::EnqueueResult::Invalid:
        return "invalid";
    case np2_keyboard_input_bridge::EnqueueResult::Full:
        return "full";
    case np2_keyboard_input_bridge::EnqueueResult::Quarantined:
        return "quarantined";
    case np2_keyboard_input_bridge::EnqueueResult::NotInitialized:
        return "not_initialized";
    }
    return "unknown";
}

} // namespace

bool Producer::start(np2_keyboard_input_bridge::KeyboardInputBridge &bridge) noexcept
{
    if (state_.load(std::memory_order_acquire) != State::Stopped) {
        return accepting();
    }

    bridge_ = &bridge;
    raw_queue_ = xQueueCreateStatic(
        kRawQueueCapacity, sizeof(RawEvent), s_storage.queue_buffer.data(),
        &s_storage.queue);
    usb_ready_ = xSemaphoreCreateBinaryStatic(&s_storage.usb_ready);
    usb_done_ = xSemaphoreCreateBinaryStatic(&s_storage.usb_done);
    hid_start_ = xSemaphoreCreateBinaryStatic(&s_storage.hid_start);
    hid_done_ = xSemaphoreCreateBinaryStatic(&s_storage.hid_done);
    producer_ready_ =
        xSemaphoreCreateBinaryStatic(&s_storage.producer_ready);
    producer_done_ = xSemaphoreCreateBinaryStatic(&s_storage.producer_done);
    if (raw_queue_ == nullptr || usb_ready_ == nullptr || usb_done_ == nullptr ||
        hid_start_ == nullptr ||
        hid_done_ == nullptr || producer_ready_ == nullptr ||
        producer_done_ == nullptr) {
        emit_disabled_once("STATIC_STORAGE_FAILED");
        state_.store(State::Disabled, std::memory_order_release);
        return false;
    }

    (void)xQueueReset(raw_queue_);
    (void)xSemaphoreTake(usb_ready_, 0U);
    (void)xSemaphoreTake(usb_done_, 0U);
    (void)xSemaphoreTake(hid_start_, 0U);
    (void)xSemaphoreTake(hid_done_, 0U);
    (void)xSemaphoreTake(producer_ready_, 0U);
    (void)xSemaphoreTake(producer_done_, 0U);

    hid_boot_keyboard_init(&parser_state_);
    adapter_.reset();
    stop_requested_.store(false, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    producer_done_flag_.store(false, std::memory_order_release);
    start_result_.store(false, std::memory_order_release);
    fault_latched_.store(false, std::memory_order_release);
    source_cleanup_requested_.store(false, std::memory_order_release);
    bridge_failure_.store(false, std::memory_order_release);
    bridge_calls_allowed_.store(true, std::memory_order_release);
    disabled_reported_.store(false, std::memory_order_release);
    fatal_reported_.store(false, std::memory_order_release);
    counters_reported_.store(false, std::memory_order_release);
    ever_connected_ = false;
    second_keyboard_reported_ = false;
    usb_client_.store(nullptr, std::memory_order_release);
    active_handle_.store(nullptr, std::memory_order_release);
    active_open_.store(false, std::memory_order_release);
    usb_host_installed_.store(false, std::memory_order_release);
    hid_installed_.store(false, std::memory_order_release);
    state_.store(State::Starting, std::memory_order_release);

    usb_task_ = xTaskCreateStatic(
        &Producer::usb_library_task_entry, "p4_usb_lib", kTaskStackWords,
        this, kUsbLibraryPriority, s_storage.usb_stack.data(),
        &s_storage.usb_task);
    hid_task_ = xTaskCreateStatic(
        &Producer::hid_event_task_entry, "p4_usb_hid", kTaskStackWords, this,
        kHidEventPriority, s_storage.hid_stack.data(), &s_storage.hid_task);
    producer_task_ = xTaskCreateStatic(
        &Producer::producer_task_entry, "p4_usb_prod", kTaskStackWords, this,
        kProducerPriority, s_storage.producer_stack.data(),
        &s_storage.producer_task);
    if (usb_task_ == nullptr || hid_task_ == nullptr ||
        producer_task_ == nullptr) {
        emit_disabled_once("TASK_CREATE_FAILED");
        request_stop();
        stop();
        state_.store(State::Disabled, std::memory_order_release);
        return false;
    }

    if (xSemaphoreTake(producer_ready_, kStartupTimeout) != pdTRUE) {
        emit_disabled_once("START_TIMEOUT");
        request_stop();
        stop();
        state_.store(State::Disabled, std::memory_order_release);
        return false;
    }
    if (!start_result_.load(std::memory_order_acquire)) {
        state_.store(State::Disabled, std::memory_order_release);
        return false;
    }
    state_.store(State::Ready, std::memory_order_release);
    return true;
}

void Producer::request_stop() noexcept
{
    accepting_.store(false, std::memory_order_release);
    /* Detach the bridge before waking producer-owned tasks.  Internal fault
     * paths do not call request_stop(), so they may still perform the one
     * source-local cleanup requested by their fault latch. */
    bridge_calls_allowed_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    const auto client = usb_client_.load(std::memory_order_acquire);
    if (client != nullptr) {
        (void)usb_host_client_unblock(client);
    }
    if (producer_task_ != nullptr) {
        xTaskNotifyGive(producer_task_);
    }
    if (hid_task_ != nullptr) {
        xTaskNotifyGive(hid_task_);
    }
    if (usb_task_ != nullptr) {
        xTaskNotifyGive(usb_task_);
    }
}

void Producer::stop() noexcept
{
    if (state_.load(std::memory_order_acquire) == State::Stopped) {
        return;
    }
    state_.store(State::Stopping, std::memory_order_release);
    request_stop();
    if (producer_done_ != nullptr &&
        xSemaphoreTake(producer_done_, kShutdownTimeout) != pdTRUE) {
        increment_fatal_once("PRODUCER_STOP_TIMEOUT");
    }
    if (usb_done_ != nullptr &&
        xSemaphoreTake(usb_done_, kShutdownTimeout) != pdTRUE) {
        increment_fatal_once("USB_TASK_STOP_TIMEOUT");
    }
    producer_task_ = nullptr;
    hid_task_ = nullptr;
    usb_task_ = nullptr;
    raw_queue_ = nullptr;
    usb_client_.store(nullptr, std::memory_order_release);
    active_handle_.store(nullptr, std::memory_order_release);
    active_open_.store(false, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    emit_counters_once();
    marker("P4_NANO_USB_KEYBOARD_HOST=STOPPED");
    state_.store(State::Stopped, std::memory_order_release);
}

Counters Producer::counters() const noexcept
{
    return {reports_received_.load(std::memory_order_relaxed),
            reports_clean_.load(std::memory_order_relaxed),
            reports_error_usage_.load(std::memory_order_relaxed),
            reports_malformed_.load(std::memory_order_relaxed),
            neutral_events_generated_.load(std::memory_order_relaxed),
            neutral_events_enqueued_.load(std::memory_order_relaxed),
            unsupported_usages_.load(std::memory_order_relaxed),
            internal_queue_overflows_.load(std::memory_order_relaxed),
            enqueue_full_.load(std::memory_order_relaxed),
            enqueue_quarantined_.load(std::memory_order_relaxed),
            disconnects_.load(std::memory_order_relaxed),
            reconnects_.load(std::memory_order_relaxed),
            second_keyboard_rejected_.load(std::memory_order_relaxed),
            transfer_errors_.load(std::memory_order_relaxed),
            producer_fatal_.load(std::memory_order_relaxed)};
}

void Producer::usb_library_task_entry(void *context) noexcept
{
    static_cast<Producer *>(context)->usb_library_task();
}

void Producer::hid_event_task_entry(void *context) noexcept
{
    static_cast<Producer *>(context)->hid_event_task();
}

void Producer::producer_task_entry(void *context) noexcept
{
    static_cast<Producer *>(context)->producer_task();
}

void Producer::usb_library_task() noexcept
{
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = 0,
    };
    const esp_err_t install_result = usb_host_install(&config);
    if (install_result != ESP_OK) {
        emit_disabled_once("USB_HOST_INSTALL_FAILED");
        stop_requested_.store(true, std::memory_order_release);
        accepting_.store(false, std::memory_order_release);
        if (usb_ready_ != nullptr) {
            (void)xSemaphoreGive(usb_ready_);
        }
        if (usb_done_ != nullptr) {
            (void)xSemaphoreGive(usb_done_);
        }
        vTaskDelete(nullptr);
        return;
    }
    usb_host_installed_.store(true, std::memory_order_release);
    marker("P4_NANO_USB_KEYBOARD_HOST=INSTALLING");
    if (usb_ready_ != nullptr) {
        (void)xSemaphoreGive(usb_ready_);
    }

    bool devices_freed = false;
    while (true) {
        uint32_t flags = 0U;
        const esp_err_t result =
            usb_host_lib_handle_events(kTaskPoll, &flags);
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            increment_fatal_once("USB_HOST_EVENT_FAILED");
            stop_requested_.store(true, std::memory_order_release);
        }
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0U &&
            stop_requested_.load(std::memory_order_acquire)) {
            const esp_err_t free_result = usb_host_device_free_all();
            if (free_result == ESP_OK) {
                devices_freed = true;
            } else if (free_result == ESP_ERR_NOT_FINISHED) {
                const TickType_t deadline = xTaskGetTickCount() + kShutdownTimeout;
                while (xTaskGetTickCount() < deadline) {
                    uint32_t free_flags = 0U;
                    const esp_err_t free_event =
                        usb_host_lib_handle_events(kTaskPoll, &free_flags);
                    if (free_event != ESP_OK && free_event != ESP_ERR_TIMEOUT) {
                        break;
                    }
                    if ((free_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0U) {
                        devices_freed = true;
                        break;
                    }
                }
            }
            break;
        }
        if (stop_requested_.load(std::memory_order_acquire) &&
            usb_client_.load(std::memory_order_acquire) == nullptr) {
            /* Deregistration generates NO_CLIENTS; keep polling until that
             * lifecycle event is observed rather than uninstalling early. */
            continue;
        }
    }

    if (!devices_freed && stop_requested_.load(std::memory_order_acquire)) {
        increment_fatal_once("USB_DEVICE_FREE_TIMEOUT");
    }
    const esp_err_t uninstall_result = usb_host_uninstall();
    if (uninstall_result != ESP_OK) {
        increment_fatal_once("USB_HOST_UNINSTALL_FAILED");
    }
    usb_host_installed_.store(false, std::memory_order_release);
    if (usb_done_ != nullptr) {
        (void)xSemaphoreGive(usb_done_);
    }
    vTaskDelete(nullptr);
}

void Producer::hid_event_task() noexcept
{
    if (hid_start_ != nullptr) {
        (void)xSemaphoreTake(hid_start_, kStartupTimeout);
    }
    if (!hid_installed_.load(std::memory_order_acquire)) {
        if (hid_done_ != nullptr) {
            (void)xSemaphoreGive(hid_done_);
        }
        vTaskDelete(nullptr);
        return;
    }
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const esp_err_t result = hid_host_handle_events(kTaskPoll);
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            increment_fatal_once("HID_EVENT_FAILED");
            stop_requested_.store(true, std::memory_order_release);
            accepting_.store(false, std::memory_order_release);
            break;
        }
    }
    if (hid_done_ != nullptr) {
        (void)xSemaphoreGive(hid_done_);
    }
    vTaskDelete(nullptr);
}

void Producer::producer_task() noexcept
{
    if (usb_ready_ == nullptr ||
        xSemaphoreTake(usb_ready_, kStartupTimeout) != pdTRUE ||
        !usb_host_installed_.load(std::memory_order_acquire)) {
        emit_disabled_once("USB_HOST_UNAVAILABLE");
        start_result_.store(false, std::memory_order_release);
        if (producer_ready_ != nullptr) {
            (void)xSemaphoreGive(producer_ready_);
        }
        teardown_usb_and_hid();
        producer_done_flag_.store(true, std::memory_order_release);
        if (producer_done_ != nullptr) {
            (void)xSemaphoreGive(producer_done_);
        }
        vTaskDelete(nullptr);
        return;
    }

    const usb_host_client_config_t configured_client = {
        .is_synchronous = false,
        .max_num_event_msg = 8U,
        .async = {
            .client_event_callback =
                [](const usb_host_client_event_msg_t *, void *) {},
            .callback_arg = this,
        },
    };
    usb_host_client_handle_t client = nullptr;
    const esp_err_t client_result =
        usb_host_client_register(&configured_client, &client);
    if (client_result != ESP_OK) {
        increment_fatal_once("USB_CLIENT_REGISTER_FAILED");
        emit_disabled_once("USB_CLIENT_REGISTER_FAILED");
        start_result_.store(false, std::memory_order_release);
        if (producer_ready_ != nullptr) {
            (void)xSemaphoreGive(producer_ready_);
        }
        teardown_usb_and_hid();
        producer_done_flag_.store(true, std::memory_order_release);
        if (producer_done_ != nullptr) {
            (void)xSemaphoreGive(producer_done_);
        }
        vTaskDelete(nullptr);
        return;
    }
    usb_client_.store(client, std::memory_order_release);

    const hid_host_driver_config_t hid_config = {
        .create_background_task = false,
        .task_priority = kHidEventPriority,
        .stack_size = kTaskStackBytes,
        .core_id = tskNO_AFFINITY,
        .callback = &Producer::hid_driver_callback,
        .callback_arg = this,
    };
    const esp_err_t hid_result = hid_host_install(&hid_config);
    if (hid_result != ESP_OK) {
        increment_fatal_once("HID_INSTALL_FAILED");
        emit_disabled_once("HID_INSTALL_FAILED");
        start_result_.store(false, std::memory_order_release);
        if (producer_ready_ != nullptr) {
            (void)xSemaphoreGive(producer_ready_);
        }
        teardown_usb_and_hid();
        producer_done_flag_.store(true, std::memory_order_release);
        if (producer_done_ != nullptr) {
            (void)xSemaphoreGive(producer_done_);
        }
        vTaskDelete(nullptr);
        return;
    }
    hid_installed_.store(true, std::memory_order_release);
    if (hid_start_ != nullptr) {
        (void)xSemaphoreGive(hid_start_);
    }
    start_result_.store(true, std::memory_order_release);
    if (producer_ready_ != nullptr) {
        (void)xSemaphoreGive(producer_ready_);
    }
    marker("P4_NANO_USB_KEYBOARD_HOST=READY");

    RawEvent event{};
    while (!stop_requested_.load(std::memory_order_acquire)) {
        while (raw_queue_ != nullptr &&
               xQueueReceive(raw_queue_, &event, 0U) == pdTRUE) {
            process_event(event);
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
        }
        const auto current_client =
            usb_client_.load(std::memory_order_acquire);
        if (current_client == nullptr) {
            break;
        }
        const esp_err_t event_result =
            usb_host_client_handle_events(current_client, kTaskPoll);
        if (event_result != ESP_OK && event_result != ESP_ERR_TIMEOUT &&
            !stop_requested_.load(std::memory_order_acquire)) {
            increment_fatal_once("USB_CLIENT_EVENT_FAILED");
            disable_for_fault("USB_CLIENT_EVENT_FAILED", true);
        }
    }

    while (raw_queue_ != nullptr &&
           xQueueReceive(raw_queue_, &event, 0U) == pdTRUE) {
        /* Stop is out-of-band; queued reports are deliberately discarded. */
    }
    teardown_usb_and_hid();
    producer_done_flag_.store(true, std::memory_order_release);
    if (producer_done_ != nullptr) {
        (void)xSemaphoreGive(producer_done_);
    }
    vTaskDelete(nullptr);
}

void Producer::hid_driver_callback(hid_host_device_handle_t handle,
                                   const hid_host_driver_event_t event,
                                   void *context) noexcept
{
    auto *producer = static_cast<Producer *>(context);
    if (producer == nullptr || event != HID_HOST_DRIVER_EVENT_CONNECTED ||
        !producer->accepting_.load(std::memory_order_acquire)) {
        return;
    }
    RawEvent raw{};
    raw.kind = RawEventKind::Connected;
    raw.handle = handle;
    (void)producer->enqueue_raw(raw);
}

void Producer::hid_interface_callback(hid_host_device_handle_t handle,
                                      const hid_host_interface_event_t event,
                                      void *context) noexcept
{
    auto *producer = static_cast<Producer *>(context);
    if (producer == nullptr) {
        return;
    }
    RawEvent raw{};
    raw.handle = handle;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        raw.kind = RawEventKind::InputReport;
        std::size_t length = 0U;
        const esp_err_t result = hid_host_device_get_raw_input_report_data(
            handle, raw.data, sizeof(raw.data), &length);
        if (result != ESP_OK) {
            raw.kind = RawEventKind::TransferError;
            raw.error = result;
        } else {
            raw.length = length;
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        raw.kind = RawEventKind::TransferError;
        raw.error = ESP_FAIL;
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        /* The HID driver requires close from the interface callback. */
        (void)hid_host_device_close(handle);
        /* Keep the active handle until the FIFO-ordered disconnect event is
         * consumed by the producer task.  Clearing it here would make the
         * queued disconnect indistinguishable from a stale device event. */
        raw.kind = RawEventKind::Disconnected;
    } else {
        return;
    }
    /* A shutdown may disable acceptance before HID delivers its mandatory
     * second close callback.  The close above is still required, but no new
     * raw event may enter the producer queue after the out-of-band stop. */
    if (!producer->accepting_.load(std::memory_order_acquire)) {
        return;
    }
    (void)producer->enqueue_raw(raw);
}

bool Producer::enqueue_raw(const RawEvent &event) noexcept
{
    if (!accepting_.load(std::memory_order_acquire) || raw_queue_ == nullptr) {
        return false;
    }
    if (xQueueSend(raw_queue_, &event, 0U) == pdTRUE) {
        return true;
    }
    internal_queue_overflows_.fetch_add(1U, std::memory_order_relaxed);
    source_cleanup_requested_.store(true, std::memory_order_release);
    fault_latched_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    marker("P4_NANO_USB_KEYBOARD_STATE=DISABLED reason=INTERNAL_QUEUE_FULL");
    if (producer_task_ != nullptr) {
        xTaskNotifyGive(producer_task_);
    }
    const auto client = usb_client_.load(std::memory_order_acquire);
    if (client != nullptr) {
        (void)usb_host_client_unblock(client);
    }
    return false;
}

void Producer::process_event(const RawEvent &event) noexcept
{
    switch (event.kind) {
    case RawEventKind::Connected:
        process_connected(event.handle);
        break;
    case RawEventKind::InputReport:
        process_input_report(event);
        break;
    case RawEventKind::Disconnected:
        process_disconnected(event.handle);
        break;
    case RawEventKind::TransferError:
        process_transfer_error(event.handle, event.error);
        break;
    }
}

void Producer::process_connected(hid_host_device_handle_t handle) noexcept
{
    if (handle == nullptr) {
        return;
    }
    hid_host_dev_params_t params{};
    const esp_err_t params_result = hid_host_device_get_params(handle, &params);
    if (params_result != ESP_OK) {
        increment_fatal_once("HID_PARAMS_FAILED");
        disable_for_fault("HID_PARAMS_FAILED", true);
        return;
    }
    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
        params.proto != HID_PROTOCOL_KEYBOARD) {
        return;
    }
    if (active_handle_.load(std::memory_order_acquire) != nullptr) {
        second_keyboard_rejected_.fetch_add(1U, std::memory_order_relaxed);
        if (!second_keyboard_reported_) {
            second_keyboard_reported_ = true;
            marker("P4_NANO_USB_KEYBOARD_DEVICE=SECOND_REJECTED");
        }
        return;
    }
    const hid_host_device_config_t config = {
        .callback = &Producer::hid_interface_callback,
        .callback_arg = this,
    };
    esp_err_t result = hid_host_device_open(handle, &config);
    if (result == ESP_OK) {
        active_handle_.store(handle, std::memory_order_release);
        active_open_.store(true, std::memory_order_release);
        result = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
    }
    if (result == ESP_OK) {
        result = hid_class_request_set_idle(handle, 0U, 0U);
    }
    if (result == ESP_OK) {
        result = hid_host_device_start(handle);
    }
    if (result != ESP_OK) {
        increment_fatal_once("HID_KEYBOARD_START_FAILED");
        if (active_open_.exchange(false, std::memory_order_acq_rel)) {
            (void)hid_host_device_close(handle);
        }
        active_handle_.store(nullptr, std::memory_order_release);
        disable_for_fault("HID_KEYBOARD_START_FAILED", false);
        return;
    }
    if (ever_connected_) {
        reconnects_.fetch_add(1U, std::memory_order_relaxed);
    }
    ever_connected_ = true;
    marker("P4_NANO_USB_KEYBOARD_DEVICE=CONNECTED");
}

void Producer::process_input_report(const RawEvent &event) noexcept
{
    if (!accepting_.load(std::memory_order_acquire) ||
        event.handle == nullptr ||
        active_handle_.load(std::memory_order_acquire) != event.handle) {
        return;
    }
    reports_received_.fetch_add(1U, std::memory_order_relaxed);
    if (event.length != HID_BOOT_KEYBOARD_REPORT_SIZE) {
        reports_malformed_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    std::array<hid_boot_keyboard_event_t, HID_BOOT_KEYBOARD_MAX_EVENTS>
        transitions{};
    const std::size_t transition_count = hid_boot_keyboard_process(
        &parser_state_, event.data, event.length, transitions.data(),
        transitions.size());
    std::array<np2_keyboard_input::Event,
               usb_hid_keyboard_adapter::kMaxNeutralEvents>
        neutral_events{};
    const auto result = adapter_.translate_report(
        transitions.data(), transition_count, event.data[0],
        neutral_events.data(), neutral_events.size());
    if (result.status == usb_hid_keyboard_adapter::TranslationStatus::ErrorUsage) {
        reports_error_usage_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (result.status == usb_hid_keyboard_adapter::TranslationStatus::InvalidInput ||
        result.status ==
            usb_hid_keyboard_adapter::TranslationStatus::CapacityInsufficient) {
        increment_fatal_once("PARSER_ADAPTER_FAILED");
        disable_for_fault("PARSER_ADAPTER_FAILED", true);
        return;
    }
    reports_clean_.fetch_add(1U, std::memory_order_relaxed);
    unsupported_usages_.fetch_add(
        static_cast<std::uint32_t>(result.unsupported_usages),
        std::memory_order_relaxed);
    neutral_events_generated_.fetch_add(
        static_cast<std::uint32_t>(result.written_events),
        std::memory_order_relaxed);
    for (std::size_t index = 0U; index < result.written_events; ++index) {
        if (!accepting_.load(std::memory_order_acquire)) {
            return;
        }
        const auto enqueue_result = bridge_->enqueue(neutral_events[index]);
        if (enqueue_result ==
            np2_keyboard_input_bridge::EnqueueResult::Enqueued) {
            neutral_events_enqueued_.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (enqueue_result == np2_keyboard_input_bridge::EnqueueResult::Full) {
            enqueue_full_.fetch_add(1U, std::memory_order_relaxed);
            bridge_failure_.store(true, std::memory_order_release);
            disable_for_fault("BRIDGE_FULL", false);
        } else if (enqueue_result ==
                   np2_keyboard_input_bridge::EnqueueResult::Quarantined) {
            enqueue_quarantined_.fetch_add(1U, std::memory_order_relaxed);
            bridge_failure_.store(true, std::memory_order_release);
            disable_for_fault("BRIDGE_QUARANTINED", false);
        } else {
            increment_fatal_once(bridge_result_name(enqueue_result));
            /* Invalid/NotInitialized is already an integration fault; do not
             * retry a source disconnect against the same failed bridge. */
            disable_for_fault("BRIDGE_INVALID", false);
        }
        return;
    }
}

void Producer::process_disconnected(hid_host_device_handle_t handle) noexcept
{
    if (handle == nullptr ||
        active_handle_.load(std::memory_order_acquire) != handle) {
        return;
    }
    if (bridge_calls_allowed_.load(std::memory_order_acquire) &&
        bridge_ != nullptr && !bridge_failure_.load(std::memory_order_acquire)) {
        (void)enqueue_source_disconnect();
    }
    disconnects_.fetch_add(1U, std::memory_order_relaxed);
    reset_keyboard_state();
    marker("P4_NANO_USB_KEYBOARD_DEVICE=DISCONNECTED");
}

void Producer::process_transfer_error(hid_host_device_handle_t handle,
                                      esp_err_t error) noexcept
{
    if (handle == nullptr || active_handle_.load(std::memory_order_acquire) != handle) {
        return;
    }
    transfer_errors_.fetch_add(1U, std::memory_order_relaxed);
    (void)error;
    stop_active_device();
    if (bridge_calls_allowed_.load(std::memory_order_acquire) &&
        bridge_ != nullptr && !bridge_failure_.load(std::memory_order_acquire)) {
        (void)enqueue_source_disconnect();
    }
    disconnects_.fetch_add(1U, std::memory_order_relaxed);
    reset_keyboard_state();
    marker("P4_NANO_USB_KEYBOARD_DEVICE=DISCONNECTED");
}

void Producer::reset_keyboard_state() noexcept
{
    hid_boot_keyboard_init(&parser_state_);
    adapter_.reset();
    active_handle_.store(nullptr, std::memory_order_release);
    active_open_.store(false, std::memory_order_release);
}

bool Producer::enqueue_source_disconnect() noexcept
{
    if (!bridge_calls_allowed_.load(std::memory_order_acquire) ||
        bridge_ == nullptr) {
        return false;
    }
    const auto result = bridge_->disconnect_source(
        np2_keyboard_input::kUsbKeyboardSource);
    if (result == np2_keyboard_input_bridge::EnqueueResult::Enqueued) {
        return true;
    }
    if (result == np2_keyboard_input_bridge::EnqueueResult::Full) {
        enqueue_full_.fetch_add(1U, std::memory_order_relaxed);
        bridge_failure_.store(true, std::memory_order_release);
    } else if (result == np2_keyboard_input_bridge::EnqueueResult::Quarantined) {
        enqueue_quarantined_.fetch_add(1U, std::memory_order_relaxed);
        bridge_failure_.store(true, std::memory_order_release);
    } else {
        increment_fatal_once(bridge_result_name(result));
    }
    return false;
}

void Producer::disable_for_fault(const char *reason,
                                 const bool source_cleanup) noexcept
{
    if (source_cleanup) {
        source_cleanup_requested_.store(true, std::memory_order_release);
    }
    accepting_.store(false, std::memory_order_release);
    fault_latched_.store(true, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    emit_disabled_once(reason);
    const auto client = usb_client_.load(std::memory_order_acquire);
    if (client != nullptr) {
        (void)usb_host_client_unblock(client);
    }
    if (producer_task_ != nullptr) {
        xTaskNotifyGive(producer_task_);
    }
}

void Producer::stop_active_device() noexcept
{
    const auto handle = active_handle_.load(std::memory_order_acquire);
    if (handle == nullptr) {
        return;
    }
    if (active_open_.load(std::memory_order_acquire)) {
        (void)hid_host_device_stop(handle);
        if (active_open_.exchange(false, std::memory_order_acq_rel)) {
            (void)hid_host_device_close(handle);
        }
    }
    active_handle_.store(nullptr, std::memory_order_release);
}

void Producer::teardown_usb_and_hid() noexcept
{
    accepting_.store(false, std::memory_order_release);
    if (source_cleanup_requested_.load(std::memory_order_acquire) &&
        bridge_calls_allowed_.load(std::memory_order_acquire) &&
        !bridge_failure_.load(std::memory_order_acquire) && bridge_ != nullptr) {
        (void)enqueue_source_disconnect();
    }
    stop_active_device();
    if (hid_task_ != nullptr) {
        xTaskNotifyGive(hid_task_);
    }
    /* Release the HID task even when startup failed before hid_host_install. */
    if (hid_start_ != nullptr) {
        (void)xSemaphoreGive(hid_start_);
    }
    if (hid_done_ != nullptr && hid_task_ != nullptr) {
        (void)xSemaphoreTake(hid_done_, kShutdownTimeout);
    }
    if (hid_installed_.exchange(false, std::memory_order_acq_rel)) {
        const esp_err_t result = hid_host_uninstall();
        if (result != ESP_OK) {
            increment_fatal_once("HID_UNINSTALL_FAILED");
        }
    }
    const auto client = usb_client_.exchange(nullptr, std::memory_order_acq_rel);
    if (client != nullptr) {
        const esp_err_t result = usb_host_client_deregister(client);
        if (result != ESP_OK) {
            increment_fatal_once("USB_CLIENT_DEREGISTER_FAILED");
        }
    }
    if (usb_host_installed_.load(std::memory_order_acquire)) {
        (void)xTaskNotifyGive(usb_task_);
    }
}

void Producer::emit_disabled_once(const char *reason) noexcept
{
    bool expected = false;
    if (disabled_reported_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        std::printf("P4_NANO_USB_KEYBOARD_STATE=DISABLED reason=%s\n",
                    reason == nullptr ? "UNKNOWN" : reason);
        std::fflush(stdout);
    }
}

void Producer::increment_fatal_once(const char *reason) noexcept
{
    bool expected = false;
    if (fatal_reported_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        producer_fatal_.fetch_add(1U, std::memory_order_relaxed);
        emit_disabled_once(reason);
    }
}

void Producer::emit_counters_once() noexcept
{
    bool expected = false;
    if (!counters_reported_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const auto value = counters();
    std::printf(
        "P4_NANO_USB_KEYBOARD_COUNTERS reports_received=%u "
        "reports_clean=%u reports_error_usage=%u reports_malformed=%u "
        "neutral_events_generated=%u neutral_events_enqueued=%u "
        "unsupported_usages=%u internal_queue_overflows=%u enqueue_full=%u "
        "enqueue_quarantined=%u disconnects=%u reconnects=%u "
        "second_keyboard_rejected=%u transfer_errors=%u producer_fatal=%u\n",
        static_cast<unsigned>(value.reports_received),
        static_cast<unsigned>(value.reports_clean),
        static_cast<unsigned>(value.reports_error_usage),
        static_cast<unsigned>(value.reports_malformed),
        static_cast<unsigned>(value.neutral_events_generated),
        static_cast<unsigned>(value.neutral_events_enqueued),
        static_cast<unsigned>(value.unsupported_usages),
        static_cast<unsigned>(value.internal_queue_overflows),
        static_cast<unsigned>(value.enqueue_full),
        static_cast<unsigned>(value.enqueue_quarantined),
        static_cast<unsigned>(value.disconnects),
        static_cast<unsigned>(value.reconnects),
        static_cast<unsigned>(value.second_keyboard_rejected),
        static_cast<unsigned>(value.transfer_errors),
        static_cast<unsigned>(value.producer_fatal));
    std::fflush(stdout);
}

} // namespace p4_nano_usb_keyboard
