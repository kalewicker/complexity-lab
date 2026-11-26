#include <clab/evaluator.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <utility>

namespace clab {
namespace {
constexpr auto site_count = "state.symbol_count.v1";
constexpr auto entropy = "state.shannon_bits_per_symbol.v1";
constexpr auto type_count = "state.type_count.v1";
constexpr auto compression_ratio = "state.rle_bytes_per_raw_byte.v1";
constexpr auto changed = "state.changed_sites.v1";
constexpr auto recurrence = "state.recurrence_lag_snapshots.v1";
constexpr auto lookback = "state.recurrence_lookback_snapshots.v1";
constexpr auto event_count = "activity.events.v1";
constexpr auto births = "activity.births.v1";
constexpr auto deaths = "activity.deaths.v1";
constexpr auto entities = "relational.entity_count.v1";
constexpr auto relations = "relational.relation_count.v1";
constexpr auto ports = "relational.port_count.v1";
constexpr auto entity_types = "relational.entity_type_count.v1";
constexpr auto relation_types = "relational.relation_type_count.v1";
constexpr auto arities = "relational.arity_count.v1";

double rle_ratio(std::span<const TypeId> symbols) {
    std::uint64_t runs = 0;
    unsigned length = 0;
    std::uint8_t last = 0;
    for (const auto symbol : symbols) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const auto byte = static_cast<std::uint8_t>(symbol >> shift);
            if (!length || length == 255 || byte != last) {
                ++runs;
                length = 0;
                last = byte;
            }
            ++length;
        }
    }
    return 2.0 * static_cast<double>(runs) / static_cast<double>(symbols.size_bytes());
}
}

void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits) {
    std::deque<Snapshot> history;
    std::size_t history_bytes = 0;
    std::optional<std::size_t> fixed_site_count;
    bool saw_snapshot = false;
    bool saw_relational = false;
    bool saw_events = false;
    const auto emit = [&](auto stamp, const char* id, MetricValue value,
                          std::optional<TypeId> symbol = std::nullopt) {
        sink({stamp, id, symbol, value, MetricStatus::valid});
    };
    const auto missing = [&](auto stamp, const char* id, MetricStatus status) {
        sink({stamp, id, std::nullopt, std::nullopt, status});
    };

    while (auto record = source.next()) {
        const auto stamp = record->stamp;
        if (auto* snap = std::get_if<Snapshot>(&record->payload)) {
            saw_snapshot = true;
            const auto symbols = snap->symbols();
            std::map<TypeId, std::uint64_t> histogram;
            for (auto symbol : symbols) {
                ++histogram[symbol];
            }
            emit(stamp, site_count, std::uint64_t(symbols.size()));
            double bits = 0;
            for (auto [symbol, count] : histogram) {
                const auto probability = static_cast<double>(count) / symbols.size();
                bits -= probability * std::log2(probability);
                emit(stamp, type_count, count, symbol);
            }
            if (symbols.empty()) {
                missing(stamp, entropy, MetricStatus::insufficient_data);
                missing(stamp, compression_ratio, MetricStatus::insufficient_data);
            } else {
                emit(stamp, entropy, bits);
                emit(stamp, compression_ratio, rle_ratio(symbols));
            }

            if (!source.metadata().fixed_sites) {
                missing(stamp, changed, MetricStatus::unsupported);
                missing(stamp, recurrence, MetricStatus::unsupported);
                missing(stamp, lookback, MetricStatus::unsupported);
                continue;
            }
            const bool first = !fixed_site_count.has_value();
            if (!first && *fixed_site_count != symbols.size()) {
                throw Error(ErrorCode::invalid, "fixed-site snapshots differ in size");
            }
            fixed_site_count = symbols.size();
            emit(stamp, lookback, std::uint64_t(history.size()));
            if (history.empty()) {
                const auto status = first ? MetricStatus::insufficient_data : MetricStatus::budget_exceeded;
                missing(stamp, changed, status);
                missing(stamp, recurrence, status);
            } else {
                auto previous = history.back().symbols();
                std::uint64_t changes = 0;
                for (std::size_t i = 0; i < symbols.size(); ++i) {
                    changes += symbols[i] != previous[i];
                }
                emit(stamp, changed, changes);
                std::uint64_t lag = 1;
                for (auto it = history.rbegin(); it != history.rend(); ++it, ++lag) {
                    if (std::ranges::equal(symbols, it->symbols())) {
                        break;
                    }
                }
                emit(stamp, recurrence, lag <= history.size() ? lag : 0);
            }

            if (symbols.size_bytes() > limits.recurrence_bytes || limits.recurrence_snapshots == 0) {
                continue;
            }
            while (history.size() >= limits.recurrence_snapshots ||
                   symbols.size_bytes() > limits.recurrence_bytes - history_bytes) {
                history_bytes -= history.front().symbols().size_bytes();
                history.pop_front();
            }
            history_bytes += symbols.size_bytes();
            history.push_back(std::move(*snap));
        } else if (const auto* graph = std::get_if<RelationalSnapshot>(&record->payload)) {
            saw_relational = true;
            emit(stamp, entities, std::uint64_t(graph->entities().size()));
            emit(stamp, relations, std::uint64_t(graph->relations().size()));
            emit(stamp, ports, std::uint64_t(graph->ports().size()));
            std::map<TypeId, std::uint64_t> entity_histogram, relation_histogram, arity_histogram;
            for (const auto& entity : graph->entities()) {
                ++entity_histogram[entity.type];
            }
            for (const auto& relation : graph->relations()) {
                ++relation_histogram[relation.type];
                ++arity_histogram[relation.port_count];
            }
            for (const auto& [kind, count] : entity_histogram) {
                emit(stamp, entity_types, count, kind);
            }
            for (const auto& [kind, count] : relation_histogram) {
                emit(stamp, relation_types, count, kind);
            }
            for (const auto& [arity, count] : arity_histogram) {
                emit(stamp, arities, count, arity);
            }
        } else if (const auto* counts = std::get_if<EventCounts>(&record->payload)) {
            saw_events = true;
            emit(stamp, event_count, counts->events);
            emit(stamp, births, counts->births);
            emit(stamp, deaths, counts->deaths);
        } else {
            const auto& ledger = std::get<WorkLedger>(record->payload);
            emit(stamp, "work.logical_updates.v1", ledger.logical_updates);
            emit(stamp, "work.reads.v1", ledger.reads);
            emit(stamp, "work.writes.v1", ledger.writes);
            emit(stamp, "work.rng_draws.v1", ledger.rng_draws);
            emit(stamp, "resource.live_state_bytes.v1", ledger.live_state_bytes);
            emit(stamp, "resource.peak_state_bytes.v1", ledger.peak_state_bytes);
        }
    }
    if (!saw_snapshot) {
        for (auto id : {site_count, entropy, type_count, compression_ratio, changed, recurrence, lookback}) {
            missing(std::nullopt, id, MetricStatus::unsupported);
        }
    }
    if (!saw_relational) {
        for (auto id : {entities, relations, ports, entity_types, relation_types, arities}) {
            missing(std::nullopt, id, MetricStatus::unsupported);
        }
    }
    if (!saw_events) {
        for (auto id : {event_count, births, deaths}) {
            missing(std::nullopt, id, MetricStatus::unsupported);
        }
    }
}
}
