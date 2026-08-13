#pragma once

#include <cstddef>
#include <cstdint>

namespace binary_data_plane::cobs {

bool encode(const std::uint8_t *input,
            std::size_t input_length,
            std::uint8_t *output,
            std::size_t output_capacity,
            std::size_t *output_length);

bool decode(const std::uint8_t *input,
            std::size_t input_length,
            std::uint8_t *output,
            std::size_t output_capacity,
            std::size_t *output_length);

} // namespace binary_data_plane::cobs
