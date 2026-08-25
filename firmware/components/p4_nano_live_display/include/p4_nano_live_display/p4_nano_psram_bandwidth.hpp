#pragma once

#include <cstddef>
#include <cstdint>

namespace p4_nano_psram_bandwidth {

enum class Operation : std::uint8_t {
    Read,
    Write16,
    Write32,
    Memcpy,
    RowMemcpy,
    Proxy,
};

const char *operation_name(Operation operation) noexcept;

std::uint32_t run_kernel(Operation operation, std::uint8_t *source,
                         std::uint8_t *destination,
                         std::uint32_t pattern) noexcept;

std::size_t read_bytes(Operation operation) noexcept;
std::size_t write_bytes(Operation operation) noexcept;

} // namespace p4_nano_psram_bandwidth
