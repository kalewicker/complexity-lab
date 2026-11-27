#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <utility>

namespace clab {
namespace {
constexpr std::string_view symbol_histogram_id = "artifact.symbol_histogram.v1";
constexpr std::string_view history_id = "artifact.snapshot_history.v1";
constexpr std::string_view entity_histogram_id = "artifact.entity_type_histogram.v1";
constexpr std::string_view relation_histograms_id = "artifact.relation_histograms.v1";
constexpr std::string_view entity_index_id = "artifact.entity_index.v1";
constexpr std::string_view incidence_id = "artifact.relational_incidence.v1";
constexpr std::string_view event_totals_id = "artifact.event_totals.v1";

constexpr std::array symbol_histogram{symbol_histogram_id}, snapshot_history{history_id};
constexpr std::array entity_histogram{entity_histogram_id}, relation_histograms{relation_histograms_id};
constexpr std::array entity_index{entity_index_id}, incidence{incidence_id}, event_totals{event_totals_id};

constexpr std::array artifacts{
    DependencySpec{symbol_histogram_id}, DependencySpec{history_id},
    DependencySpec{entity_histogram_id}, DependencySpec{relation_histograms_id},
    DependencySpec{entity_index_id}, DependencySpec{incidence_id, entity_index},
    DependencySpec{event_totals_id},
};

constexpr MetricSpec site_count{"state.symbol_count.v1", "categorical", "symbols"};
constexpr MetricSpec entropy{"state.shannon_bits_per_symbol.v1", "categorical", "bits_per_symbol", symbol_histogram};
constexpr MetricSpec type_count{"state.type_count.v1", "categorical", "symbols", symbol_histogram};
constexpr MetricSpec compression_ratio{"state.rle_bytes_per_raw_byte.v1", "categorical", "bytes_per_byte"};
constexpr MetricSpec longest_run{"state.longest_symbol_run.v1", "categorical", "symbols"};
constexpr MetricSpec changed{"state.changed_sites.v1", "categorical", "sites", snapshot_history};
constexpr MetricSpec recurrence{"state.recurrence_lag_snapshots.v1", "categorical", "snapshots", snapshot_history};
constexpr MetricSpec lookback{"state.recurrence_lookback_snapshots.v1", "categorical", "snapshots", snapshot_history};
constexpr MetricSpec event_count{"activity.events.v1", "events", "events", event_totals};
constexpr MetricSpec births{"activity.births.v1", "events", "entities", event_totals};
constexpr MetricSpec deaths{"activity.deaths.v1", "events", "entities", event_totals};
constexpr MetricSpec observed_events{"activity.observed_events.v1", "events", "events", event_totals};
constexpr MetricSpec reconstructed_events{"activity.reconstructed_events.v1", "events", "events", event_totals};
constexpr MetricSpec entities{"relational.entity_count.v1", "relational", "entities"};
constexpr MetricSpec relations{"relational.relation_count.v1", "relational", "relations"};
constexpr MetricSpec ports{"relational.port_count.v1", "relational", "ports"};
constexpr MetricSpec entity_types{"relational.entity_type_count.v1", "relational", "entities", entity_histogram};
constexpr MetricSpec relation_types{"relational.relation_type_count.v1", "relational", "relations", relation_histograms};
constexpr MetricSpec arities{"relational.arity_count.v1", "relational", "relations", relation_histograms};
constexpr MetricSpec components{"relational.component_count.v1", "relational", "components", incidence};
constexpr MetricSpec port_degrees{"relational.port_degree_count.v1", "relational", "entities", incidence};
constexpr MetricSpec logical_updates{"work.logical_updates.v1", "ledger", "updates"};
constexpr MetricSpec reads{"work.reads.v1", "ledger", "reads"};
constexpr MetricSpec writes{"work.writes.v1", "ledger", "writes"};
constexpr MetricSpec rng_draws{"work.rng_draws.v1", "ledger", "draws"};
constexpr MetricSpec live_state_bytes{"resource.live_state_bytes.v1", "ledger", "bytes"};
constexpr MetricSpec peak_state_bytes{"resource.peak_state_bytes.v1", "ledger", "bytes"};

constexpr std::array catalog{
    site_count, entropy, type_count, compression_ratio, longest_run, changed, recurrence, lookback,
    entities, relations, ports, entity_types, relation_types, arities, components, port_degrees,
    event_count, births, deaths, observed_events, reconstructed_events,
    logical_updates, reads, writes, rng_draws, live_state_bytes, peak_state_bytes,
};

std::uint64_t longest_symbol_run(std::span<const TypeId> symbols) {
    std::uint64_t longest = 0, length = 0;
    TypeId previous{};
    for (const auto symbol : symbols) {
        length = symbol == previous ? length + 1 : 1;
        longest = std::max(longest, length);
        previous = symbol;
    }
    return longest;
}

struct IncidenceDiagnostics {
    std::uint64_t component_count{};
    std::map<std::uint64_t, std::uint64_t> degree_counts;
};

std::vector<EntityId> index_entities(const RelationalSnapshot& graph) {
    std::vector<EntityId> ids;
    ids.reserve(graph.entities().size());
    for (const auto& row : graph.entities()) {
        ids.push_back(row.id);
    }
    std::ranges::sort(ids);
    return ids;
}

IncidenceDiagnostics measure_incidence(const RelationalSnapshot& graph,
                                       std::span<const EntityId> ids) {
    struct Node {
        std::size_t parent;
        std::uint64_t degree{};
        std::size_t component_size{1};
    };
    std::vector<Node> nodes;
    nodes.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        nodes.push_back({i});
    }
    const auto root = [&](std::size_t node) {
        while (nodes[node].parent != node) {
            nodes[node].parent = nodes[nodes[node].parent].parent;
            node = nodes[node].parent;
        }
        return node;
    };

