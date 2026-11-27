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
}
