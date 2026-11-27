#pragma once

#include <clab/categorical_heldout.hpp>
#include <clab/evaluator.hpp>

namespace clab {
struct TemporalCategoricalSplit {
    std::size_t train_snapshots{}, guard_snapshots{}, test_snapshots{}, sites{};
};

struct TemporalCategoricalCosts {
    EncodedCategorical model;
    std::uint64_t test_header_bits{}, test_data_bits{}, raw_bits{}, unseen_symbols{};
};

struct TemporalCategoricalResult {
    MetricStatus status{MetricStatus::unsupported};
    std::string_view reason;
    std::size_t train_seen{}, guard_seen{}, test_seen{};
    std::optional<Stamp> train_end, test_start, test_end;
    std::optional<TemporalCategoricalCosts> costs;
};

[[nodiscard]] TemporalCategoricalResult
score_categorical_temporal(IObservationSource& source, TemporalCategoricalSplit split,
                           CodeLimits limits = {});

struct TemporalMotifCosts {
    EncodedCategorical model;
    std::uint64_t test_header_bits{}, placement_bits{}, residual_bits{}, occurrences{};
    [[nodiscard]] std::uint64_t test_bits() const {
        return checked_add(test_header_bits, checked_add(placement_bits, residual_bits));
    }
};

struct TemporalCategoricalMotifCosts {
    EncodedCategorical baseline_model;
    std::uint64_t baseline_test_bits{};
    // An empty motif on a completed split means the training gate did not admit a motif.
    std::optional<TemporalMotifCosts> motif;
    [[nodiscard]] std::optional<std::int64_t> charged_gain_bits() const;
};

struct TemporalCategoricalMotifResult {
    MetricStatus status{MetricStatus::unsupported};
    std::string_view reason;
    std::size_t train_seen{}, guard_seen{}, test_seen{};
    std::optional<Stamp> train_end, test_start, test_end;
    std::optional<TemporalCategoricalMotifCosts> costs;
};

[[nodiscard]] TemporalCategoricalMotifResult
score_categorical_motif_temporal(IObservationSource& source, TemporalCategoricalSplit split,
                                 CodeLimits limits = {});

struct TemporalMotifDictionaryCosts {
    EncodedCategorical model, first_model;
    CodeLength test;
    std::uint64_t placement_bits{}, first_test_bits{};
    std::array<std::uint64_t, 2> occurrences{};
};

struct TemporalCategoricalDictionaryCosts {
    EncodedCategorical baseline_model;
    std::uint64_t baseline_test_bits{};
    // A completed split can retain Huffman costs even when training admits no pair.
    std::optional<TemporalMotifDictionaryCosts> dictionary;
};

struct TemporalCategoricalDictionaryResult {
    MetricStatus status{MetricStatus::unsupported};
    std::string_view reason;
    std::size_t train_seen{}, guard_seen{}, test_seen{};
    std::optional<Stamp> train_end, test_start, test_end;
    std::optional<TemporalCategoricalDictionaryCosts> costs;
};

[[nodiscard]] TemporalCategoricalDictionaryResult
score_categorical_motif_dictionary_temporal(IObservationSource& source, TemporalCategoricalSplit split,
                                            CodeLimits limits = {});

inline constexpr std::size_t temporal_motif_permutation_limit = 199;
inline constexpr std::string_view temporal_motif_permutation_id = "categorical.frame_permutation.v1";
inline constexpr std::string_view temporal_motif_site_permutation_id = "categorical.shared_site_permutation.v1";

struct TemporalMotifPermutationDraw {
    MetricStatus status{MetricStatus::valid};
    std::optional<std::int64_t> charged_gain_bits;
};

struct TemporalMotifPermutationResult {
    TemporalCategoricalMotifResult observed;
    std::vector<TemporalMotifPermutationDraw> draws;
    // Absent unless the observation and every requested draw completed.
    std::optional<std::size_t> at_least_observed;
};

[[nodiscard]] TemporalMotifPermutationResult
score_categorical_motif_permutations(IObservationSource& source, TemporalCategoricalSplit split,
                                     std::size_t draws, std::uint64_t seed, CodeLimits limits = {});

[[nodiscard]] TemporalMotifPermutationResult
score_categorical_motif_site_permutations(IObservationSource& source, TemporalCategoricalSplit split,
                                          std::size_t draws, std::uint64_t seed, CodeLimits limits = {});
}
