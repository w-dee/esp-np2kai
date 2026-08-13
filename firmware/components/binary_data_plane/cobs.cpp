#include "cobs.hpp"

namespace binary_data_plane::cobs {

bool encode(const std::uint8_t *input,
            std::size_t input_length,
            std::uint8_t *output,
            std::size_t output_capacity,
            std::size_t *output_length)
{
    if (output_length == nullptr || output == nullptr || (input == nullptr && input_length != 0)) {
        return false;
    }

    std::size_t read_index = 0;
    std::size_t write_index = 1;
    std::size_t code_index = 0;
    std::uint8_t code = 1;

    if (output_capacity == 0) {
        return false;
    }

    while (read_index < input_length) {
        if (input[read_index] == 0) {
            if (code_index >= output_capacity) {
                return false;
            }
            output[code_index] = code;
            code = 1;
            code_index = write_index++;
            ++read_index;
            if (code_index >= output_capacity && read_index < input_length) {
                return false;
            }
        } else {
            if (write_index >= output_capacity) {
                return false;
            }
            output[write_index++] = input[read_index++];
            if (++code == 0xff) {
                output[code_index] = code;
                code = 1;
                code_index = write_index++;
                if (code_index > output_capacity) {
                    return false;
                }
            }
        }
    }

    if (code_index >= output_capacity) {
        return false;
    }
    output[code_index] = code;
    *output_length = write_index;
    return true;
}

bool decode(const std::uint8_t *input,
            std::size_t input_length,
            std::uint8_t *output,
            std::size_t output_capacity,
            std::size_t *output_length)
{
    if (output_length == nullptr || input == nullptr || output == nullptr || input_length == 0) {
        return false;
    }

    std::size_t read_index = 0;
    std::size_t write_index = 0;
    while (read_index < input_length) {
        const std::uint8_t code = input[read_index++];
        if (code == 0 || read_index + static_cast<std::size_t>(code - 1) > input_length) {
            return false;
        }

        const std::size_t copy_length = code - 1;
        if (write_index + copy_length > output_capacity) {
            return false;
        }
        for (std::size_t index = 0; index < copy_length; ++index) {
            output[write_index++] = input[read_index++];
        }

        if (code != 0xff && read_index < input_length) {
            if (write_index >= output_capacity) {
                return false;
            }
            output[write_index++] = 0;
        }
    }

    *output_length = write_index;
    return true;
}

} // namespace binary_data_plane::cobs
