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
}
