#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "np2_pccore_profiler.h"
#include "np2video_runner/pccore_trace.h"

namespace p4_nano_overlap {

/* These records are written by one benchmark task each.  They intentionally
 * contain only timestamps and the existing presentation identities; no
 * production protocol state is represented here. */
struct SubmitInterval {
    std::uint64_t start_us = 0U;
    std::uint64_t end_us = 0U;
    std::uint32_t source_update_sequence = 0U;
    std::uint64_t published_sequence = 0U;
};

struct TransformInterval {
    std::uint64_t acquire_us = 0U;
    std::uint64_t transform_start_us = 0U;
    std::uint64_t transform_end_us = 0U;
    std::uint64_t cache_sync_end_us = 0U;
    std::uint64_t release_us = 0U;
    std::uint32_t source_update_sequence = 0U;
    std::uint64_t published_sequence = 0U;
    std::uint32_t transform_index = 0U;
    bool measured = false;
};

using PccoreInterval = np2video_pccore_interval;
using DrawInterval = np2_pccore_draw_interval;

struct PccoreOverlap {
    std::uint64_t total_us = 0U;
    std::size_t intersecting_pccore_count = 0U;
};

struct PccoreTraceValidation {
    bool intervals_valid = true;
    bool chronological = true;
    bool call_indices_monotonic = true;
    bool intervals_non_overlapping = true;
    std::size_t max_concurrent = 0U;
};

struct DrawTraceValidation {
    bool intervals_valid = true;
    bool chronological = true;
    bool call_indices_monotonic = true;
    bool intervals_non_overlapping = true;
    std::size_t max_concurrent = 0U;
};

struct IntervalOverlap {
    std::uint64_t total_us = 0U;
    std::size_t intersecting_submit_count = 0U;
};

struct DrawOverlap {
    std::uint64_t total_us = 0U;
    std::size_t intersecting_draw_count = 0U;
};

inline IntervalOverlap calculate_overlap(
    const TransformInterval &transform,
    std::span<const SubmitInterval> submits)
{
    IntervalOverlap result{};
    if (transform.transform_end_us <= transform.transform_start_us) {
        return result;
    }
    for (const SubmitInterval &submit : submits) {
        if (submit.end_us <= submit.start_us ||
            submit.end_us <= transform.transform_start_us ||
            transform.transform_end_us <= submit.start_us) {
            continue;
        }
        const std::uint64_t start =
            submit.start_us > transform.transform_start_us
                ? submit.start_us
                : transform.transform_start_us;
        const std::uint64_t end =
            submit.end_us < transform.transform_end_us
                ? submit.end_us
                : transform.transform_end_us;
        if (end > start) {
            result.total_us += end - start;
            ++result.intersecting_submit_count;
        }
    }
    return result;
}

inline PccoreOverlap calculate_pccore_overlap(
    const TransformInterval &transform,
    std::span<const PccoreInterval> pccore_intervals)
{
    PccoreOverlap result{};
    if (transform.transform_end_us <= transform.transform_start_us) {
        return result;
    }
    for (const PccoreInterval &pccore : pccore_intervals) {
        if (pccore.end_us <= pccore.start_us ||
            pccore.end_us <= transform.transform_start_us ||
            transform.transform_end_us <= pccore.start_us) {
            continue;
        }
        const std::uint64_t start =
            pccore.start_us > transform.transform_start_us
                ? pccore.start_us
                : transform.transform_start_us;
        const std::uint64_t end =
            pccore.end_us < transform.transform_end_us
                ? pccore.end_us
                : transform.transform_end_us;
        if (end > start) {
            result.total_us += end - start;
            ++result.intersecting_pccore_count;
        }
    }
    return result;
}

inline DrawOverlap calculate_draw_overlap(
    const TransformInterval &transform,
    std::span<const DrawInterval> draw_intervals)
{
    DrawOverlap result{};
    if (transform.transform_end_us <= transform.transform_start_us) {
        return result;
    }
    for (const DrawInterval &draw : draw_intervals) {
        if (draw.end_us <= draw.start_us ||
            draw.end_us <= transform.transform_start_us ||
            transform.transform_end_us <= draw.start_us) {
            continue;
        }
        const std::uint64_t start = draw.start_us > transform.transform_start_us
                                        ? draw.start_us
                                        : transform.transform_start_us;
        const std::uint64_t end = draw.end_us < transform.transform_end_us
                                      ? draw.end_us
                                      : transform.transform_end_us;
        if (end > start) {
            result.total_us += end - start;
            ++result.intersecting_draw_count;
        }
    }
    return result;
}

inline PccoreTraceValidation validate_pccore_intervals(
    std::span<const PccoreInterval> pccore_intervals)
{
    PccoreTraceValidation result{};
    for (std::size_t index = 0U; index < pccore_intervals.size(); ++index) {
        const PccoreInterval &current = pccore_intervals[index];
        if (current.end_us < current.start_us) {
            result.intervals_valid = false;
        }
        if (index == 0U) {
            continue;
        }
        const PccoreInterval &previous = pccore_intervals[index - 1U];
        if (current.start_us < previous.start_us) {
            result.chronological = false;
        }
        if (current.call_index <= previous.call_index) {
            result.call_indices_monotonic = false;
        }
        if (previous.end_us > current.start_us) {
            result.intervals_non_overlapping = false;
        }
    }
    for (const PccoreInterval &candidate : pccore_intervals) {
        if (candidate.end_us <= candidate.start_us) {
            continue;
        }
        std::size_t active = 0U;
        for (const PccoreInterval &other : pccore_intervals) {
            if (other.end_us > other.start_us &&
                other.start_us <= candidate.start_us &&
                candidate.start_us < other.end_us) {
                ++active;
            }
        }
        if (active > result.max_concurrent) {
            result.max_concurrent = active;
        }
    }
    return result;
}

inline DrawTraceValidation validate_draw_intervals(
    std::span<const DrawInterval> draw_intervals)
{
    DrawTraceValidation result{};
    for (std::size_t index = 0U; index < draw_intervals.size(); ++index) {
        const DrawInterval &current = draw_intervals[index];
        if (current.end_us < current.start_us) {
            result.intervals_valid = false;
        }
        if (index == 0U) {
            continue;
        }
        const DrawInterval &previous = draw_intervals[index - 1U];
        if (current.start_us < previous.start_us) {
            result.chronological = false;
        }
        if (current.call_index <= previous.call_index) {
            result.call_indices_monotonic = false;
        }
        if (previous.end_us > current.start_us) {
            result.intervals_non_overlapping = false;
        }
    }
    for (const DrawInterval &candidate : draw_intervals) {
        if (candidate.end_us <= candidate.start_us) {
            continue;
        }
        std::size_t active = 0U;
        for (const DrawInterval &other : draw_intervals) {
            if (other.end_us > other.start_us &&
                other.start_us <= candidate.start_us &&
                candidate.start_us < other.end_us) {
                ++active;
            }
        }
        if (active > result.max_concurrent) {
            result.max_concurrent = active;
        }
    }
    return result;
}

inline bool pccore_trace_complete(std::size_t recorded_count,
                                   std::uint64_t expected_count,
                                   bool overflow)
{
    return !overflow && recorded_count == expected_count;
}

inline bool draw_trace_complete(std::size_t recorded_count,
                                std::uint64_t expected_count,
                                bool overflow)
{
    return !overflow && recorded_count == expected_count;
}

inline bool intervals_intersect(const SubmitInterval &first,
                                const SubmitInterval &second)
{
    return first.end_us > first.start_us && second.end_us > second.start_us &&
           first.end_us > second.start_us && second.end_us > first.start_us;
}

inline bool submit_trace_complete(std::size_t recorded_count,
                                  std::size_t successful_window_count)
{
    return recorded_count == successful_window_count;
}

inline std::size_t max_concurrent_submit_intervals(
    std::span<const SubmitInterval> submits)
{
    std::size_t maximum = 0U;
    for (const SubmitInterval &candidate : submits) {
        if (candidate.end_us <= candidate.start_us) {
            continue;
        }
        std::size_t active = 0U;
        for (const SubmitInterval &other : submits) {
            if (other.start_us <= candidate.start_us &&
                candidate.start_us < other.end_us &&
                other.end_us > other.start_us) {
                ++active;
            }
        }
        if (active > maximum) {
            maximum = active;
        }
    }
    return maximum;
}

template <typename ChildInterval, typename ParentInterval>
std::size_t count_without_containing_interval(
    std::span<const ChildInterval> children,
    std::span<const ParentInterval> parents)
{
    std::size_t unmatched = 0U;
    for (const ChildInterval &child : children) {
        bool contained = false;
        for (const ParentInterval &parent : parents) {
            if (parent.start_us <= child.start_us &&
                child.end_us <= parent.end_us) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            ++unmatched;
        }
    }
    return unmatched;
}

template <typename T, std::size_t Capacity>
bool append_bounded(std::array<T, Capacity> &storage, std::size_t &stored,
                    const T &value, bool &overflow)
{
    if (stored >= Capacity) {
        overflow = true;
        return false;
    }
    storage[stored++] = value;
    return true;
}

template <std::size_t MaxMeasuredTransforms>
struct Analysis {
    std::size_t submit_count = 0U;
    std::size_t transform_count = 0U;
    std::size_t measured_transform_count = 0U;
    std::size_t overlapping_transform_count = 0U;
    std::size_t zero_overlap_transform_count = 0U;
    std::size_t matched_transform_count = 0U;
    std::size_t unmatched_acquired_count = 0U;
    std::size_t unmatched_submit_count = 0U;
    std::size_t sequence_metadata_mismatch_count = 0U;
    std::size_t max_concurrent_submit_count = 0U;
    std::size_t single_submit_overlap_transform_count = 0U;
    std::size_t multiple_submit_overlap_transform_count = 0U;
    bool submit_intervals_valid = true;
    bool submit_intervals_chronological = true;
    bool submit_intervals_non_overlapping = true;
    bool submit_source_sequences_monotonic = true;
    bool submit_published_sequences_monotonic = true;
    bool transform_source_sequences_monotonic = true;
    bool transform_published_sequences_monotonic = true;
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_fraction_ppm{};
    std::array<std::uint64_t, MaxMeasuredTransforms> transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> zero_overlap_transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> overlapping_transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        intersecting_submit_count{};
    std::size_t overlap_stored = 0U;
    std::size_t overlap_fraction_stored = 0U;
    std::size_t transform_stored = 0U;
    std::size_t zero_overlap_transform_stored = 0U;
    std::size_t overlapping_transform_stored = 0U;
    std::size_t intersecting_submit_count_stored = 0U;

