#include <clab/categorical_temporal.hpp>
#include <clab/categorical_motif.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
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

std::optional<std::int64_t> TemporalCategoricalMotifCosts::charged_gain_bits() const {
    if (!motif) {
        return std::nullopt;
    }
    const auto baseline = checked_add(baseline_test_bits, baseline_model.length.total_bits());
    const auto model = checked_add(motif->test_bits(), motif->model.length.total_bits());
    return static_cast<std::int64_t>(baseline) - static_cast<std::int64_t>(model);
}

std::optional<std::int64_t> TemporalCategoricalDictionaryCosts::charged_gain_vs_first_bits() const {
    if (!dictionary) {
        return std::nullopt;
    }
    const auto first = checked_add(dictionary->first_test_bits, dictionary->first_model.length.total_bits());
    const auto pair = checked_add(dictionary->test.total_bits(), dictionary->model.length.total_bits());
    return static_cast<std::int64_t>(first) - static_cast<std::int64_t>(pair);
}

namespace {
class TemporalMotifScorer {
public:
    void fit(std::span<const TypeId> training, std::size_t sites,
             CodeLimits limits, TemporalCategoricalMotifCosts& costs) {
        baseline_.emplace(FrozenCategoricalModel::fit(training, limits));
        costs.baseline_model = baseline_->model_code();
        motif_ = FrozenCategoricalMotif::fit_frames(training, sites, limits);
        if (motif_) {
            costs.motif.emplace(TemporalMotifCosts{motif_->model_code()});
        }
    }

    void score(std::span<const TypeId> test, CodeLimits limits, TemporalCategoricalMotifCosts& costs) const {
        const auto encoded = baseline_->encode(test, limits);
        costs.baseline_test_bits = checked_add(costs.baseline_test_bits, encoded.code.length.total_bits());
        if (motif_) {
            const auto frame = motif_->encode(test, limits);
            auto& sums = *costs.motif;
            sums.test_header_bits = checked_add(sums.test_header_bits, frame.code.length.header_bits);
            sums.placement_bits = checked_add(sums.placement_bits, frame.placement_bits);
            sums.residual_bits = checked_add(sums.residual_bits, frame.code.length.data_bits - frame.placement_bits);
            sums.occurrences = checked_add(sums.occurrences, frame.placements.size());
        }
    }

private:
    std::optional<FrozenCategoricalModel> baseline_;
    std::optional<FrozenCategoricalMotif> motif_;
};

class TemporalDictionaryScorer {
public:
    explicit TemporalDictionaryScorer(std::optional<CategoricalMotifAnchor> anchor = std::nullopt) : anchor_(anchor) {}

    void fit(std::span<const TypeId> training, std::size_t sites,
             CodeLimits limits, TemporalCategoricalDictionaryCosts& costs) {
        baseline_.emplace(FrozenCategoricalModel::fit(training, limits));
        costs.baseline_model = baseline_->model_code();
        model_ = anchor_ ? FrozenCategoricalMotifDictionary::fit_anchored_frames(training, sites, *anchor_, limits)
                         : FrozenCategoricalMotifDictionary::fit_frames(training, sites, limits);
        if (model_) {
            auto& dictionary = costs.dictionary.emplace();
            dictionary.model = model_->model_code();
            dictionary.first_model = model_->first_entry().model_code();
        }
    }

