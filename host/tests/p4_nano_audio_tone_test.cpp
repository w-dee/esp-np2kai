#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "np2_crc32.h"
#include "np2_sha256.h"
#include "p4_nano_audio_output/tone_model.hpp"

int main()
{
    using namespace p4_nano_audio_output;
    static_assert(kSamplesPerCycle == 48U);
    static_assert(kExpectedFrames == 96000U);
    static_assert(kExpectedBytes == 384000U);
    static_assert(kBlockFrames == 240U);
    static_assert(kBlockBytes == 960U);
    static_assert(kLogicalBlockCount == 400U);
    static_assert(kDmaTotalBytes == 3840U);

    std::array<std::uint8_t, kExpectedBytes> pcm{};
    for (std::size_t block = 0; block < kLogicalBlockCount; ++block) {
        fill_block(block * kBlockFrames,
                   pcm.data() + block * kBlockBytes);
    }
    for (std::size_t frame = 0; frame < kExpectedFrames; ++frame) {
        const std::size_t offset = frame * kBytesPerFrame;
        assert(std::memcmp(pcm.data() + offset, pcm.data() + offset + 2U, 2U) == 0);
        const auto raw = static_cast<std::uint16_t>(pcm[offset]) |
                         (static_cast<std::uint16_t>(pcm[offset + 1U]) << 8U);
        const auto sample = static_cast<std::int16_t>(raw);
        assert(sample == sample_at(frame));
        assert(sample <= kTonePeak && sample >= -kTonePeak);
    }
    for (std::size_t i = 0; i < kSamplesPerCycle; ++i) {
        assert(sample_at(i) == sample_at(i + kSamplesPerCycle));
    }

    np2_sha256_context sha{};
    np2_sha256_init(&sha);
    np2_sha256_update(&sha, pcm.data(), pcm.size());
    std::array<std::uint8_t, NP2_SHA256_DIGEST_SIZE> digest{};
    np2_sha256_final(&sha, digest.data());
    assert(np2_crc32_iso_hdlc(pcm.data(), pcm.size()) == UINT32_C(0x3054ef52));
    constexpr std::array<std::uint8_t, NP2_SHA256_DIGEST_SIZE> expected = {
        0xa0, 0xa4, 0xe3, 0x11, 0x82, 0xd3, 0x2b, 0xfd,
        0x51, 0x48, 0x5f, 0xfe, 0xf2, 0xee, 0x80, 0x30,
        0x6a, 0x57, 0x67, 0x0c, 0x56, 0x67, 0x87, 0x9d,
        0xfd, 0x35, 0x6d, 0x9d, 0x63, 0x6d, 0x91, 0xc5,
    };
    assert(digest == expected);
    std::printf("P4_AUDIO_TONE_HOST_TEST=PASS frames=%zu bytes=%zu blocks=%zu peak=%d clipping=0 crc32=0x3054ef52 sha256=a0a4e31182d32bfd51485ffef2ee80306a57670c5667879dfd356d9d636d91c5\n",
                kExpectedFrames, kExpectedBytes, kLogicalBlockCount,
                kTonePeak);
    return 0;
}
