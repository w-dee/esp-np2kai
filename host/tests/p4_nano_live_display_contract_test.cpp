#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "p4_nano_live_display/p4_nano_live_display_contract.hpp"

int main()
{
    using namespace p4_nano_live_display;
    static_assert(kSourceWidth == 640U);
    static_assert(kSourceHeight == 400U);
    static_assert(kSourceBytes == 512000U);
    static_assert(kPresentationSlotCount == 2U);
    static_assert(kPresentationSlotBytes == 512000U);
    static_assert(kNativeWidth == 800U);
    static_assert(kNativeHeight == 1280U);
    static_assert(kNativeFramebufferBytes == 2048000U);
    assert(std::strcmp(kCanonicalRotation, "CCW") == 0);
    assert(kPresentationSlotCount == 2U);
    assert(kPresentationSlotBytes * kPresentationSlotCount == 1024000U);
    return 0;
}
