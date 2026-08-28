#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace p4_nano_audio_output {

inline constexpr std::uint32_t kSampleRateHz = 48000U;
inline constexpr std::uint32_t kToneFrequencyHz = 1000U;
inline constexpr std::size_t kSamplesPerCycle = 48U;
inline constexpr std::size_t kToneSeconds = 2U;
inline constexpr std::size_t kExpectedFrames =
    kSampleRateHz * kToneSeconds;
inline constexpr std::size_t kBytesPerFrame = 4U;
inline constexpr std::size_t kExpectedBytes = kExpectedFrames * kBytesPerFrame;
inline constexpr std::size_t kBlockFrames = 240U;
inline constexpr std::size_t kBlockBytes = kBlockFrames * kBytesPerFrame;
inline constexpr std::size_t kLogicalBlockCount =
    kExpectedFrames / kBlockFrames;
inline constexpr std::size_t kDmaDescriptorCount = 4U;
inline constexpr std::size_t kDmaTotalBytes = kDmaDescriptorCount * kBlockBytes;
inline constexpr std::int16_t kTonePeak = 4096;

/*
 * Fixed integer table committed from sin(2*pi*n/48)*4096 rounded once.  The
 * runtime path has no libm dependency or floating-point phase accumulator.
 */
inline constexpr std::array<std::int16_t, kSamplesPerCycle> kToneTable = {
     0,   535,  1060,  1567,  2048,  2493,  2896,  3250,
  3547,  3784,  3956,  4061,  4096,  4061,  3956,  3784,
  3547,  3250,  2896,  2493,  2048,  1567,  1060,   535,
     0,  -535, -1060, -1567, -2048, -2493, -2896, -3250,
 -3547, -3784, -3956, -4061, -4096, -4061, -3956, -3784,
 -3547, -3250, -2896, -2493, -2048, -1567, -1060,  -535,
};

constexpr std::int16_t sample_at(std::size_t frame) noexcept
{
    return kToneTable[frame % kSamplesPerCycle];
}

constexpr void put_le16(std::uint8_t *out, std::int16_t sample) noexcept
{
    const auto value = static_cast<std::uint16_t>(sample);
    out[0] = static_cast<std::uint8_t>(value & 0xffU);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
}

inline void fill_block(std::size_t first_frame, std::uint8_t *out) noexcept
{
    for (std::size_t frame = 0; frame < kBlockFrames; ++frame) {
        const std::int16_t sample = sample_at(first_frame + frame);
        put_le16(out + frame * kBytesPerFrame, sample);
        put_le16(out + frame * kBytesPerFrame + 2U, sample);
    }
}

} // namespace p4_nano_audio_output