    void score(std::span<const TypeId> test, CodeLimits limits, TemporalCategoricalDictionaryCosts& costs) const {
        const auto encoded = baseline_->encode(test, limits);
        costs.baseline_test_bits = checked_add(costs.baseline_test_bits, encoded.code.length.total_bits());
        if (!model_) {
            return;
        }
        auto& sums = *costs.dictionary;
        const auto pair = model_->encode(test, limits);
        const auto first = model_->first_entry().encode(test, limits);
        sums.test.header_bits = checked_add(sums.test.header_bits, pair.encoded.length.header_bits);
        sums.test.data_bits = checked_add(sums.test.data_bits, pair.encoded.length.data_bits);
        sums.placement_bits = checked_add(sums.placement_bits, pair.placement_bits);
        sums.first_test_bits = checked_add(sums.first_test_bits, first.code.length.total_bits());
        for (std::size_t i = 0; i < pair.entries.size(); ++i) {
            sums.occurrences[i] = checked_add(sums.occurrences[i], pair.entries[i].placements.size());
        }
    }

private:
    std::optional<CategoricalMotifAnchor> anchor_;
    std::optional<FrozenCategoricalModel> baseline_;
    std::optional<FrozenCategoricalMotifDictionary> model_;
};

struct MotifWindows {
    std::vector<TypeId> training, test;
};

template <typename Result, typename Scorer>
Result score_motif_window(IObservationSource& source, TemporalCategoricalSplit split,
                          CodeLimits limits, MotifWindows* windows, Scorer scorer = Scorer{}) {
    return score_temporal<Result>(
        source, split, limits, std::min(limits.symbols, categorical_iid_symbol_limit),
        [&](std::span<const TypeId> training, auto& costs) {
            scorer.fit(training, split.sites, limits, costs);
            if (windows) {
                windows->training.assign(training.begin(), training.end());
            }
        },
        [&](std::span<const TypeId> test, auto& costs) {
            scorer.score(test, limits, costs);
            if (windows) {
                windows->test.insert(windows->test.end(), test.begin(), test.end());
            }
        });
}

TemporalCategoricalMotifResult score_single_window(IObservationSource& source, TemporalCategoricalSplit split,
                                                    CodeLimits limits, MotifWindows* windows = nullptr) {
    auto result = score_motif_window<TemporalCategoricalMotifResult, TemporalMotifScorer>(source, split, limits, windows);
    if (result.costs && !result.costs->motif) {
        result.reason = "no_training_motif";
    }
    return result;
}

TemporalCategoricalDictionaryResult score_dictionary_window(
    IObservationSource& source, TemporalCategoricalSplit split, CodeLimits limits, MotifWindows* windows = nullptr,
    std::optional<CategoricalMotifAnchor> anchor = std::nullopt) {
    auto result = score_motif_window<TemporalCategoricalDictionaryResult, TemporalDictionaryScorer>(
        source, split, limits, windows, TemporalDictionaryScorer{anchor});
    if (result.costs && !result.costs->dictionary) {
        result.reason = "no_training_dictionary";
    }
    return result;
}

template <typename Item>
void shuffle_sites(std::span<Item> items, std::uint64_t& seed) {
    for (std::uint64_t bound = items.size(); bound > 1; --bound) {
        // Reject the partial interval before reducing SplitMix64 output modulo the bound.
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        std::uint64_t value;
        do {
            seed += UINT64_C(0x9e3779b97f4a7c15);
            value = seed;
            value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
            value ^= value >> 31;
        } while (value < threshold);
        std::swap(items[bound - 1], items[value % bound]);
    }
}

MotifWindows permute_independent_frames(const MotifWindows& original, std::size_t sites, std::uint64_t& seed) {
    auto permuted = original;
    for (auto frames : {std::span(permuted.training), std::span(permuted.test)}) {
        for (std::size_t offset = 0; offset < frames.size(); offset += sites) {
            shuffle_sites(frames.subspan(offset, sites), seed);
        }
    }
    return permuted;
}

MotifWindows permute_shared_sites(const MotifWindows& original, std::size_t sites, std::uint64_t& seed,
                                  std::optional<CategoricalMotifAnchor> anchor = std::nullopt) {
    std::vector<std::size_t> order(sites);
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (anchor) {
        std::vector<std::size_t> free;
        for (std::size_t site = 0; site < sites; ++site) {
            if (site < anchor->start || site >= anchor->start + anchor->width) {
                free.push_back(site);
            }
        }
        auto shuffled = free;
        shuffle_sites(std::span(shuffled), seed);
        for (std::size_t i = 0; i < free.size(); ++i) {
            order[free[i]] = shuffled[i];
        }
    } else {
        shuffle_sites(std::span(order), seed);
    }
    const auto apply = [&](const std::vector<TypeId>& frames) {
        std::vector<TypeId> permuted;
        permuted.reserve(frames.size());
        for (std::size_t offset = 0; offset < frames.size(); offset += sites) {
            for (auto source : order) {
                permuted.push_back(frames[offset + source]);
            }
        }
        return permuted;
    };
    return {apply(original.training), apply(original.test)};
}

MotifWindows permute_all_shared_sites(const MotifWindows& window, std::size_t sites, std::uint64_t& seed) {
    return permute_shared_sites(window, sites, seed);
}

std::int64_t positive_gain(std::optional<std::int64_t> gain) {
    return std::max<std::int64_t>(0, gain.value_or(0));
}

template <typename Result, typename Scorer>
Result score_motif_permutations(IObservationSource& source, TemporalCategoricalSplit split,
                                std::size_t count, std::uint64_t seed, CodeLimits limits,
                                auto permute, auto score_window, auto charged_gain,
                                Scorer fresh = Scorer{}) {
    if (!count || count > temporal_motif_permutation_limit) {
        throw Error(ErrorCode::invalid, "temporal motif permutation draw count is outside its bounds");
    }
    MotifWindows windows;
    Result result;
    result.observed = score_window(source, split, limits, &windows);
    if (!result.observed.costs) {
        return result;
    }
    const auto observed = positive_gain(((*result.observed.costs).*charged_gain)());
    result.draws.reserve(count);
    std::size_t at_least = 0;
    bool complete = true;
    for (std::size_t draw = 0; draw < count; ++draw) {
        const auto permuted = permute(windows, split.sites, seed);
        auto scorer = fresh;
        typename decltype(result.observed.costs)::value_type costs;
        try {
            scorer.fit(permuted.training, split.sites, limits, costs);
            const auto test = std::span(permuted.test);
            for (std::size_t offset = 0; offset < test.size(); offset += split.sites) {
                scorer.score(test.subspan(offset, split.sites), limits, costs);
            }
            const auto gain = (costs.*charged_gain)();
            at_least += positive_gain(gain) >= observed;
            result.draws.push_back({MetricStatus::valid, gain});
        } catch (const Error& error) {
            if (error.code() != ErrorCode::limit) {
                throw;
            }
            result.draws.push_back({MetricStatus::budget_exceeded, std::nullopt});
            complete = false;
        }
    }
    if (complete) {
        result.at_least_observed = at_least;
    }
    return result;
}
}

