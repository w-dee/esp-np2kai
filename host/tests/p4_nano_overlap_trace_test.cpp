#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "p4_nano_live_display/p4_nano_overlap_trace.hpp"

namespace {

using p4_nano_overlap::SubmitInterval;
using p4_nano_overlap::TransformInterval;
using p4_nano_overlap::PccoreInterval;

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

PccoreInterval pccore(std::uint64_t start, std::uint64_t end,
                      std::uint32_t call_index)
{
    PccoreInterval value{};
    value.start_us = start;
    value.end_us = end;
    value.call_index = call_index;
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
        const auto overlap = p4_nano_overlap::calculate_overlap(t, s);
        assert(overlap.total_us == 25U);
        assert(overlap.intersecting_submit_count == 1U);
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
    p4_nano_overlap::Analysis<2U> result{};
    p4_nano_overlap::analyze<2U>(submits, transforms, result);
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

void test_submit_trace_completeness_and_intersection_count()
{
    assert(p4_nano_overlap::submit_trace_complete(3U, 3U));
    assert(!p4_nano_overlap::submit_trace_complete(2U, 3U));
    assert(!p4_nano_overlap::submit_trace_complete(4U, 3U));

    const SubmitInterval submits[] = {
        submit(90U, 110U, 1U), submit(130U, 160U, 2U)};
    const TransformInterval transforms[] = {transform(100U, 150U, 1U)};
    p4_nano_overlap::Analysis<1U> result{};
    p4_nano_overlap::analyze<1U>(submits, transforms, result);
    assert(result.intersecting_submit_count_stored == 1U);
    assert(result.intersecting_submit_count[0] == 2U);
    assert(result.overlapping_transform_count == 1U);
    assert(result.single_submit_overlap_transform_count == 0U);
    assert(result.multiple_submit_overlap_transform_count == 1U);

    const SubmitInterval one_submit[] = {submit(110U, 120U, 1U)};
    p4_nano_overlap::Analysis<1U> one_result{};
    p4_nano_overlap::analyze<1U>(one_submit, transforms, one_result);
    assert(one_result.intersecting_submit_count[0] == 1U);
    assert(one_result.single_submit_overlap_transform_count == 1U);
    assert(one_result.multiple_submit_overlap_transform_count == 0U);

    const SubmitInterval boundary_submit[] = {submit(150U, 170U, 2U)};
    const auto boundary = p4_nano_overlap::calculate_overlap(
        transforms[0], boundary_submit);
    assert(boundary.total_us == 0U);
    assert(boundary.intersecting_submit_count == 0U);
}

void test_different_sequences_and_capacity()
{
    const SubmitInterval submits[] = {submit(10U, 20U, 2U)};
    const TransformInterval transforms[] = {transform(15U, 25U, 3U)};
    p4_nano_overlap::Analysis<1U> result{};
    p4_nano_overlap::analyze<1U>(submits, transforms, result);
    assert(result.overlapping_transform_count == 1U);
    assert(result.unmatched_acquired_count == 1U);
    assert(result.unmatched_submit_count == 1U);

    SubmitInterval metadata_submit = submit(10U, 20U, 4U);
    TransformInterval metadata_transform = transform(15U, 25U, 4U);
    metadata_transform.source_update_sequence = 99U;
    p4_nano_overlap::Analysis<1U> metadata_result{};
    p4_nano_overlap::analyze<1U>(
        std::span<const SubmitInterval>(&metadata_submit, 1U),
        std::span<const TransformInterval>(&metadata_transform, 1U),
        metadata_result);
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

void test_pccore_overlap_edges_and_multiple_intervals()
{
    const TransformInterval t = transform(100U, 200U, 1U);
    {
        const PccoreInterval intervals[] = {pccore(0U, 50U, 0U)};
        assert(p4_nano_overlap::calculate_pccore_overlap(t, intervals)
                   .total_us == 0U);
    }
    {
        const PccoreInterval intervals[] = {pccore(200U, 250U, 0U)};
        assert(p4_nano_overlap::calculate_pccore_overlap(t, intervals)
                   .total_us == 0U);
    }
    {
        const PccoreInterval intervals[] = {pccore(120U, 150U, 0U)};
        const auto overlap =
            p4_nano_overlap::calculate_pccore_overlap(t, intervals);
        assert(overlap.total_us == 30U);
        assert(overlap.intersecting_pccore_count == 1U);
    }
    {
        const PccoreInterval intervals[] = {pccore(0U, 300U, 0U)};
        assert(p4_nano_overlap::calculate_pccore_overlap(t, intervals)
                   .total_us == 100U);
    }
    {
        const PccoreInterval intervals[] = {
            pccore(90U, 110U, 0U), pccore(150U, 180U, 1U),
            pccore(220U, 250U, 2U)};
        const auto overlap =
            p4_nano_overlap::calculate_pccore_overlap(t, intervals);
        assert(overlap.total_us == 40U);
        assert(overlap.intersecting_pccore_count == 2U);
    }
}

void test_pccore_analysis_validation_and_conditionals()
{
    const PccoreInterval intervals[] = {
        pccore(0U, 90U, 0U), pccore(110U, 300U, 1U)};
    const TransformInterval transforms[] = {
        transform(80U, 100U, 1U, false),
        transform(120U, 150U, 2U, true),
        transform(350U, 380U, 3U, true)};
    p4_nano_overlap::PccoreAnalysis<2U> result{};
    p4_nano_overlap::analyze_pccore<2U>(intervals, transforms, 2U, false,
                                        result);
    assert(result.pccore_count == 2U);
    assert(result.transform_count == 3U);
    assert(result.measured_transform_count == 2U);
    assert(result.overlapping_transform_count == 1U);
    assert(result.zero_overlap_transform_count == 1U);
    assert(result.overlap_us[0] == 30U);
    assert(result.overlap_fraction_ppm[0] == 1000000U);
    assert(result.intersecting_pccore_count[0] == 1U);
    assert(result.zero_overlap_transform_us[0] == 30U);
    assert(result.overlapping_transform_us[0] == 30U);
    assert(result.trace_completeness);
    assert(result.trace_validation.intervals_valid);
    assert(result.trace_validation.chronological);
    assert(result.trace_validation.call_indices_monotonic);
    assert(result.trace_validation.intervals_non_overlapping);
    assert(result.trace_validation.max_concurrent == 1U);

    const PccoreInterval non_monotonic[] = {
        pccore(100U, 200U, 2U), pccore(90U, 210U, 1U)};
    const auto validation =
        p4_nano_overlap::validate_pccore_intervals(non_monotonic);
    assert(!validation.chronological);
    assert(!validation.call_indices_monotonic);
    assert(!validation.intervals_non_overlapping);
    assert(validation.max_concurrent == 2U);
    assert(!p4_nano_overlap::pccore_trace_complete(1U, 2U, false));
    assert(!p4_nano_overlap::pccore_trace_complete(2U, 2U, true));
}

void test_pccore_trace_capacity_and_all_overlap()
{
    std::array<PccoreInterval, 1U> bounded{};
    std::size_t stored = 0U;
    bool overflow = false;
    assert(p4_nano_overlap::append_bounded(
        bounded, stored, pccore(0U, 100U, 0U), overflow));
    assert(!p4_nano_overlap::append_bounded(
        bounded, stored, pccore(100U, 200U, 1U), overflow));
    assert(stored == 1U);
    assert(overflow);

    const PccoreInterval all_overlap[] = {pccore(0U, 1000U, 0U)};
    const TransformInterval transforms[] = {
        transform(100U, 200U, 1U, true),
        transform(300U, 400U, 2U, true)};
    p4_nano_overlap::PccoreAnalysis<2U> result{};
    p4_nano_overlap::analyze_pccore<2U>(all_overlap, transforms, 1U, false,
                                        result);
    assert(result.overlapping_transform_count == 2U);
    assert(result.zero_overlap_transform_count == 0U);
    assert(result.overlap_us[0] == 100U);
    assert(result.overlap_us[1] == 100U);
    assert(result.overlap_fraction_ppm[0] == 1000000U);
    assert(result.overlap_fraction_ppm[1] == 1000000U);
}

void test_analysis_large_reuse_and_reset()
{
    using LargeAnalysis = p4_nano_overlap::Analysis<128U>;

    const SubmitInterval first_submit[] = {submit(10U, 20U, 1U)};
    const TransformInterval first_transform[] = {
        transform(15U, 25U, 1U)};
    LargeAnalysis result{};
    result.submit_count = 999U;
    result.submit_intervals_non_overlapping = false;
    result.overlap_us.fill(UINT64_MAX);
    result.zero_overlap_transform_us.fill(UINT64_MAX);
    result.overlap_stored = 128U;
    result.zero_overlap_transform_stored = 128U;

    p4_nano_overlap::analyze<128U>(first_submit, first_transform, result);
    assert(result.submit_count == 1U);
    assert(result.transform_count == 1U);
    assert(result.measured_transform_count == 1U);
    assert(result.overlapping_transform_count == 1U);
    assert(result.zero_overlap_transform_count == 0U);
    assert(result.overlap_stored == 1U);
    assert(result.overlap_us[0] == 5U);
    assert(result.overlap_fraction_ppm[0] == 500000U);
    assert(result.overlapping_transform_stored == 1U);
    assert(result.overlapping_transform_us[0] == 10U);

    const SubmitInterval second_submit[] = {submit(0U, 5U, 2U)};
    const TransformInterval second_transform[] = {
        transform(10U, 20U, 2U)};
    p4_nano_overlap::analyze<128U>(second_submit, second_transform, result);
    assert(result.submit_count == 1U);
    assert(result.transform_count == 1U);
    assert(result.measured_transform_count == 1U);
    assert(result.overlapping_transform_count == 0U);
    assert(result.zero_overlap_transform_count == 1U);
    assert(result.matched_transform_count == 1U);
    assert(result.unmatched_acquired_count == 0U);
    assert(result.unmatched_submit_count == 0U);
    assert(result.overlap_stored == 1U);
    assert(result.overlap_us[0] == 0U);
    assert(result.overlap_fraction_ppm[0] == 0U);
    assert(result.zero_overlap_transform_stored == 1U);
    assert(result.zero_overlap_transform_us[0] == 10U);
    assert(result.overlapping_transform_stored == 0U);
    assert(result.intersecting_submit_count_stored == 1U);
    assert(result.intersecting_submit_count[0] == 0U);
    for (std::size_t index = 1U; index < 128U; ++index) {
        assert(result.overlap_us[index] == 0U);
        assert(result.overlap_fraction_ppm[index] == 0U);
        assert(result.transform_us[index] == 0U);
        assert(result.zero_overlap_transform_us[index] == 0U);
        assert(result.overlapping_transform_us[index] == 0U);
        assert(result.intersecting_submit_count[index] == 0U);
    }
}

} // namespace

int main()
{
    test_interval_edges();
    test_analysis_identity_and_warmup();
    test_submit_trace_completeness_and_intersection_count();
    test_different_sequences_and_capacity();
    test_concurrency_assumption();
    test_pccore_overlap_edges_and_multiple_intervals();
    test_pccore_analysis_validation_and_conditionals();
    test_pccore_trace_capacity_and_all_overlap();
    test_analysis_large_reuse_and_reset();
    return 0;
}
