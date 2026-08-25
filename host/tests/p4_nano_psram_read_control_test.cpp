#include <cassert>
#include <cstdint>

#include "p4_nano_live_display/p4_nano_psram_read_control.hpp"

namespace {

std::uint32_t checksum_for_pattern()
{
    std::uint32_t lanes[8] = {
        p4_nano_psram_read_control::initial_lane(0U),
        p4_nano_psram_read_control::initial_lane(1U),
        p4_nano_psram_read_control::initial_lane(2U),
        p4_nano_psram_read_control::initial_lane(3U),
        p4_nano_psram_read_control::initial_lane(4U),
        p4_nano_psram_read_control::initial_lane(5U),
        p4_nano_psram_read_control::initial_lane(6U),
        p4_nano_psram_read_control::initial_lane(7U),
    };
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(
                     p4_nano_psram_read_control::kWordsPerSweep);
         index += 8U) {
        for (std::uint32_t lane = 0U; lane < 8U; ++lane) {
            const std::uint32_t word_index = index + lane;
            lanes[lane] = p4_nano_psram_read_control::fold_lane(
                lanes[lane],
                p4_nano_psram_read_control::pattern_word(word_index),
                word_index);
        }
    }
    return lanes[0] ^ lanes[1] ^ lanes[2] ^ lanes[3] ^ lanes[4] ^ lanes[5] ^
           lanes[6] ^ lanes[7];
}

} // namespace

int main()
{
    using namespace p4_nano_psram_read_control;
    static_assert(kBufferBytes == 4U * 1024U * 1024U);
    static_assert(kAlignmentBytes == 64U);
    assert(pattern_word(0U) != pattern_word(1U));
    const std::uint32_t checksum = checksum_for_pattern();
    assert(checksum == expected_sweep_checksum());
    assert(checksum == kExpectedSweepChecksum);

    const std::uint32_t original = pattern_word(4096U);
    const std::uint32_t changed = original ^ 0x01000000U;
    std::uint32_t original_lane = initial_lane(0U);
    std::uint32_t changed_lane = initial_lane(0U);
    original_lane = fold_lane(original_lane, original, 4096U);
    changed_lane = fold_lane(changed_lane, changed, 4096U);
    assert(original_lane != changed_lane);

    std::uint32_t sweeps = 0U;
    assert(derive_sweeps_per_relief(50000U, 1U, 250000U,
                                    kMaxSweepsPerRelief, &sweeps));
    assert(sweeps == 5U);
    assert(!derive_sweeps_per_relief(0U, 1U, 250000U,
                                     kMaxSweepsPerRelief, &sweeps));
    assert(!derive_sweeps_per_relief(1U, 0U, 250000U,
                                     kMaxSweepsPerRelief, &sweeps));
    assert(!derive_sweeps_per_relief(1U, 1U, 0U,
                                     kMaxSweepsPerRelief, &sweeps));
    assert(!derive_sweeps_per_relief(1U, 1U, 1U, 0U, &sweeps));
    assert(!derive_sweeps_per_relief(UINT64_MAX, UINT32_MAX, UINT32_MAX,
                                     kMaxSweepsPerRelief, &sweeps));
    assert(kBufferBytes == kWordsPerSweep * sizeof(std::uint32_t));
    double rate = 0.0;
    assert(!payload_mib_per_second(kBufferBytes, 0U, &rate));
    assert(payload_mib_per_second(2U * 1024U * 1024U, 1'000'000U, &rate));
    assert(rate == 2.0);
    return 0;
}