TemporalCategoricalMotifResult score_categorical_motif_temporal(IObservationSource& source,
                                                                 TemporalCategoricalSplit split,
                                                                 CodeLimits limits) {
    return score_single_window(source, split, limits);
}

TemporalCategoricalDictionaryResult score_categorical_motif_dictionary_temporal(
    IObservationSource& source, TemporalCategoricalSplit split, CodeLimits limits) {
    return score_dictionary_window(source, split, limits);
}

TemporalMotifPermutationResult score_categorical_motif_permutations(IObservationSource& source,
                                                                     TemporalCategoricalSplit split,
                                                                     std::size_t count, std::uint64_t seed,
                                                                     CodeLimits limits) {
    return score_motif_permutations<TemporalMotifPermutationResult, TemporalMotifScorer>(
        source, split, count, seed, limits, permute_independent_frames,
        score_single_window, &TemporalCategoricalMotifCosts::charged_gain_bits);
}

TemporalMotifPermutationResult score_categorical_motif_site_permutations(IObservationSource& source,
                                                                          TemporalCategoricalSplit split,
                                                                          std::size_t count, std::uint64_t seed,
                                                                          CodeLimits limits) {
    return score_motif_permutations<TemporalMotifPermutationResult, TemporalMotifScorer>(
        source, split, count, seed, limits, permute_all_shared_sites,
        score_single_window, &TemporalCategoricalMotifCosts::charged_gain_bits);
}

TemporalDictionaryPermutationResult score_categorical_motif_dictionary_site_permutations(
    IObservationSource& source, TemporalCategoricalSplit split, std::size_t count,
    std::uint64_t seed, CodeLimits limits) {
    return score_motif_permutations<TemporalDictionaryPermutationResult, TemporalDictionaryScorer>(
        source, split, count, seed, limits, permute_all_shared_sites,
        [](auto& observations, auto regions, auto caps, auto* window) {
            return score_dictionary_window(observations, regions, caps, window);
        }, &TemporalCategoricalDictionaryCosts::charged_gain_vs_first_bits);
}

TemporalDictionaryPermutationResult score_categorical_motif_dictionary_anchored_permutations(
    IObservationSource& source, TemporalCategoricalSplit split, CategoricalMotifAnchor anchor,
    std::size_t count, std::uint64_t seed, CodeLimits limits) {
    if (anchor.width < 2 || anchor.width > categorical_motif_max_width || anchor.start > split.sites ||
        anchor.width > split.sites - anchor.start || split.sites - anchor.width < 2 || split.train_snapshots < 2) {
        throw Error(ErrorCode::invalid, "invalid motif anchor or too few anchored training frames or movable sites");
    }
    return score_motif_permutations<TemporalDictionaryPermutationResult, TemporalDictionaryScorer>(
        source, split, count, seed, limits,
        [anchor](const MotifWindows& window, std::size_t sites, std::uint64_t& state) {
            return permute_shared_sites(window, sites, state, anchor);
        }, [anchor](auto& observations, auto regions, auto caps, auto* window) {
            return score_dictionary_window(observations, regions, caps, window, anchor);
        }, &TemporalCategoricalDictionaryCosts::charged_gain_vs_first_bits, TemporalDictionaryScorer{anchor});
}
}
