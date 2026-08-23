#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "p4_nano_display/p4_nano_display_pattern.hpp"

int main()
{
    using namespace p4_nano_display;
    static_assert(kNativeStrideBytes == 1600);
    static_assert(kNativeFramebufferBytes == 2048000);
    std::vector<std::uint16_t> pixels(kNativePixelCount);
    assert(fill_static_pattern(pixels.data(), pixels.size()));
    assert(pixels[0] == 0xffe0);
    assert(pixels[kNativeWidth - 1] == 0xf81f);
    assert(pixels[(kNativeHeight - 1) * kNativeWidth] == 0x07ff);
    assert(pixels[kNativePixelCount - 1] == 0xfd20);
    assert(pixels[kNativeWidth / 2] == 0xf800);
    assert(pixels[(kNativeHeight / 2) * kNativeWidth + kNativeWidth - 1] == 0x07e0);
    assert(pixels[(kNativeHeight - 1) * kNativeWidth + kNativeWidth / 2] == 0x001f);
    assert(pixels[(kNativeHeight / 2) * kNativeWidth] == 0xffff);

    const std::uint32_t crc = static_pattern_crc32(pixels.data(), pixels.size());
    std::printf("P4_NANO_STATIC_PATTERN_CRC32=0x%08x\n", crc);
    assert(crc == 0x5383260aU);
    return 0;
}