    void reset()
    {
        submit_count = 0U;
        transform_count = 0U;
        measured_transform_count = 0U;
        overlapping_transform_count = 0U;
        zero_overlap_transform_count = 0U;
        matched_transform_count = 0U;
        unmatched_acquired_count = 0U;
        unmatched_submit_count = 0U;
        sequence_metadata_mismatch_count = 0U;
        max_concurrent_submit_count = 0U;
        single_submit_overlap_transform_count = 0U;
        multiple_submit_overlap_transform_count = 0U;
        submit_intervals_valid = true;
        submit_intervals_chronological = true;
        submit_intervals_non_overlapping = true;
        submit_source_sequences_monotonic = true;
        submit_published_sequences_monotonic = true;
        transform_source_sequences_monotonic = true;
        transform_published_sequences_monotonic = true;
        overlap_us.fill(0U);
        overlap_fraction_ppm.fill(0U);
        transform_us.fill(0U);
        zero_overlap_transform_us.fill(0U);
        overlapping_transform_us.fill(0U);
        intersecting_submit_count.fill(0U);
        overlap_stored = 0U;
        overlap_fraction_stored = 0U;
        transform_stored = 0U;
        zero_overlap_transform_stored = 0U;
        overlapping_transform_stored = 0U;
        intersecting_submit_count_stored = 0U;
    }
};

template <std::size_t MaxMeasuredTransforms>
struct PccoreAnalysis {
    std::size_t pccore_count = 0U;
    std::size_t transform_count = 0U;
    std::size_t measured_transform_count = 0U;
    std::size_t overlapping_transform_count = 0U;
    std::size_t zero_overlap_transform_count = 0U;
    PccoreTraceValidation trace_validation{};
    bool trace_overflow = false;
    bool trace_completeness = false;
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_fraction_ppm{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        intersecting_pccore_count{};
    std::array<std::uint64_t, MaxMeasuredTransforms> transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        zero_overlap_transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        overlapping_transform_us{};
    std::size_t overlap_stored = 0U;
    std::size_t overlap_fraction_stored = 0U;
    std::size_t intersecting_pccore_count_stored = 0U;
    std::size_t transform_stored = 0U;
    std::size_t zero_overlap_transform_stored = 0U;
    std::size_t overlapping_transform_stored = 0U;

    void reset()
    {
        pccore_count = 0U;
        transform_count = 0U;
        measured_transform_count = 0U;
        overlapping_transform_count = 0U;
        zero_overlap_transform_count = 0U;
        trace_validation = {};
        trace_overflow = false;
        trace_completeness = false;
        overlap_us.fill(0U);
        overlap_fraction_ppm.fill(0U);
        intersecting_pccore_count.fill(0U);
        transform_us.fill(0U);
        zero_overlap_transform_us.fill(0U);
        overlapping_transform_us.fill(0U);
        overlap_stored = 0U;
        overlap_fraction_stored = 0U;
        intersecting_pccore_count_stored = 0U;
        transform_stored = 0U;
        zero_overlap_transform_stored = 0U;
        overlapping_transform_stored = 0U;
    }
};

template <std::size_t MaxMeasuredTransforms>
struct DrawAnalysis {
    std::size_t draw_count = 0U;
    std::size_t transform_count = 0U;
    std::size_t measured_transform_count = 0U;
    std::size_t overlapping_transform_count = 0U;
    std::size_t zero_overlap_transform_count = 0U;
    DrawTraceValidation trace_validation{};
    bool trace_overflow = false;
    bool trace_completeness = false;
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> overlap_fraction_ppm{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        intersecting_draw_count{};
    std::array<std::uint64_t, MaxMeasuredTransforms> transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        zero_overlap_transform_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        overlapping_transform_us{};
    std::size_t overlap_stored = 0U;
    std::size_t overlap_fraction_stored = 0U;
    std::size_t intersecting_draw_count_stored = 0U;
    std::size_t transform_stored = 0U;
    std::size_t zero_overlap_transform_stored = 0U;
    std::size_t overlapping_transform_stored = 0U;

    void reset()
    {
        draw_count = 0U;
        transform_count = 0U;
        measured_transform_count = 0U;
        overlapping_transform_count = 0U;
        zero_overlap_transform_count = 0U;
        trace_validation = {};
        trace_overflow = false;
        trace_completeness = false;
        overlap_us.fill(0U);
        overlap_fraction_ppm.fill(0U);
        intersecting_draw_count.fill(0U);
        transform_us.fill(0U);
        zero_overlap_transform_us.fill(0U);
        overlapping_transform_us.fill(0U);
        overlap_stored = 0U;
        overlap_fraction_stored = 0U;
        intersecting_draw_count_stored = 0U;
        transform_stored = 0U;
        zero_overlap_transform_stored = 0U;
        overlapping_transform_stored = 0U;
    }
};

template <std::size_t MaxMeasuredTransforms>
struct HierarchyAnalysis {
    std::size_t draw_without_containing_pccore_count = 0U;
    std::size_t submit_without_containing_draw_count = 0U;
    bool submit_subset_draw = false;
    bool draw_subset_pccore = false;
    bool pccore_overlap_within_transform = false;
    bool subtraction_order_valid = false;
    bool validity = false;
    std::array<std::uint64_t, MaxMeasuredTransforms>
        non_submit_draw_overlap_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        non_submit_draw_overlap_fraction_ppm{};
    std::array<std::uint64_t, MaxMeasuredTransforms>
        non_draw_pccore_overlap_us{};
    std::array<std::uint64_t, MaxMeasuredTransforms> outside_pccore_us{};
    std::size_t stored = 0U;