    IncidenceDiagnostics result{ids.size(), {}};
    for (const auto& relation : graph.relations()) {
        std::optional<std::size_t> anchor;
        for (const auto& port : graph.ports().subspan(relation.first_port, relation.port_count)) {
            // Snapshot validation guarantees that each endpoint is in the table.
            const auto index = static_cast<std::size_t>(
                std::ranges::lower_bound(ids, port.entity) - ids.begin());
            ++nodes[index].degree;
            if (!anchor) {
                anchor = index;
                continue;
            }
            auto a = root(*anchor), b = root(index);
            if (a == b) {
                continue;
            }
            if (nodes[a].component_size < nodes[b].component_size) {
                std::swap(a, b);
            }
            nodes[b].parent = a;
            nodes[a].component_size += nodes[b].component_size;
            --result.component_count;
        }
    }
    for (const auto& node : nodes) {
        ++result.degree_counts[node.degree];
    }
    return result;
}

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

std::span<const MetricSpec> metric_catalog() noexcept { return catalog; }

const MetricSpec* find_metric(std::string_view id) noexcept {
    const auto found = std::ranges::find(catalog, id, &MetricSpec::id);
    return found == catalog.end() ? nullptr : &*found;
}

std::vector<std::string> plan_metrics(std::span<const std::string_view> selected) {
    for (const auto id : selected) {
        if (!find_metric(id)) {
            throw Error(ErrorCode::invalid, "unknown metric ID: " + std::string(id));
        }
    }
    const auto graph = [] {
        std::vector<DependencySpec> nodes(artifacts.begin(), artifacts.end());
        for (const auto& metric : catalog) {
            nodes.push_back({metric.id, metric.prerequisites});
        }
        return nodes;
    };
    return resolve_dependencies(graph(), selected);
}

void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits) {
    evaluate(source, sink, limits, {});
}

