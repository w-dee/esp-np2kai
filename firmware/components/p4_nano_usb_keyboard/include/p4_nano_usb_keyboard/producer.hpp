#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_boot_keyboard/hid_boot_keyboard.h"
#include "np2_keyboard_input_bridge/keyboard_input_bridge.hpp"
#include "usb_hid_keyboard_adapter/adapter.hpp"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

namespace p4_nano_usb_keyboard {

inline constexpr std::size_t kRawQueueCapacity = 24U;
inline constexpr std::size_t kRawReportBytes = 64U;
inline constexpr std::size_t kTaskStackBytes = 4096U;
inline constexpr UBaseType_t kUsbLibraryPriority = 2U;
inline constexpr UBaseType_t kProducerPriority = 4U;
inline constexpr UBaseType_t kHidEventPriority = 5U;

enum class State : std::uint8_t {
    Stopped,
    Starting,
    Ready,
    Disabled,
    Stopping,
    TeardownFailed,
};

enum class StopResult : std::uint8_t {
    Clean,
    Failed,
};

enum class RawEventKind : std::uint8_t {
    Connected,
    InputReport,
    Disconnected,
    TransferError,
};

struct RawEvent final {
    RawEventKind kind = RawEventKind::Connected;
    hid_host_device_handle_t handle = nullptr;
    std::size_t length = 0U;
    esp_err_t error = ESP_OK;
    std::uint8_t data[kRawReportBytes]{};
};

struct Counters final {
    std::uint32_t reports_received = 0U;
    std::uint32_t reports_clean = 0U;
    std::uint32_t reports_error_usage = 0U;
    std::uint32_t reports_malformed = 0U;
    std::uint32_t neutral_events_generated = 0U;
    std::uint32_t neutral_events_enqueued = 0U;
    std::uint32_t unsupported_usages = 0U;
    std::uint32_t internal_queue_overflows = 0U;
    std::uint32_t enqueue_full = 0U;
    std::uint32_t enqueue_quarantined = 0U;
    std::uint32_t disconnects = 0U;
    std::uint32_t reconnects = 0U;
    std::uint32_t second_keyboard_rejected = 0U;
    std::uint32_t transfer_errors = 0U;
    std::uint32_t producer_fatal = 0U;
};

class Producer final {
public:
    Producer() noexcept = default;
    Producer(const Producer &) = delete;
    Producer &operator=(const Producer &) = delete;

    /* Start is bounded and returns false only when the optional USB path was
     * disabled.  A false result is degraded mode; the NP2 runtime continues. */
    bool start(np2_keyboard_input_bridge::KeyboardInputBridge &bridge) noexcept;

    /* Out-of-band, idempotent, nonblocking shutdown request. */
    void request_stop() noexcept;

    /* Wait for producer-owned tasks and USB/HID teardown with bounded waits. */
    StopResult stop() noexcept;

    State state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    bool accepting() const noexcept
    {
        return accepting_.load(std::memory_order_acquire);
    }

    bool device_connected() const noexcept
    {
        return active_handle_.load(std::memory_order_acquire) != nullptr &&
               active_open_.load(std::memory_order_acquire);
    }

    Counters counters() const noexcept;

private:
    static void usb_library_task_entry(void *context) noexcept;
    static void hid_event_task_entry(void *context) noexcept;
    static void producer_task_entry(void *context) noexcept;
    static void hid_driver_callback(hid_host_device_handle_t handle,
                                    hid_host_driver_event_t event,
                                    void *context) noexcept;
    static void hid_interface_callback(hid_host_device_handle_t handle,
                                       hid_host_interface_event_t event,
                                       void *context) noexcept;

    void usb_library_task() noexcept;
    void hid_event_task() noexcept;
    void producer_task() noexcept;

