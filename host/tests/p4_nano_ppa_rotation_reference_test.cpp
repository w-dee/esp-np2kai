#include "p4_nano_live_display/p4_nano_ppa_rotation_reference.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

std::uint32_t crc32_iso_hdlc(const std::uint8_t *bytes, std::size_t length)
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U &
                                  (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

} // namespace

int main()
{
    using namespace p4_nano_ppa_rotation;

    static_assert(kSourceWidth == 640U);
    static_assert(kSourceHeight == 400U);
    static_assert(kOutputWidth == 400U);
    static_assert(kOutputHeight == 640U);
    static_assert(kSourceBytes == 512000U);
    static_assert(kOutputBytes == 512000U);
    static_assert(kExpectedSourceCrc == 0x8dadbf82U);
    static_assert(kExpectedOutputCrc == 0x379511d7U);

    const std::vector<std::uint16_t> source(kSourceWidth * kSourceHeight,
                                            0U);
    std::vector<std::uint16_t> output(kOutputWidth * kOutputHeight, 0U);
    auto mutable_source = source;
    for (std::uint32_t sy = 0U; sy < kSourceHeight; ++sy) {
        for (std::uint32_t sx = 0U; sx < kSourceWidth; ++sx) {
            mutable_source[source_index(sx, sy)] = static_cast<std::uint16_t>(
                (sy * kSourceWidth + sx) * 17U + 3U);
        }
    }

    std::uint32_t dx = 0U;
    std::uint32_t dy = 0U;
    map_source_to_output(0U, 0U, &dx, &dy);
    assert(dx == 0U && dy == 639U);
    map_source_to_output(639U, 0U, &dx, &dy);
    assert(dx == 0U && dy == 0U);
    map_source_to_output(0U, 399U, &dx, &dy);
    assert(dx == 399U && dy == 639U);
    map_source_to_output(639U, 399U, &dx, &dy);
    assert(dx == 399U && dy == 0U);
    map_source_to_output(17U, 23U, &dx, &dy);
    assert(dx == 23U && dy == 622U);

    for (std::uint32_t sy = 0U; sy < kSourceHeight; ++sy) {
        for (std::uint32_t sx = 0U; sx < kSourceWidth; ++sx) {
            map_source_to_output(sx, sy, &dx, &dy);
            output[output_index(dx, dy)] = mutable_source[source_index(sx, sy)];
        }
    }
    assert(reference_matches(mutable_source.data(), output.data()));
    assert(output[output_index(23U, 622U)] ==
           mutable_source[source_index(17U, 23U)]);

    const auto crc = crc32_iso_hdlc(
        reinterpret_cast<const std::uint8_t *>(output.data()), kOutputBytes);
    assert(crc != 0U);
    return 0;
}