static void evaluate_impl(IObservationSource& source, const MetricSink& sink, EvalLimits limits,
                          std::span<const std::string_view> selected, HistogramCache* cache) {
    const auto plan = plan_metrics(selected);
    const auto needs = [&](std::string_view id) {
        return std::ranges::find(plan, id) != plan.end();
    };
    const bool needs_history = needs(history_id);
    const bool needs_symbols = needs(symbol_histogram_id);
    const bool needs_entity_histogram = needs(entity_histogram_id);
    const bool needs_relation_histograms = needs(relation_histograms_id);
    const bool needs_incidence = needs(incidence_id);
    const bool needs_event_totals = needs(event_totals_id);
    const auto wants = [&](const MetricSpec& metric) {
        return selected.empty() || std::ranges::find(selected, metric.id) != selected.end();
    };
    std::deque<Snapshot> history;
    std::size_t history_bytes = 0;
    std::optional<std::size_t> fixed_site_count;
    bool saw_snapshot = false;
    bool saw_relational = false;
    bool saw_events = false;
    const auto emit = [&](std::optional<Stamp> stamp, const MetricSpec& metric, MetricValue value,
                          std::optional<TypeId> symbol = std::nullopt) {
        if (wants(metric)) {
            sink({stamp, std::string(metric.id), symbol, value, MetricStatus::valid});
        }
    };
    const auto missing = [&](std::optional<Stamp> stamp, const MetricSpec& metric, MetricStatus status) {
        if (wants(metric)) {
            sink({stamp, std::string(metric.id), std::nullopt, std::nullopt, status});
        }
    };

    while (auto record = source.next()) {
        const auto stamp = record->stamp;
        if (auto* snap = std::get_if<Snapshot>(&record->payload)) {
            saw_snapshot = true;
            const auto symbols = snap->symbols();
            emit(stamp, site_count, std::uint64_t(symbols.size()));
            if (wants(longest_run)) {
                emit(stamp, longest_run, longest_symbol_run(symbols));
            }
            if (needs_symbols) {
                const auto histogram = cache ? cache->get(source.metadata(), symbols)
                                             : count_symbols(symbols);
                double bits = 0;
                for (const auto [symbol, count] : histogram) {
                    const auto probability = static_cast<double>(count) / symbols.size();
                    bits -= probability * std::log2(probability);
                    emit(stamp, type_count, count, symbol);
                }
                if (symbols.empty()) {
                    missing(stamp, entropy, MetricStatus::insufficient_data);
                } else {
                    emit(stamp, entropy, bits);
                }
            }
            if (wants(compression_ratio)) {
                if (symbols.empty()) {
                    missing(stamp, compression_ratio, MetricStatus::insufficient_data);
                } else {
                    emit(stamp, compression_ratio, rle_ratio(symbols));
                }
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
            if (!needs_history) {
                continue;
            }
            emit(stamp, lookback, std::uint64_t(history.size()));
            if (history.empty()) {
                const auto status = first ? MetricStatus::insufficient_data : MetricStatus::budget_exceeded;
                missing(stamp, changed, status);
                missing(stamp, recurrence, status);
            } else {
                if (wants(changed)) {
                    const auto previous = history.back().symbols();
                    std::uint64_t changes = 0;
                    for (std::size_t i = 0; i < symbols.size(); ++i) {
                        changes += symbols[i] != previous[i];
                    }
                    emit(stamp, changed, changes);
                }
                if (wants(recurrence)) {
                    std::uint64_t lag = 1;
                    for (auto it = history.rbegin(); it != history.rend(); ++it, ++lag) {
                        if (std::ranges::equal(symbols, it->symbols())) {
                            break;
                        }
                    }
                    emit(stamp, recurrence, lag <= history.size() ? lag : 0);
                }
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
            if (needs_entity_histogram) {
                std::map<TypeId, std::uint64_t> histogram;
                for (const auto& entity : graph->entities()) {
                    ++histogram[entity.type];
                }
                for (const auto& [kind, count] : histogram) {
                    emit(stamp, entity_types, count, kind);
                }
            }
            if (needs_relation_histograms) {
                std::map<TypeId, std::uint64_t> type_histogram, arity_histogram;
                for (const auto& relation : graph->relations()) {
                    ++type_histogram[relation.type];
                    ++arity_histogram[relation.port_count];
                }
                for (const auto& [kind, count] : type_histogram) {
                    emit(stamp, relation_types, count, kind);
                }
                for (const auto& [arity, count] : arity_histogram) {
                    emit(stamp, arities, count, arity);
                }
            }
            if (needs_incidence) {
                const auto ids = index_entities(*graph);
                const auto result = measure_incidence(*graph, ids);
                emit(stamp, components, result.component_count);
                if (!result.degree_counts.empty() &&
                    result.degree_counts.rbegin()->first > std::numeric_limits<TypeId>::max()) {
                    missing(stamp, port_degrees, MetricStatus::budget_exceeded);
                } else {
                    for (const auto& [degree, count] : result.degree_counts) {
                        emit(stamp, port_degrees, count, static_cast<TypeId>(degree));
                    }
                }
            }
        } else if (const auto* counts = std::get_if<EventCounts>(&record->payload)) {
            saw_events = true;
            emit(stamp, event_count, counts->events);
            emit(stamp, births, counts->births);
            emit(stamp, deaths, counts->deaths);
            missing(stamp, observed_events, MetricStatus::unsupported);
            missing(stamp, reconstructed_events, MetricStatus::unsupported);
        } else if (const auto* batch = std::get_if<EventBatch>(&record->payload)) {
            saw_events = true;
            if (!needs_event_totals) {
                continue;
            }
            EventCounts totals;
            std::uint64_t observed = 0;
            for (const auto& event : batch->events()) {
                ++totals.events;
                totals.births = checked_add(totals.births, event.created.size());
                totals.deaths = checked_add(totals.deaths, event.deleted.size());
                observed += event.provenance == EventProvenance::observed;
            }
            emit(stamp, event_count, totals.events);
            emit(stamp, births, totals.births);
            emit(stamp, deaths, totals.deaths);
            emit(stamp, observed_events, observed);
            emit(stamp, reconstructed_events, totals.events - observed);
        } else if (const auto* ledger = std::get_if<WorkLedger>(&record->payload)) {
            emit(stamp, logical_updates, ledger->logical_updates);
            emit(stamp, reads, ledger->reads);
            emit(stamp, writes, ledger->writes);
            emit(stamp, rng_draws, ledger->rng_draws);
            emit(stamp, live_state_bytes, ledger->live_state_bytes);
            emit(stamp, peak_state_bytes, ledger->peak_state_bytes);
        }
    }
    for (const auto& metric : catalog) {
        if ((metric.channel == "categorical" && !saw_snapshot) ||
            (metric.channel == "relational" && !saw_relational) ||
            (metric.channel == "events" && !saw_events)) {
            missing(std::nullopt, metric, MetricStatus::unsupported);
        }
    }
}
void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits,
              std::span<const std::string_view> selected) {
    evaluate_impl(source, sink, limits, selected, nullptr);
}

void evaluate(IObservationSource& source, const MetricSink& sink, EvalLimits limits,
              std::span<const std::string_view> selected, HistogramCache& cache) {
    evaluate_impl(source, sink, limits, selected, &cache);
}

}
