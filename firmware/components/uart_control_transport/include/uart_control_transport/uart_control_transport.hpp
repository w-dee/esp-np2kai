#pragma once

#include "file_transfer/file_transfer.hpp"
#include "storage/storage.hpp"
#include "uart_control_transport/uart_control_transport.h"

namespace uart_control_transport {

esp_err_t start_with_storage(const uart_control_metadata_t *,
                             storage::Storage,
                             file_transfer::Limits);

} // namespace uart_control_transport
