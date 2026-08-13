#pragma once

#include <cstddef>
#include <cstdint>

namespace binary_data_plane::crc32 {

std::uint32_t init();
std::uint32_t update(std::uint32_t running,
                     const std::uint8_t *data,
                     std::size_t length);
std::uint32_t finish(std::uint32_t running);
std::uint32_t calculate(const std::uint8_t *data, std::size_t length);

} // namespace binary_data_plane::crc32
