#include "uart_control_transport/uart_control_transport.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "binary_data_plane/binary_data_plane.hpp"
#include "control_plane/control_plane.hpp"
#include "control_stream/control_stream.hpp"
#include "file_transfer/file_transfer.hpp"
#include "storage_ram/storage_ram.hpp"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr int kRxBufferSize = 2048;
#if defined(UART_FATFS_PROFILE)
// The FATFS profile is exercised with large stop-and-wait transfers. Reading
// a complete wire frame at once avoids unnecessary emulator iterations while
// leaving the normal RAM profile's scheduling unchanged.
constexpr std::size_t kReadChunkSize = kRxBufferSize;
#else
constexpr std::size_t kReadChunkSize = 64;
#endif
// File-transfer dispatch and binary frame handling share this task. Keep enough
// headroom for the bounded cJSON response objects plus the 1 KiB frame path.
constexpr std::size_t kControlTaskStackSize = 12288;
constexpr UBaseType_t kControlTaskPriority = tskIDLE_PRIORITY + 2;
constexpr uart_port_t kConsoleUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr TickType_t kReadWaitTicks = pdMS_TO_TICKS(100);

control_plane::ControlPlane s_control_plane;
binary_data_plane::TransferManager s_binary_manager;
storage_ram::StorageRam s_storage_ram;
storage::Storage s_storage{};
file_transfer::Limits s_file_transfer_limits{};
file_transfer::Service s_file_transfer;
control_plane::ServiceContext s_service_context;
control_stream::ControlStream s_control_stream;
control_plane::Metadata s_metadata{};
bool s_started = false;

bool write_machine(void *, const std::uint8_t *data, std::size_t length)
{
    bool success = false;
    flockfile(stdout);
    if (std::fflush(stdout) == 0) {
        success = uart_write_bytes(kConsoleUart, data, length) == static_cast<int>(length);
    }
    funlockfile(stdout);
    return success;
}

bool write_control(void *context, const char *data, std::size_t length)
{
    return write_machine(context,
                         reinterpret_cast<const std::uint8_t *>(data),
                         length);
}

int read_uart_chunk(std::uint8_t *buffer, std::size_t capacity)
{
    // Wait for the first byte while idle, then drain only what is already
    // buffered. Do not make a partial control/binary frame wait for the
    // requested chunk size before handing it to ControlStream.
    const int first_bytes = uart_read_bytes(kConsoleUart, buffer, 1, kReadWaitTicks);
    if (first_bytes <= 0) return first_bytes;

    std::size_t buffered_bytes = 0;
    if (uart_get_buffered_data_len(kConsoleUart, &buffered_bytes) != ESP_OK) {
        return first_bytes;
    }

    const std::size_t remaining_capacity = capacity - static_cast<std::size_t>(first_bytes);
    const std::size_t drain_bytes = buffered_bytes < remaining_capacity ?
        buffered_bytes : remaining_capacity;
    if (drain_bytes == 0) return first_bytes;

    const int drained_bytes = uart_read_bytes(kConsoleUart,
                                              buffer + first_bytes,
                                              drain_bytes,
                                              0);
    return first_bytes + (drained_bytes > 0 ? drained_bytes : 0);
}

void control_task(void *)
{
    const binary_data_plane::OutputSink binary_sink{nullptr, write_machine};
    binary_data_plane::init(&s_binary_manager, binary_sink);
    s_file_transfer.init(s_storage, &s_binary_manager, s_file_transfer_limits);
    s_service_context = control_plane::ServiceContext{&s_binary_manager, &s_file_transfer};
    const control_plane::OutputSink control_sink{nullptr, write_control};
    control_plane::init(&s_control_plane,
                        control_sink,
                        s_metadata,
                        &s_service_context);
    control_stream::init(&s_control_stream,
                         &s_control_plane,
                         &s_binary_manager);

    std::printf("ESP-NP2KAI UART CONTROL READY\n");
    std::fflush(stdout);

    std::uint8_t read_buffer[kReadChunkSize];
    while (true) {
        const int bytes_read = read_uart_chunk(read_buffer, sizeof(read_buffer));
        if (bytes_read > 0) {
            const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            control_stream::feed(&s_control_stream,
                                 read_buffer,
                                 static_cast<std::size_t>(bytes_read),
                                 now_ms);
            binary_data_plane::poll(
                &s_binary_manager,
                static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
        } else {
            binary_data_plane::poll(
                &s_binary_manager,
                static_cast<std::uint32_t>(esp_timer_get_time() / 1000));
        }
    }
}

esp_err_t start_impl(const uart_control_metadata_t *metadata)
{
    if (s_started) {
        return ESP_OK;
    }

    s_metadata = control_plane::Metadata{
        metadata->project,
        metadata->firmware_version,
        metadata->idf_version,
        metadata->target,
    };

    if (!uart_is_driver_installed(kConsoleUart)) {
        const esp_err_t install_result = uart_driver_install(kConsoleUart,
                                                              kRxBufferSize,
                                                              0,
                                                              0,
                                                              nullptr,
                                                              0);
        if (install_result != ESP_OK) {
            return install_result;
        }
    }

    // Respect ESP-IDF's configured UART number, pins, and baud rate. This only
    // switches the existing console VFS to the installed driver.
    uart_vfs_dev_use_driver(kConsoleUart);

    const BaseType_t task_result = xTaskCreate(control_task,
                                               "uart_control",
                                               kControlTaskStackSize,
                                               nullptr,
                                               kControlTaskPriority,
                                               nullptr);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

} // namespace

namespace uart_control_transport {

esp_err_t start_with_storage(const uart_control_metadata_t *metadata,
                             storage::Storage backend,
                             file_transfer::Limits limits)
{
    if (metadata == nullptr || backend.context == nullptr || backend.stat == nullptr ||
        backend.list_page == nullptr || backend.begin_read == nullptr ||
        backend.begin_write == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) return ESP_OK;
    s_storage = backend;
    s_file_transfer_limits = limits;
    return start_impl(metadata);
}

} // namespace uart_control_transport

extern "C" esp_err_t uart_control_transport_start(const uart_control_metadata_t *metadata)
{
    if (metadata == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_OK;
    s_storage_ram.init();
    return uart_control_transport::start_with_storage(
        metadata, s_storage_ram.api(), file_transfer::Limits{});
}

extern "C" bool uart_control_transport_write(const char *data, size_t length)
{
    if ((data == nullptr) || (length == 0)) {
        return false;
    }
    return write_machine(nullptr,
                         reinterpret_cast<const std::uint8_t *>(data),
                         length);
}
