#include <clab/categorical_temporal.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace clab {
TemporalCategoricalResult score_categorical_temporal(IObservationSource& source,
                                                       TemporalCategoricalSplit split,
                                                       CodeLimits limits) {
    if (!split.train_snapshots || !split.guard_snapshots || !split.test_snapshots || !split.sites) {
        throw Error(ErrorCode::invalid, "temporal categorical split requires positive sizes");
    }

    TemporalCategoricalResult result;
    const bool fixed = source.metadata().fixed_sites;
    bool budget = split.train_snapshots > std::min(limits.symbols, categorical_training_limit) / split.sites
                  || split.test_snapshots > limits.symbols / split.sites;
    bool ordered = true;
    std::optional<std::size_t> site_count;
    std::optional<double> previous_time;
    std::vector<TypeId> training;
    std::optional<FrozenCategoricalModel> model;
    TemporalCategoricalCosts costs;

    while (const auto record = source.next()) {
        const auto* snapshot = std::get_if<Snapshot>(&record->payload);
        if (!snapshot) {
            continue;
        }
        const auto symbols = snapshot->symbols();
        if (fixed && site_count && *site_count != symbols.size()) {
            throw Error(ErrorCode::invalid, "fixed-site snapshots differ in size");
        }
        site_count = symbols.size();
        if (result.test_seen == split.test_snapshots) {
            continue; // The rest of the source is still validated.
        }
        const auto time = record->stamp.simulated_time;
        ordered = ordered && std::isfinite(time) && (!previous_time || time > *previous_time);
        previous_time = time;
        const bool usable = fixed && ordered && !budget && symbols.size() >= split.sites;
        try {
            if (result.train_seen < split.train_snapshots) {
                ++result.train_seen;
                result.train_end = record->stamp;
                if (usable) {
                    const auto region = symbols.first(split.sites);
                    training.insert(training.end(), region.begin(), region.end());
                    if (result.train_seen == split.train_snapshots) {
                        model.emplace(FrozenCategoricalModel::fit(training, limits));
                    }
                }
            } else if (result.guard_seen < split.guard_snapshots) {
                ++result.guard_seen;
            } else {
                if (++result.test_seen == 1) {
                    result.test_start = record->stamp;
                }
                result.test_end = record->stamp;
                if (usable && model) {
                    const auto encoded = model->encode(symbols.first(split.sites), limits);
                    costs.test_header_bits = checked_add(costs.test_header_bits,
                                                         encoded.code.length.header_bits);
                    costs.test_data_bits = checked_add(costs.test_data_bits, encoded.code.length.data_bits);
                    const auto raw = checked_add(encoded.code.length.header_bits,
                                                 checked_multiply(split.sites, 32));
                    costs.raw_bits = checked_add(costs.raw_bits, raw);
                    costs.unseen_symbols = checked_add(costs.unseen_symbols, encoded.unseen_symbols);
                }
            }
        } catch (const Error& error) {
            if (error.code() != ErrorCode::limit) {
                throw;
            }
            budget = true;
        }
    }

    if (!site_count) {
        result.reason = "no_categorical_snapshots";
    } else if (!fixed) {
        result.reason = "fixed_sites_required";
    } else if (result.test_seen != split.test_snapshots) {
        result.status = MetricStatus::insufficient_data;
        result.reason = "too_few_snapshots";
    } else if (*site_count < split.sites) {
        result.status = MetricStatus::insufficient_data;
        result.reason = "too_few_sites";
    } else if (!ordered) {
        result.status = MetricStatus::insufficient_data;
        result.reason = "nonincreasing_time";
    } else if (budget) {
        result.status = MetricStatus::budget_exceeded;
        result.reason = "code_limit";
    } else {
        result.status = MetricStatus::valid;
        costs.model = model->model_code();
        result.costs = std::move(costs);
    }
    return result;
}
}
