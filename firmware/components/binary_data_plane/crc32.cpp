#include "crc32.hpp"

namespace binary_data_plane::crc32 {
namespace {

constexpr std::uint32_t kReflectedPolynomial = 0xedb88320u;

} // namespace

std::uint32_t init()
{
    return 0xffffffffu;
}

std::uint32_t update(std::uint32_t running,
                     const std::uint8_t *data,
                     std::size_t length)
{
    if (data == nullptr && length != 0) {
        return running;
    }

    for (std::size_t index = 0; index < length; ++index) {
        running ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            running = (running & 1u) != 0 ?
                (running >> 1) ^ kReflectedPolynomial : (running >> 1);
        }
    }
    return running;
}

std::uint32_t finish(std::uint32_t running)
{
    return running ^ 0xffffffffu;
}

std::uint32_t calculate(const std::uint8_t *data, std::size_t length)
{
    return finish(update(init(), data, length));
}

} // namespace binary_data_plane::crc32
