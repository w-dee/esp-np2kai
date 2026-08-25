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
using p4_nano_overlap::DrawInterval;
using p4_nano_overlap::CpuNeventInterval;

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

DrawInterval draw(std::uint64_t start, std::uint64_t end,
                  std::uint32_t call_index)
{
    DrawInterval value{};
    value.start_us = start;
    value.end_us = end;
    value.call_index = call_index;
    return value;
}

CpuNeventInterval cpu_nevent(std::uint64_t cpu_start,
                              std::uint64_t nevent_start,
                              std::uint64_t nevent_end,
                              std::uint32_t call_index, bool has_cpu)
{
    CpuNeventInterval value{};
    value.cpu_start_us = cpu_start;
    value.nevent_start_us = nevent_start;
    value.nevent_end_us = nevent_end;
    value.call_index = call_index;
    value.has_cpu = has_cpu;
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

void test_draw_intersection_edges_and_conditionals()
{
    const TransformInterval t = transform(100U, 200U, 1U);
    {
        const DrawInterval draws[] = {draw(0U, 100U, 1U)};
        assert(p4_nano_overlap::calculate_draw_overlap(t, draws).total_us ==
               0U);
    }
    {
        const DrawInterval draws[] = {draw(200U, 300U, 1U)};
        assert(p4_nano_overlap::calculate_draw_overlap(t, draws).total_us ==
               0U);
    }
    {
        const DrawInterval draws[] = {draw(50U, 125U, 1U)};
        assert(p4_nano_overlap::calculate_draw_overlap(t, draws).total_us ==
               25U);
    }
    {
        const DrawInterval draws[] = {draw(175U, 250U, 1U)};
        assert(p4_nano_overlap::calculate_draw_overlap(t, draws).total_us ==
               25U);
    }
    {
        const DrawInterval draws[] = {draw(125U, 175U, 1U)};
        assert(p4_nano_overlap::calculate_draw_overlap(t, draws).total_us ==
               50U);
    }
    {
        const DrawInterval draws[] = {
            draw(110U, 130U, 1U), draw(150U, 180U, 2U)};
        const auto overlap = p4_nano_overlap::calculate_draw_overlap(t, draws);
        assert(overlap.total_us == 50U);
        assert(overlap.intersecting_draw_count == 2U);
    }

    const DrawInterval all_draw[] = {draw(0U, 1000U, 1U)};
    const TransformInterval transforms[] = {
        transform(0U, 10U, 1U, false), transform(100U, 200U, 2U, true),
        transform(300U, 400U, 3U, true), transform(500U, 510U, 4U, false)};
    p4_nano_overlap::DrawAnalysis<2U> all_result{};
    p4_nano_overlap::analyze_draw<2U>(all_draw, transforms, 1U, false,
                                       all_result);
    assert(all_result.measured_transform_count == 2U);
    assert(all_result.overlapping_transform_count == 2U);
    assert(all_result.zero_overlap_transform_count == 0U);

    const DrawInterval zero_draw[] = {draw(0U, 10U, 1U)};
    p4_nano_overlap::DrawAnalysis<2U> zero_result{};
    p4_nano_overlap::analyze_draw<2U>(zero_draw, transforms, 1U, false,
                                       zero_result);
    assert(zero_result.overlapping_transform_count == 0U);
    assert(zero_result.zero_overlap_transform_count == 2U);
}

void test_draw_trace_validity_completeness_and_capacity()
{
    const DrawInterval valid[] = {
        draw(10U, 20U, 1U), draw(20U, 30U, 2U)};
    const auto validation = p4_nano_overlap::validate_draw_intervals(valid);
    assert(validation.intervals_valid);
    assert(validation.chronological);
    assert(validation.call_indices_monotonic);
    assert(validation.intervals_non_overlapping);
    assert(validation.max_concurrent == 1U);
    assert(p4_nano_overlap::draw_trace_complete(2U, 2U, false));
    assert(!p4_nano_overlap::draw_trace_complete(1U, 2U, false));
    assert(!p4_nano_overlap::draw_trace_complete(3U, 2U, false));
    assert(!p4_nano_overlap::draw_trace_complete(2U, 2U, true));

    const DrawInterval invalid_end[] = {draw(20U, 10U, 1U)};
    assert(!p4_nano_overlap::validate_draw_intervals(invalid_end)
                .intervals_valid);
    const DrawInterval invalid_index[] = {
        draw(10U, 20U, 2U), draw(20U, 30U, 1U)};
    assert(!p4_nano_overlap::validate_draw_intervals(invalid_index)
                .call_indices_monotonic);
    const DrawInterval overlapping[] = {
        draw(10U, 30U, 1U), draw(20U, 40U, 2U)};
    const auto overlapping_validation =
        p4_nano_overlap::validate_draw_intervals(overlapping);
    assert(!overlapping_validation.intervals_non_overlapping);
    assert(overlapping_validation.max_concurrent == 2U);

    std::array<DrawInterval, 1U> bounded{};
    std::size_t stored = 0U;
    bool overflow = false;
    const DrawInterval first = draw(10U, 20U, 1U);
    assert(p4_nano_overlap::append_bounded(bounded, stored, first, overflow));
    assert(!p4_nano_overlap::append_bounded(
        bounded, stored, draw(20U, 30U, 2U), overflow));
    assert(stored == 1U);
    assert(overflow);
    assert(bounded[0].start_us == first.start_us);
    assert(bounded[0].end_us == first.end_us);
    assert(bounded[0].call_index == first.call_index);

    np2_pccore_draw_trace profiler_trace{};
    np2_pccore_draw_trace_reset(&profiler_trace);
    for (std::uint32_t index = 0U;
         index < NP2_PCCORE_DRAW_TRACE_CAPACITY; ++index) {
        assert(np2_pccore_draw_trace_append(&profiler_trace,
                                            100U + index,
                                            101U + index,
                                            static_cast<std::uint64_t>(index) +
                                                1U));
    }
    assert(profiler_trace.stored == NP2_PCCORE_DRAW_TRACE_CAPACITY);
    assert(!profiler_trace.overflow);
    const auto last = profiler_trace.intervals[
        NP2_PCCORE_DRAW_TRACE_CAPACITY - 1U];
    assert(!np2_pccore_draw_trace_append(
        &profiler_trace, 1000U, 1001U,
        static_cast<std::uint64_t>(NP2_PCCORE_DRAW_TRACE_CAPACITY) + 1U));
    assert(profiler_trace.overflow);
    assert(profiler_trace.stored == NP2_PCCORE_DRAW_TRACE_CAPACITY);
    assert(profiler_trace.intervals[NP2_PCCORE_DRAW_TRACE_CAPACITY - 1U]
               .start_us == last.start_us);
    assert(profiler_trace.intervals[NP2_PCCORE_DRAW_TRACE_CAPACITY - 1U]
               .end_us == last.end_us);
    assert(profiler_trace.intervals[NP2_PCCORE_DRAW_TRACE_CAPACITY - 1U]
               .call_index == last.call_index);
}

void test_hierarchy_containment_and_subtraction()
{
    const SubmitInterval submits[] = {submit(150U, 160U, 1U)};
    const DrawInterval draws[] = {draw(120U, 180U, 1U)};
    const PccoreInterval pccores[] = {pccore(100U, 200U, 1U)};
    const TransformInterval transforms[] = {transform(100U, 200U, 1U, true)};
    p4_nano_overlap::Analysis<1U> submit_result{};
    p4_nano_overlap::DrawAnalysis<1U> draw_result{};
    p4_nano_overlap::PccoreAnalysis<1U> pccore_result{};
    p4_nano_overlap::HierarchyAnalysis<1U> hierarchy{};
    p4_nano_overlap::analyze<1U>(submits, transforms, submit_result);
    p4_nano_overlap::analyze_draw<1U>(draws, transforms, 1U, false,
                                       draw_result);
    p4_nano_overlap::analyze_pccore<1U>(pccores, transforms, 1U, false,
                                         pccore_result);
    p4_nano_overlap::analyze_hierarchy<1U>(
        submits, draws, pccores, submit_result, draw_result, pccore_result,
        hierarchy);
    assert(hierarchy.submit_subset_draw);
    assert(hierarchy.draw_subset_pccore);
    assert(hierarchy.validity);
    assert(hierarchy.submit_without_containing_draw_count == 0U);
    assert(hierarchy.draw_without_containing_pccore_count == 0U);
    assert(hierarchy.non_submit_draw_overlap_us[0] == 50U);
    assert(hierarchy.non_draw_pccore_overlap_us[0] == 40U);
    assert(hierarchy.outside_pccore_us[0] == 0U);

    const SubmitInterval submit_before_draw[] = {submit(110U, 160U, 1U)};
    p4_nano_overlap::analyze<1U>(submit_before_draw, transforms,
                                 submit_result);
    p4_nano_overlap::analyze_hierarchy<1U>(
        submit_before_draw, draws, pccores, submit_result, draw_result,
        pccore_result, hierarchy);
    assert(!hierarchy.submit_subset_draw);
    assert(!hierarchy.validity);

    const DrawInterval draw_outside_pccore[] = {draw(90U, 180U, 1U)};
    p4_nano_overlap::analyze_draw<1U>(draw_outside_pccore, transforms, 1U,
                                       false, draw_result);
    p4_nano_overlap::analyze<1U>(submits, transforms, submit_result);
    p4_nano_overlap::analyze_hierarchy<1U>(
        submits, draw_outside_pccore, pccores, submit_result, draw_result,
        pccore_result, hierarchy);
    assert(!hierarchy.draw_subset_pccore);
    assert(!hierarchy.validity);

    p4_nano_overlap::analyze_draw<1U>(draws, transforms, 1U, false,
                                       draw_result);
    p4_nano_overlap::analyze_hierarchy<1U>(
        submits, draws, pccores, submit_result, draw_result, pccore_result,
        hierarchy);
    submit_result.overlap_us[0] = 61U;
    p4_nano_overlap::analyze_hierarchy<1U>(
        submits, draws, pccores, submit_result, draw_result, pccore_result,
        hierarchy);
    assert(!hierarchy.subtraction_order_valid);
    assert(!hierarchy.validity);
    submit_result.overlap_us[0] = 10U;
    draw_result.overlap_us[0] = 101U;
    p4_nano_overlap::analyze_hierarchy<1U>(
        submits, draws, pccores, submit_result, draw_result, pccore_result,
        hierarchy);
    assert(!hierarchy.subtraction_order_valid);
    assert(!hierarchy.validity);
}

void test_cpu_nevent_trace_and_overlap_analysis()
{
    np2_pccore_cpu_nevent_trace trace{};
    np2_pccore_cpu_nevent_trace_reset(&trace);
    assert(np2_pccore_cpu_nevent_trace_append(&trace, 0U, 20U, 40U, 1U,
                                               true));
    assert(np2_pccore_cpu_nevent_trace_append(&trace, 40U, 40U, 60U, 2U,
                                               false));
    assert(np2_pccore_cpu_nevent_trace_append(&trace, 60U, 80U, 100U, 3U,
                                               true));
    assert(trace.stored == 3U);
    assert(trace.has_cpu_stored == 2U);
    assert(!trace.overflow);

    const auto validation = p4_nano_overlap::validate_cpu_nevent_intervals(
        std::span<const CpuNeventInterval>(trace.intervals, trace.stored));
    assert(validation.intervals_valid);
    assert(validation.chronological);
    assert(validation.call_indices_monotonic);
    assert(validation.intervals_non_overlapping);
    assert(validation.max_concurrent == 1U);
    const TransformInterval cpu_only = transform(0U, 20U, 1U);
    const TransformInterval nevent_only = transform(20U, 40U, 2U);
    const TransformInterval boundary = transform(20U, 60U, 3U);
    assert(p4_nano_overlap::calculate_cpu_exec_overlap(
               cpu_only, std::span<const CpuNeventInterval>(trace.intervals,
                                                              trace.stored))
               .total_us == 20U);
    assert(p4_nano_overlap::calculate_nevent_overlap(
               cpu_only, std::span<const CpuNeventInterval>(trace.intervals,
                                                              trace.stored))
               .total_us == 0U);
    assert(p4_nano_overlap::calculate_cpu_exec_overlap(
               nevent_only,
               std::span<const CpuNeventInterval>(trace.intervals,
                                                  trace.stored))
               .total_us == 0U);
    assert(p4_nano_overlap::calculate_nevent_overlap(
               nevent_only,
               std::span<const CpuNeventInterval>(trace.intervals,
                                                  trace.stored))
               .total_us == 20U);
    assert(p4_nano_overlap::calculate_cpu_exec_overlap(
               boundary, std::span<const CpuNeventInterval>(trace.intervals,
                                                              trace.stored))
               .total_us == 0U);
    assert(p4_nano_overlap::calculate_nevent_overlap(
               boundary, std::span<const CpuNeventInterval>(trace.intervals,
                                                              trace.stored))
               .total_us == 40U);
    assert(p4_nano_overlap::cpu_nevent_trace_complete(3U, 3U, 2U, 2U,
                                                       false));
    assert(!p4_nano_overlap::cpu_nevent_trace_complete(2U, 3U, 2U, 2U,
                                                        false));
    assert(!p4_nano_overlap::cpu_nevent_trace_complete(3U, 3U, 1U, 2U,
                                                        false));
    assert(!p4_nano_overlap::cpu_nevent_trace_complete(3U, 3U, 2U, 2U,
                                                        true));

    const TransformInterval measured[] = {
        transform(0U, 100U, 1U, true),
        transform(25U, 125U, 2U, true),
        transform(150U, 160U, 3U, false),
    };
    const DrawInterval draws[] = {draw(50U, 55U, 1U)};
    const SubmitInterval submits[] = {submit(51U, 53U, 1U)};
    const PccoreInterval pccores[] = {pccore(0U, 100U, 1U)};
    p4_nano_overlap::Analysis<2U> submit_analysis{};
    p4_nano_overlap::DrawAnalysis<2U> draw_analysis{};
    p4_nano_overlap::PccoreAnalysis<2U> pccore_analysis{};
    p4_nano_overlap::HierarchyAnalysis<2U> hierarchy{};
    p4_nano_overlap::CpuNeventAnalysis<2U> result{};
    p4_nano_overlap::analyze<2U>(submits, measured, submit_analysis);
    p4_nano_overlap::analyze_draw<2U>(draws, measured, 1U, false,
                                       draw_analysis);
    p4_nano_overlap::analyze_pccore<2U>(pccores, measured, 1U, false,
                                         pccore_analysis);
    p4_nano_overlap::analyze_hierarchy<2U>(
        submits, draws, pccores, submit_analysis, draw_analysis,
        pccore_analysis, hierarchy);
    p4_nano_overlap::analyze_cpu_nevent<2U>(
        std::span<const CpuNeventInterval>(trace.intervals, trace.stored),
        draws, pccores, measured, 2U, 3U, trace.has_cpu_stored, trace.overflow,
        submit_analysis, draw_analysis, pccore_analysis, hierarchy, result);
    assert(result.pair_count == 3U);
    assert(result.cpu_count == 2U);
    assert(result.nevent_count == 3U);
    assert(result.measured_transform_count == 2U);
    assert(result.pair_trace_completeness);
    assert(result.pair_cpu_completeness);
    assert(result.cpu_phase_count_order_valid);
    assert(result.pair_subset_pccore);
    assert(result.draw_subset_nevent);
    assert(result.structural_validity);
    assert(result.arithmetic_validity);
    assert(result.full_hierarchy_validity);
    assert(result.cpu_overlap_us[0] == 40U);
    assert(result.nevent_overlap_us[0] == 60U);
    assert(result.non_draw_nevent_overlap_us[0] == 55U);
    assert(result.other_pccore_overlap_us[0] == 0U);
    assert(result.cpu_overlap_us[1] == 20U);
    assert(result.nevent_overlap_us[1] == 55U);
    assert(result.non_draw_nevent_overlap_us[1] == 50U);
    assert(result.other_pccore_overlap_us[1] == 0U);

    const CpuNeventInterval invalid_end[] = {cpu_nevent(10U, 20U, 19U, 1U,
                                                         true)};
    assert(!p4_nano_overlap::validate_cpu_nevent_intervals(invalid_end)
                .intervals_valid);
    const CpuNeventInterval invalid_skip[] = {cpu_nevent(10U, 20U, 30U, 1U,
                                                          false)};
    assert(!p4_nano_overlap::validate_cpu_nevent_intervals(invalid_skip)
                .intervals_valid);
    const CpuNeventInterval invalid_cpu_start[] = {
        cpu_nevent(30U, 20U, 40U, 1U, true)};
    assert(!p4_nano_overlap::validate_cpu_nevent_intervals(invalid_cpu_start)
                .intervals_valid);
    const CpuNeventInterval invalid_order[] = {
        cpu_nevent(20U, 30U, 40U, 2U, true),
        cpu_nevent(10U, 10U, 20U, 1U, false),
    };
    const auto invalid_order_validation =
        p4_nano_overlap::validate_cpu_nevent_intervals(invalid_order);
    assert(!invalid_order_validation.chronological);
    assert(!invalid_order_validation.call_indices_monotonic);
    assert(!invalid_order_validation.intervals_non_overlapping);

    const DrawInterval draw_outside[] = {draw(10U, 15U, 1U)};
    p4_nano_overlap::DrawAnalysis<2U> outside_draw_analysis{};
    p4_nano_overlap::analyze_draw<2U>(draw_outside, measured, 1U, false,
                                       outside_draw_analysis);
    p4_nano_overlap::analyze_cpu_nevent<2U>(
        std::span<const CpuNeventInterval>(trace.intervals, trace.stored),
        draw_outside, pccores, measured, 2U, 3U, trace.has_cpu_stored,
        trace.overflow, submit_analysis, outside_draw_analysis,
        pccore_analysis, hierarchy, result);
    assert(!result.draw_subset_nevent);
    assert(!result.structural_validity);

    const CpuNeventInterval pair_outside[] = {
        cpu_nevent(100U, 100U, 110U, 1U, false)};
    assert(p4_nano_overlap::count_cpu_nevent_without_containing_interval(
               std::span<const CpuNeventInterval>(pair_outside),
               std::span<const PccoreInterval>(pccores)) == 1U);
    const DrawInterval draw_after[] = {draw(95U, 105U, 1U)};
    assert(p4_nano_overlap::count_draw_without_containing_cpu_nevent(
               std::span<const DrawInterval>(draw_after),
               std::span<const CpuNeventInterval>(trace.intervals,
                                                  trace.stored)) == 1U);

    p4_nano_overlap::analyze_cpu_nevent<2U>(
        std::span<const CpuNeventInterval>(trace.intervals, trace.stored),
        draws, pccores, measured, 4U, 3U, trace.has_cpu_stored,
        trace.overflow, submit_analysis, draw_analysis, pccore_analysis,
        hierarchy, result);
    assert(!result.cpu_phase_count_order_valid);
    assert(!result.structural_validity);

    np2_pccore_cpu_nevent_trace bounded{};
    np2_pccore_cpu_nevent_trace_reset(&bounded);
    for (std::uint32_t index = 0U;
         index < NP2_PCCORE_CPU_NEVENT_TRACE_CAPACITY; ++index) {
        assert(np2_pccore_cpu_nevent_trace_append(
            &bounded, index, index, index + 1U, index + 1U, false));
    }
    assert(bounded.stored == NP2_PCCORE_CPU_NEVENT_TRACE_CAPACITY);
    assert(bounded.has_cpu_stored == 0U);
    assert(!np2_pccore_cpu_nevent_trace_append(
        &bounded, 0U, 0U, 1U, NP2_PCCORE_CPU_NEVENT_TRACE_CAPACITY + 1U,
        false));
    assert(bounded.overflow);
    assert(bounded.stored == NP2_PCCORE_CPU_NEVENT_TRACE_CAPACITY);
    np2_pccore_cpu_nevent_trace_reset(&bounded);
    assert(bounded.stored == 0U);
    assert(bounded.has_cpu_stored == 0U);
    assert(!bounded.overflow);
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
    test_draw_intersection_edges_and_conditionals();
    test_draw_trace_validity_completeness_and_capacity();
    test_hierarchy_containment_and_subtraction();
    test_cpu_nevent_trace_and_overlap_analysis();
    return 0;
}
