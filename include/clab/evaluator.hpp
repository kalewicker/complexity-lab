#pragma once

#include <clab/observation.hpp>

#include <functional>

namespace clab {
enum class MetricStatus { valid, unsupported, insufficient_data, budget_exceeded };
using MetricValue = std::variant<std::uint64_t, double>;

struct MetricRow {
    std::optional<Stamp> stamp;
    std::string id;
    std::optional<TypeId> symbol;
    std::optional<MetricValue> value;
    MetricStatus status;
};

struct EvalLimits {
    std::size_t recurrence_snapshots = 16;
    std::size_t recurrence_bytes = 4 * 1024 * 1024;
};

using MetricSink = std::function<void(const MetricRow&)>;
void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits = {});
}