    void reset()
    {
        draw_without_containing_pccore_count = 0U;
        submit_without_containing_draw_count = 0U;
        submit_subset_draw = false;
        draw_subset_pccore = false;
        pccore_overlap_within_transform = false;
        subtraction_order_valid = false;
        validity = false;
        non_submit_draw_overlap_us.fill(0U);
        non_submit_draw_overlap_fraction_ppm.fill(0U);
        non_draw_pccore_overlap_us.fill(0U);
        outside_pccore_us.fill(0U);
        stored = 0U;
    }
};

template <std::size_t MaxMeasuredTransforms>
void analyze_pccore(
    std::span<const PccoreInterval> pccore_intervals,
    std::span<const TransformInterval> transforms,
    std::uint64_t expected_pccore_count, bool trace_overflow,
    PccoreAnalysis<MaxMeasuredTransforms> &result)
{
    result.reset();
    result.pccore_count = pccore_intervals.size();
    result.transform_count = transforms.size();
    result.trace_overflow = trace_overflow;
    result.trace_completeness = pccore_trace_complete(
        pccore_intervals.size(), expected_pccore_count, trace_overflow);
    result.trace_validation = validate_pccore_intervals(pccore_intervals);

    for (const TransformInterval &transform : transforms) {
        if (!transform.measured ||
            result.measured_transform_count >= MaxMeasuredTransforms) {
            continue;
        }
        ++result.measured_transform_count;
        const PccoreOverlap overlap =
            calculate_pccore_overlap(transform, pccore_intervals);
        const std::uint64_t duration_us =
            transform.transform_end_us > transform.transform_start_us
                ? transform.transform_end_us - transform.transform_start_us
                : 0U;
        const std::uint64_t fraction_ppm =
            duration_us == 0U
                ? 0U
                : (overlap.total_us * 1'000'000U) / duration_us;
        result.overlap_us[result.overlap_stored++] = overlap.total_us;
        result.overlap_fraction_ppm[result.overlap_fraction_stored++] =
            fraction_ppm;
        result.intersecting_pccore_count[
            result.intersecting_pccore_count_stored++] =
            static_cast<std::uint64_t>(overlap.intersecting_pccore_count);
        result.transform_us[result.transform_stored++] = duration_us;
        if (overlap.total_us == 0U) {
            ++result.zero_overlap_transform_count;
            result.zero_overlap_transform_us[
                result.zero_overlap_transform_stored++] = duration_us;
        } else {
            ++result.overlapping_transform_count;
            result.overlapping_transform_us[
                result.overlapping_transform_stored++] = duration_us;
        }
    }
}

template <std::size_t MaxMeasuredTransforms>
void analyze_draw(std::span<const DrawInterval> draw_intervals,
                  std::span<const TransformInterval> transforms,
                  std::uint64_t expected_draw_count, bool trace_overflow,
                  DrawAnalysis<MaxMeasuredTransforms> &result)
{
    result.reset();
    result.draw_count = draw_intervals.size();
    result.transform_count = transforms.size();
    result.trace_overflow = trace_overflow;
    result.trace_completeness = draw_trace_complete(
        draw_intervals.size(), expected_draw_count, trace_overflow);
    result.trace_validation = validate_draw_intervals(draw_intervals);

    for (const TransformInterval &transform : transforms) {
        if (!transform.measured ||
            result.measured_transform_count >= MaxMeasuredTransforms) {
            continue;
        }
        ++result.measured_transform_count;
        const DrawOverlap overlap =
            calculate_draw_overlap(transform, draw_intervals);
        const std::uint64_t duration_us =
            transform.transform_end_us > transform.transform_start_us
                ? transform.transform_end_us - transform.transform_start_us
                : 0U;
        const std::uint64_t fraction_ppm = duration_us == 0U
                                               ? 0U
                                               : (overlap.total_us * 1'000'000U) /
                                                     duration_us;
        result.overlap_us[result.overlap_stored++] = overlap.total_us;
        result.overlap_fraction_ppm[result.overlap_fraction_stored++] =
            fraction_ppm;
        result.intersecting_draw_count[
            result.intersecting_draw_count_stored++] =
            static_cast<std::uint64_t>(overlap.intersecting_draw_count);
        result.transform_us[result.transform_stored++] = duration_us;
        if (overlap.total_us == 0U) {
            ++result.zero_overlap_transform_count;
            result.zero_overlap_transform_us[
                result.zero_overlap_transform_stored++] = duration_us;
        } else {
            ++result.overlapping_transform_count;
            result.overlapping_transform_us[
                result.overlapping_transform_stored++] = duration_us;
        }
    }
}

template <std::size_t MaxMeasuredTransforms>
void analyze(
    std::span<const SubmitInterval> submits,
    std::span<const TransformInterval> transforms,
    Analysis<MaxMeasuredTransforms> &result)
{
    result.reset();
    result.submit_count = submits.size();
    result.transform_count = transforms.size();
    result.max_concurrent_submit_count =
        max_concurrent_submit_intervals(submits);
    result.submit_intervals_non_overlapping =
        result.max_concurrent_submit_count <= 1U;

    for (std::size_t index = 0U; index < submits.size(); ++index) {
        if (submits[index].end_us < submits[index].start_us) {
            result.submit_intervals_valid = false;
        }
        if (index > 0U &&
            submits[index].start_us < submits[index - 1U].start_us) {
            result.submit_intervals_chronological = false;
        }
    }

    for (std::size_t index = 1U; index < submits.size(); ++index) {
        result.submit_source_sequences_monotonic =
            result.submit_source_sequences_monotonic &&
            submits[index].source_update_sequence >
                submits[index - 1U].source_update_sequence;
        result.submit_published_sequences_monotonic =
            result.submit_published_sequences_monotonic &&
            submits[index].published_sequence >
                submits[index - 1U].published_sequence;
    }
    for (std::size_t index = 1U; index < transforms.size(); ++index) {
        result.transform_source_sequences_monotonic =
            result.transform_source_sequences_monotonic &&
            transforms[index].source_update_sequence >
                transforms[index - 1U].source_update_sequence;
        result.transform_published_sequences_monotonic =
            result.transform_published_sequences_monotonic &&
            transforms[index].published_sequence >
                transforms[index - 1U].published_sequence;
    }

    for (const SubmitInterval &submit : submits) {
        bool matched = false;
        for (const TransformInterval &transform : transforms) {
            if (submit.published_sequence == transform.published_sequence) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            ++result.unmatched_submit_count;
        }
    }

    for (const TransformInterval &transform : transforms) {
        const IntervalOverlap overlap = calculate_overlap(transform, submits);
        bool matched = false;
        for (const SubmitInterval &submit : submits) {
            if (submit.published_sequence == transform.published_sequence) {
                matched = true;
                if (submit.source_update_sequence !=
                    transform.source_update_sequence) {
                    ++result.sequence_metadata_mismatch_count;
                }
                break;
            }
        }
        if (matched) {
            ++result.matched_transform_count;
        } else {
            ++result.unmatched_acquired_count;
        }
        if (!transform.measured) {
            continue;
        }
        if (result.measured_transform_count >= MaxMeasuredTransforms) {
            continue;
        }
        ++result.measured_transform_count;
        const std::uint64_t duration_us =
            transform.transform_end_us > transform.transform_start_us
                ? transform.transform_end_us - transform.transform_start_us
                : 0U;
        const std::uint64_t fraction_ppm =
            duration_us == 0U ? 0U : (overlap.total_us * 1'000'000U) /
                                      duration_us;
        result.overlap_us[result.overlap_stored++] = overlap.total_us;
        result.overlap_fraction_ppm[result.overlap_fraction_stored++] =
            fraction_ppm;
        result.transform_us[result.transform_stored++] = duration_us;
        result.intersecting_submit_count[
            result.intersecting_submit_count_stored++] =
            static_cast<std::uint64_t>(overlap.intersecting_submit_count);
        if (overlap.total_us == 0U) {
            ++result.zero_overlap_transform_count;
            result.zero_overlap_transform_us[
                result.zero_overlap_transform_stored++] = duration_us;
        } else {
            ++result.overlapping_transform_count;
            if (overlap.intersecting_submit_count == 1U) {
                ++result.single_submit_overlap_transform_count;
            } else {
                ++result.multiple_submit_overlap_transform_count;
            }
            result.overlapping_transform_us[
                result.overlapping_transform_stored++] = duration_us;
        }
    }
}

template <std::size_t MaxMeasuredTransforms>
void analyze_hierarchy(
    std::span<const SubmitInterval> submits,
    std::span<const DrawInterval> draw_intervals,
    std::span<const PccoreInterval> pccore_intervals,
    const Analysis<MaxMeasuredTransforms> &submit_analysis,
    const DrawAnalysis<MaxMeasuredTransforms> &draw_analysis,
    const PccoreAnalysis<MaxMeasuredTransforms> &pccore_analysis,
    HierarchyAnalysis<MaxMeasuredTransforms> &result)
{
    result.reset();
    result.draw_without_containing_pccore_count =
        count_without_containing_interval(draw_intervals, pccore_intervals);
    result.submit_without_containing_draw_count =
        count_without_containing_interval(submits, draw_intervals);
    result.submit_subset_draw =
        result.submit_without_containing_draw_count == 0U;
    result.draw_subset_pccore =
        result.draw_without_containing_pccore_count == 0U;

    const bool interval_families_valid =
        submit_analysis.submit_intervals_valid &&
        submit_analysis.submit_intervals_chronological &&
        submit_analysis.submit_intervals_non_overlapping &&
        draw_analysis.trace_validation.intervals_valid &&
        draw_analysis.trace_validation.chronological &&
        draw_analysis.trace_validation.call_indices_monotonic &&
        draw_analysis.trace_validation.intervals_non_overlapping &&
        draw_analysis.trace_validation.max_concurrent <= 1U &&
        pccore_analysis.trace_validation.intervals_valid &&
        pccore_analysis.trace_validation.chronological &&
        pccore_analysis.trace_validation.call_indices_monotonic &&
        pccore_analysis.trace_validation.intervals_non_overlapping &&
        pccore_analysis.trace_validation.max_concurrent <= 1U &&
        result.submit_subset_draw && result.draw_subset_pccore;
    if (!interval_families_valid ||
        submit_analysis.measured_transform_count !=
            draw_analysis.measured_transform_count ||
        draw_analysis.measured_transform_count !=
            pccore_analysis.measured_transform_count) {
        return;
    }

    result.pccore_overlap_within_transform = true;
    result.subtraction_order_valid = true;
    for (std::size_t index = 0U;
         index < draw_analysis.measured_transform_count; ++index) {
        const std::uint64_t submit_overlap = submit_analysis.overlap_us[index];
        const std::uint64_t draw_overlap = draw_analysis.overlap_us[index];
        const std::uint64_t pccore_overlap = pccore_analysis.overlap_us[index];
        const std::uint64_t transform_us = pccore_analysis.transform_us[index];
        if (submit_overlap > draw_overlap || draw_overlap > pccore_overlap ||
            pccore_overlap > transform_us) {
            result.subtraction_order_valid = false;
            result.pccore_overlap_within_transform = false;
            return;
        }
        result.non_submit_draw_overlap_us[index] =
            draw_overlap - submit_overlap;
        result.non_submit_draw_overlap_fraction_ppm[index] =
            transform_us == 0U
                ? 0U
                : (result.non_submit_draw_overlap_us[index] * 1'000'000U) /
                      transform_us;
        result.non_draw_pccore_overlap_us[index] =
            pccore_overlap - draw_overlap;
        result.outside_pccore_us[index] = transform_us - pccore_overlap;
        ++result.stored;
    }
    result.validity = result.pccore_overlap_within_transform &&
                      result.subtraction_order_valid &&
                      result.stored == draw_analysis.measured_transform_count;
}

} // namespace p4_nano_overlap
