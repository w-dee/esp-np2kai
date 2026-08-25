#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "p4_nano_live_display/p4_nano_psram_bandwidth.hpp"

namespace {

constexpr std::size_t kBytes = 4U * 1024U * 1024U;

void fill_source(std::uint8_t *source)
{
    for (std::size_t index = 0; index < kBytes; ++index) {
        source[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xffU);
    }
}

} // namespace

int main()
{
    auto *source = static_cast<std::uint8_t *>(std::aligned_alloc(64U, kBytes));
    auto *destination =
        static_cast<std::uint8_t *>(std::aligned_alloc(64U, kBytes));
    assert(source != nullptr);
    assert(destination != nullptr);
    fill_source(source);
    std::memset(destination, 0, kBytes);

    using p4_nano_psram_bandwidth::Operation;
    const std::uint32_t read_guard =
        p4_nano_psram_bandwidth::run_kernel(Operation::Read, source,
                                            destination, 0U);
    assert(read_guard == p4_nano_psram_bandwidth::run_kernel(
                            Operation::Read, source, destination, 0U));

    constexpr std::uint32_t pattern16 = 0x10203040U;
    p4_nano_psram_bandwidth::run_kernel(Operation::Write16, source, destination,
                                        pattern16);
    const auto *out16 = reinterpret_cast<const std::uint16_t *>(destination);
    for (std::size_t index = 0; index < kBytes / sizeof(std::uint16_t); ++index) {
        assert(out16[index] == static_cast<std::uint16_t>(
                                   pattern16 ^ static_cast<std::uint32_t>(index)));
    }

    constexpr std::uint32_t pattern32 = 0x50607080U;
    p4_nano_psram_bandwidth::run_kernel(Operation::Write32, source, destination,
                                        pattern32);
    const auto *out32 = reinterpret_cast<const std::uint32_t *>(destination);
    for (std::size_t index = 0; index < kBytes / sizeof(std::uint32_t); ++index) {
        const std::uint32_t pixel =
            pattern32 ^ static_cast<std::uint32_t>(index);
        assert(out32[index] == (pixel | (pixel << 16U)));
    }

    p4_nano_psram_bandwidth::run_kernel(Operation::Memcpy, source, destination,
                                        0U);
    assert(std::memcmp(source, destination, kBytes) == 0);

    std::memset(destination, 0, kBytes);
    p4_nano_psram_bandwidth::run_kernel(Operation::RowMemcpy, source, destination,
                                        0U);
    assert(std::memcmp(source, destination, 512'000U) == 0);

    std::memset(destination, 0, kBytes);
    p4_nano_psram_bandwidth::run_kernel(Operation::Proxy, source, destination,
                                        0U);
    const auto *source_pixels = reinterpret_cast<const std::uint16_t *>(source);
    const auto *destination_pixels =
        reinterpret_cast<const std::uint16_t *>(destination);
    constexpr std::size_t source_pixel_count = 512'000U / sizeof(std::uint16_t);
    for (std::size_t index = 0; index < source_pixel_count; ++index) {
        for (std::size_t offset = 0; offset < 4U; ++offset) {
            assert(destination_pixels[index * 4U + offset] == source_pixels[index]);
        }
    }

    std::free(source);
    std::free(destination);
    return 0;
}
