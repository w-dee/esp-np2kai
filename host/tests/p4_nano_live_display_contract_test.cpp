#include <cassert>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "np2_presentation.h"
#include "p4_nano_live_display/p4_nano_live_display_contract.hpp"
#include "p4_nano_live_display_session/session_contract.hpp"

int main()
{
    using namespace p4_nano_live_display;
    namespace session = p4_nano_live_display_session;
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

    assert(session::source_geometry_valid(
        session::kSourceWidth, session::kSourceHeight, session::kSourcePitch,
        session::kSourceBpp, session::kPixelFormatRgb565Le));
    assert(!session::source_geometry_valid(
        session::kSourceWidth - 1U, session::kSourceHeight,
        session::kSourcePitch, session::kSourceBpp,
        session::kPixelFormatRgb565Le));
    assert(!session::source_geometry_valid(
        session::kSourceWidth, session::kSourceHeight - 1U,
        session::kSourcePitch, session::kSourceBpp,
        session::kPixelFormatRgb565Le));
    assert(!session::source_geometry_valid(
        session::kSourceWidth, session::kSourceHeight,
        session::kSourcePitch - sizeof(std::uint16_t), session::kSourceBpp,
        session::kPixelFormatRgb565Le));
    assert(!session::source_geometry_valid(
        session::kSourceWidth, session::kSourceHeight, session::kSourcePitch,
        15U, session::kPixelFormatRgb565Le));
    assert(!session::source_geometry_valid(
        session::kSourceWidth, session::kSourceHeight, session::kSourcePitch,
        session::kSourceBpp, 0U));

    session::FirstFrameGate gate;
    assert(!gate.visible);
    assert(gate.mark_valid_frame());
    assert(gate.visible);
    assert(!gate.mark_valid_frame());
    gate.reset();
    assert(gate.mark_valid_frame());

    static std::array<std::uint8_t, session::kSourceBytes> source{};
    static std::array<std::uint8_t, session::kPresentationSlotBytes> slot0{};
    static std::array<std::uint8_t, session::kPresentationSlotBytes> slot1{};
    const np2_presentation_slot_storage slots[NP2_PRESENTATION_SLOT_COUNT]{
        {slot0.data(), slot0.size()},
        {slot1.data(), slot1.size()},
    };
    np2_presentation_publisher publisher{};
    assert(np2_presentation_init(&publisher, slots) == NP2_PRESENTATION_OK);
    const np2_presentation_source_view frame{
        .ptr = source.data(),
        .width = session::kSourceWidth,
        .height = session::kSourceHeight,
        .pitch = session::kSourcePitch,
        .bpp = session::kSourceBpp,
        .pixel_format = NP2_PRESENTATION_PIXEL_FORMAT_RGB565LE,
        .source_generation = 1U,
        .source_update_sequence = 1U,
    };
    assert(np2_presentation_submit(&publisher, &frame) == NP2_PRESENTATION_OK);
    np2_presentation_frame_view acquired{};
    np2_presentation_token token{};
    assert(np2_presentation_acquire(&publisher, &acquired, &token) ==
           NP2_PRESENTATION_OK);
    assert(token.lease != 0U);
    assert(np2_presentation_release(&publisher, &token) == NP2_PRESENTATION_OK);
    assert(token.lease == 0U);
    assert(np2_presentation_release(&publisher, &token) ==
           NP2_PRESENTATION_INVALID_TOKEN);

    np2_presentation_source_view invalid = frame;
    invalid.pitch = session::kSourcePitch - sizeof(std::uint16_t);
    assert(np2_presentation_submit(&publisher, &invalid) ==
           NP2_PRESENTATION_INVALID_FRAME);

    /* A held lease can exhaust the only adequately sized slot.  This is a
     * normal latest-wins backpressure result, not a session-fatal publisher
     * error; a later release must make the publisher usable again. */
    const np2_presentation_slot_storage constrained_slots[
        NP2_PRESENTATION_SLOT_COUNT]{
        {slot0.data(), slot0.size()},
        {slot1.data(), sizeof(std::uint16_t)},
    };
    np2_presentation_publisher constrained_publisher{};
    assert(np2_presentation_init(&constrained_publisher,
                                 constrained_slots) == NP2_PRESENTATION_OK);
    assert(np2_presentation_submit(&constrained_publisher, &frame) ==
           NP2_PRESENTATION_OK);
    np2_presentation_frame_view held_view{};
    np2_presentation_token held_token{};
    assert(np2_presentation_acquire(&constrained_publisher, &held_view,
                                    &held_token) == NP2_PRESENTATION_OK);
    assert(np2_presentation_submit(&constrained_publisher, &frame) ==
           NP2_PRESENTATION_DROPPED);
    assert(np2_presentation_dropped_count(&constrained_publisher) == 1U);
    assert(np2_presentation_release(&constrained_publisher, &held_token) ==
           NP2_PRESENTATION_OK);
    assert(np2_presentation_acquire(&constrained_publisher, &held_view,
                                    &held_token) == NP2_PRESENTATION_NO_FRAME);
    return 0;
}