    bool enqueue_raw(const RawEvent &event) noexcept;
    bool initialize_usb_and_hid() noexcept;
    void process_event(const RawEvent &event) noexcept;
    void process_connected(hid_host_device_handle_t handle) noexcept;
    void process_input_report(const RawEvent &event) noexcept;
    void process_disconnected(hid_host_device_handle_t handle) noexcept;
    void process_transfer_error(hid_host_device_handle_t handle,
                                esp_err_t error) noexcept;
    void reset_keyboard_state() noexcept;
    bool enqueue_source_disconnect() noexcept;
    void disable_for_fault(const char *reason,
                           bool source_cleanup) noexcept;
    void stop_active_device() noexcept;
    bool teardown_usb_and_hid() noexcept;
    void emit_disabled_once(const char *reason) noexcept;
    void emit_counters_once() noexcept;
    void increment_fatal_once(const char *reason) noexcept;
    void mark_teardown_failed(const char *reason) noexcept;

    np2_keyboard_input_bridge::KeyboardInputBridge *bridge_ = nullptr;
    QueueHandle_t raw_queue_ = nullptr;
    SemaphoreHandle_t usb_ready_ = nullptr;
    SemaphoreHandle_t usb_done_ = nullptr;
    SemaphoreHandle_t hid_start_ = nullptr;
    SemaphoreHandle_t hid_done_ = nullptr;
    SemaphoreHandle_t producer_ready_ = nullptr;
    SemaphoreHandle_t producer_done_ = nullptr;
    TaskHandle_t usb_task_ = nullptr;
    TaskHandle_t hid_task_ = nullptr;
    TaskHandle_t producer_task_ = nullptr;
    std::atomic<usb_host_client_handle_t> usb_client_{nullptr};
    std::atomic<hid_host_device_handle_t> active_handle_{nullptr};
    std::atomic<bool> active_open_{false};
    std::atomic<bool> usb_host_installed_{false};
    std::atomic<bool> hid_installed_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> producer_done_flag_{false};
    std::atomic<bool> hid_done_flag_{false};
    std::atomic<bool> usb_done_flag_{false};
    std::atomic<bool> start_result_{false};
    std::atomic<bool> fault_latched_{false};
    std::atomic<bool> source_cleanup_requested_{false};
    std::atomic<bool> bridge_failure_{false};
    std::atomic<bool> bridge_calls_allowed_{false};
    std::atomic<bool> teardown_failed_{false};
    std::atomic<bool> teardown_failure_reported_{false};
    std::atomic<State> state_{State::Stopped};
    std::atomic<std::uint32_t> reports_received_{0U};
    std::atomic<std::uint32_t> reports_clean_{0U};
    std::atomic<std::uint32_t> reports_error_usage_{0U};
    std::atomic<std::uint32_t> reports_malformed_{0U};
    std::atomic<std::uint32_t> neutral_events_generated_{0U};
    std::atomic<std::uint32_t> neutral_events_enqueued_{0U};
    std::atomic<std::uint32_t> unsupported_usages_{0U};
    std::atomic<std::uint32_t> internal_queue_overflows_{0U};
    std::atomic<std::uint32_t> enqueue_full_{0U};
    std::atomic<std::uint32_t> enqueue_quarantined_{0U};
    std::atomic<std::uint32_t> disconnects_{0U};
    std::atomic<std::uint32_t> reconnects_{0U};
    std::atomic<std::uint32_t> second_keyboard_rejected_{0U};
    std::atomic<std::uint32_t> transfer_errors_{0U};
    std::atomic<std::uint32_t> producer_fatal_{0U};
    std::atomic<bool> disabled_reported_{false};
    std::atomic<bool> fatal_reported_{false};
    std::atomic<bool> counters_reported_{false};
    bool ever_connected_ = false;
    bool second_keyboard_reported_ = false;
    hid_boot_keyboard_state_t parser_state_{};
    usb_hid_keyboard_adapter::Adapter adapter_{};
};

} // namespace p4_nano_usb_keyboard
