#pragma once

#include <clab/observation.hpp>

#include <functional>
#include <string_view>

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

struct MetricSpec {
    std::string_view id;
    std::string_view channel;
    std::string_view unit;
};

[[nodiscard]] std::span<const MetricSpec> metric_catalog() noexcept;
[[nodiscard]] const MetricSpec* find_metric(std::string_view id) noexcept;

using MetricSink = std::function<void(const MetricRow&)>;
void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits = {});
// An empty selection means all diagnostics. Listed IDs are validated before reading.
void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits,
              std::span<const std::string_view> selected);
}
