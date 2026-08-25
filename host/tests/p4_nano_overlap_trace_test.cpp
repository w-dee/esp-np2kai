#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "p4_nano_live_display/p4_nano_overlap_trace.hpp"

namespace {

using p4_nano_overlap::SubmitInterval;
using p4_nano_overlap::TransformInterval;

TransformInterval transform(std::uint64_t start, std::uint64_t end,
                            std::uint64_t published, bool measured = true)
{
    TransformInterval value{};
    value.transform_start_us = start;
    value.transform_end_us = end;
    value.source_update_sequence = static_cast<std::uint32_t>(published);
    value.published_sequence = published;
    value.measured = measured;
    return value;
}

SubmitInterval submit(std::uint64_t start, std::uint64_t end,
                      std::uint64_t published)
{
    SubmitInterval value{};
    value.start_us = start;
    value.end_us = end;
    value.source_update_sequence = static_cast<std::uint32_t>(published);
    value.published_sequence = published;
    return value;
}

void test_interval_edges()
{
    const TransformInterval t = transform(100U, 200U, 1U);
    {
        const SubmitInterval s[] = {submit(0U, 50U, 1U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 0U);
    }
    {
        const SubmitInterval s[] = {submit(200U, 250U, 2U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 0U);
    }
    {
        const SubmitInterval s[] = {submit(50U, 125U, 3U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 25U);
    }
    {
        const SubmitInterval s[] = {submit(175U, 250U, 4U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 25U);
    }
    {
        const SubmitInterval s[] = {submit(125U, 175U, 5U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 50U);
    }
    {
        const SubmitInterval s[] = {submit(0U, 300U, 6U)};
        assert(p4_nano_overlap::calculate_overlap(t, s).total_us == 100U);
    }
    {
        const SubmitInterval s[] = {
            submit(0U, 110U, 7U), submit(150U, 180U, 8U),
            submit(220U, 250U, 9U)};
        const auto overlap = p4_nano_overlap::calculate_overlap(t, s);
        assert(overlap.total_us == 40U);
        assert(overlap.intersecting_submit_count == 2U);
    }
}

void test_analysis_identity_and_warmup()
{
    const SubmitInterval submits[] = {
        submit(0U, 10U, 10U), submit(20U, 30U, 11U),
        submit(70U, 80U, 12U)};
    const TransformInterval transforms[] = {
        transform(10U, 20U, 10U, false),
        transform(30U, 45U, 11U, true),
        transform(60U, 70U, 12U, true)};
    const auto result = p4_nano_overlap::analyze<2U>(submits, transforms);
    assert(result.transform_count == 3U);
    assert(result.measured_transform_count == 2U);
    assert(result.overlapping_transform_count == 0U);
    assert(result.zero_overlap_transform_count == 2U);
    assert(result.matched_transform_count == 3U);
    assert(result.unmatched_acquired_count == 0U);
    assert(result.unmatched_submit_count == 0U);
    assert(result.submit_intervals_non_overlapping);
    assert(result.submit_source_sequences_monotonic);
    assert(result.transform_published_sequences_monotonic);
}

void test_different_sequences_and_capacity()
{
    const SubmitInterval submits[] = {submit(10U, 20U, 2U)};
    const TransformInterval transforms[] = {transform(15U, 25U, 3U)};
    const auto result = p4_nano_overlap::analyze<1U>(submits, transforms);
    assert(result.overlapping_transform_count == 1U);
    assert(result.unmatched_acquired_count == 1U);
    assert(result.unmatched_submit_count == 1U);

    SubmitInterval metadata_submit = submit(10U, 20U, 4U);
    TransformInterval metadata_transform = transform(15U, 25U, 4U);
    metadata_transform.source_update_sequence = 99U;
    const auto metadata_result = p4_nano_overlap::analyze<1U>(
        std::span<const SubmitInterval>(&metadata_submit, 1U),
        std::span<const TransformInterval>(&metadata_transform, 1U));
    assert(metadata_result.matched_transform_count == 1U);
    assert(metadata_result.unmatched_acquired_count == 0U);
    assert(metadata_result.sequence_metadata_mismatch_count == 1U);

    std::array<SubmitInterval, 1U> bounded{};
    std::size_t stored = 0U;
    bool overflow = false;
    assert(p4_nano_overlap::append_bounded(
        bounded, stored, submit(0U, 1U, 1U), overflow));
    assert(!p4_nano_overlap::append_bounded(
        bounded, stored, submit(1U, 2U, 2U), overflow));
    assert(stored == 1U);
    assert(overflow);
}

void test_concurrency_assumption()
{
    const SubmitInterval non_overlapping[] = {
        submit(0U, 10U, 1U), submit(10U, 20U, 2U)};
    assert(p4_nano_overlap::max_concurrent_submit_intervals(
               non_overlapping) == 1U);
    const SubmitInterval overlapping[] = {
        submit(0U, 10U, 1U), submit(5U, 20U, 2U)};
    assert(p4_nano_overlap::max_concurrent_submit_intervals(overlapping) ==
           2U);
    assert(p4_nano_overlap::intervals_intersect(overlapping[0],
                                                overlapping[1]));
}

} // namespace

int main()
{
    test_interval_edges();
    test_analysis_identity_and_warmup();
    test_different_sequences_and_capacity();
    test_concurrency_assumption();
    return 0;
}
