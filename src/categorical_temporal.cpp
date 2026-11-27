#include <clab/categorical_temporal.hpp>
#include <clab/categorical_motif.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace clab {
namespace {
template <typename Result, typename Fit, typename Score>
Result score_temporal(IObservationSource& source, TemporalCategoricalSplit split,
                      CodeLimits limits, std::size_t test_limit, Fit fit, Score score) {
    if (!split.train_snapshots || !split.guard_snapshots || !split.test_snapshots || !split.sites) {
        throw Error(ErrorCode::invalid, "temporal categorical split requires positive sizes");
    }

    Result result;
    const bool fixed = source.metadata().fixed_sites;
    bool budget = split.train_snapshots > std::min(limits.symbols, categorical_training_limit) / split.sites
                  || split.test_snapshots > test_limit / split.sites;
    bool ordered = true;
    std::optional<std::size_t> site_count;
    std::optional<double> previous_time;
    std::vector<TypeId> training;
    typename decltype(result.costs)::value_type costs;

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
                        fit(training, costs);
                    }
                }
            } else if (result.guard_seen < split.guard_snapshots) {
                ++result.guard_seen;
            } else {
                if (++result.test_seen == 1) {
                    result.test_start = record->stamp;
                }
                result.test_end = record->stamp;
                if (usable) {
                    score(symbols.first(split.sites), costs);
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
        result.costs = std::move(costs);
    }
    return result;
}
}

TemporalCategoricalResult score_categorical_temporal(IObservationSource& source,
                                                       TemporalCategoricalSplit split,
                                                       CodeLimits limits) {
    std::optional<FrozenCategoricalModel> model;
    return score_temporal<TemporalCategoricalResult>(
        source, split, limits, limits.symbols,
        [&](std::span<const TypeId> training, TemporalCategoricalCosts& costs) {
            model.emplace(FrozenCategoricalModel::fit(training, limits));
            costs.model = model->model_code();
        },
        [&](std::span<const TypeId> test, TemporalCategoricalCosts& costs) {
            const auto encoded = model->encode(test, limits);
            costs.test_header_bits = checked_add(costs.test_header_bits, encoded.code.length.header_bits);
            costs.test_data_bits = checked_add(costs.test_data_bits, encoded.code.length.data_bits);
            const auto raw = checked_add(encoded.code.length.header_bits, checked_multiply(test.size(), 32));
            costs.raw_bits = checked_add(costs.raw_bits, raw);
            costs.unseen_symbols = checked_add(costs.unseen_symbols, encoded.unseen_symbols);
        });
}

TemporalCategoricalMotifResult score_categorical_motif_temporal(IObservationSource& source,
                                                                 TemporalCategoricalSplit split,
                                                                 CodeLimits limits) {
    std::optional<FrozenCategoricalModel> baseline;
    std::optional<FrozenCategoricalMotif> motif;
    auto result = score_temporal<TemporalCategoricalMotifResult>(
        source, split, limits, std::min(limits.symbols, categorical_iid_symbol_limit),
        [&](std::span<const TypeId> training, TemporalCategoricalMotifCosts& costs) {
            baseline.emplace(FrozenCategoricalModel::fit(training, limits));
            costs.baseline_model = baseline->model_code();
            motif = FrozenCategoricalMotif::fit_frames(training, split.sites, limits);
            if (motif) {
                costs.motif.emplace(TemporalMotifCosts{motif->model_code()});
            }
        },
        [&](std::span<const TypeId> test, TemporalCategoricalMotifCosts& costs) {
            const auto encoded = baseline->encode(test, limits);
            costs.baseline_test_bits = checked_add(costs.baseline_test_bits, encoded.code.length.total_bits());
            if (motif) {
                const auto frame = motif->encode(test, limits);
                auto& sums = *costs.motif;
                sums.test_header_bits = checked_add(sums.test_header_bits, frame.code.length.header_bits);
                sums.placement_bits = checked_add(sums.placement_bits, frame.placement_bits);
                sums.residual_bits = checked_add(sums.residual_bits, frame.code.length.data_bits - frame.placement_bits);
                sums.occurrences = checked_add(sums.occurrences, frame.placements.size());
            }
        });
    if (result.costs && !result.costs->motif) {
        result.reason = "no_training_motif";
    }
    return result;
}
}
