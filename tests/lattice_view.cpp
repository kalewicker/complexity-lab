#include <clab/categorical_code.hpp>
#include <clab/categorical_temporal.hpp>
#include <clab/lattice_view.hpp>
#include <clab/trace.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

template<class Function>
void invalid(Function action) {
    try {
        action();
    } catch (const clab::Error& error) {
        require(error.code() == clab::ErrorCode::invalid, "unexpected lattice error code");
        return;
    }
    throw std::runtime_error("invalid lattice accepted");
}

struct Incidence {
    std::vector<clab::EntityRow> entities;
    std::vector<clab::RelationRow> relations;
    std::vector<clab::PortRow> ports;

    void edge(clab::EntityId a, clab::EntityId b) {
        const auto id = static_cast<clab::RelationId>(relations.size() + 1);
        relations.push_back({id, 0, static_cast<std::uint32_t>(ports.size()), 2, false});
        ports.push_back({a, 0});
        ports.push_back({b, 0});
    }

    clab::RelationalSnapshot snapshot() const {
        return clab::RelationalSnapshot(entities, relations, ports);
    }
};

constexpr std::size_t rows = 2, columns = 6, sites = rows * columns;
using Frame = std::array<clab::TypeId, sites>;
using SiteMap = std::array<clab::EntityId, sites>;
constexpr SiteMap graph_ids{22, 55, 11, 99, 303, 101, 70, 90, 170, 8, 12, 81};
constexpr SiteMap hypergraph_ids{200, 900, 3, 4, 17, 24, 1, 7, 91, 16, 27, 5};

Incidence graph_frame(const Frame& frame) {
    Incidence graph;
    for (std::size_t i = 0; i < sites; ++i) {
        graph.entities.push_back({graph_ids[i], frame[i]});
        for (std::size_t j = i + 1; j < sites; ++j) {
            if (j - i == columns || (j - i == 1 && i / columns == j / columns)) {
                graph.edge(graph_ids[i], graph_ids[j]);
            }
        }
    }
    return graph;
}

Incidence hypergraph_frame(const Frame& frame) {
    Incidence graph;
    for (std::size_t remaining = sites; remaining > 0; --remaining) {
        const auto i = remaining - 1;
        graph.entities.push_back({hypergraph_ids[i], frame[i]});
        if (i >= columns) {
            graph.edge(hypergraph_ids[i], hypergraph_ids[i - columns]);
        }
        if (i % columns != 0) {
            graph.edge(hypergraph_ids[i], hypergraph_ids[i - 1]);
        }
    }
    return graph;
}

std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "open lattice trace");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void reject_invalid_lattices() {
    using clab::categorical_lattice_view;
    const auto graph = graph_frame(Frame{});
    const auto snapshot = graph.snapshot();
    invalid([&] { (void)categorical_lattice_view(snapshot, 0, columns, graph_ids); });
    invalid([&] { (void)categorical_lattice_view(snapshot, rows, 0, graph_ids); });
    invalid([&] { (void)categorical_lattice_view(snapshot, 3, columns, graph_ids); });
    invalid([&] { (void)categorical_lattice_view(snapshot, rows, columns, std::span(graph_ids).first(sites-1)); });
    for (auto replacement : {graph_ids[1], clab::EntityId{999}}) {
        auto wrong_map = graph_ids;
        wrong_map[0] = replacement;
        invalid([&] { (void)categorical_lattice_view(snapshot, rows, columns, wrong_map); });
    }
    const auto rejects = [](const Incidence& wrong) {
        const auto relational = wrong.snapshot();
        invalid([&] { (void)categorical_lattice_view(relational, rows, columns, graph_ids); });
    };
    auto wrong = graph;
    wrong.relations.pop_back();
    wrong.ports.resize(wrong.ports.size() - 2);
    rejects(wrong);
    for (const auto& [a, b] : std::array<std::array<std::size_t, 2>, 4>{{{1, 0}, {5, 6}, {0, 7}, {0, 0}}}) {
        wrong = graph;
        wrong.edge(graph_ids[a], graph_ids[b]);
        rejects(wrong);
    }
    wrong = graph;
    wrong.relations.back().port_count = 1;
    wrong.ports.pop_back();
    rejects(wrong);
    wrong = graph;
    wrong.relations.front().ordered = true;
    rejects(wrong);
    wrong = graph;
    wrong.relations.front().type = 1;
    rejects(wrong);
    wrong = graph;
    wrong.ports.front().role = 1;
    rejects(wrong);
    const std::array schema{clab::AttributeDescriptor{
        clab::AttributeOwner::entity, "extra", false, clab::CategoricalAttribute{{"x"}},
    }};
    const std::array attributes{clab::AttributeColumn(sites)};
    const clab::RelationalSnapshot attributed(graph.entities, graph.relations, graph.ports, attributes, schema);
    invalid([&] { (void)categorical_lattice_view(attributed, rows, columns, graph_ids); });

    Incidence line;
    line.entities = {{96, 7}, {101, 9}};
    line.edge(101, 96);
    const std::array<clab::EntityId, 2> map{96, 101};
    for (const auto& [height, width] : std::array<std::array<std::size_t, 2>, 2>{{{1, 2}, {2, 1}}}) {
        const auto view = categorical_lattice_view(line.snapshot(), height, width, map);
        require(std::ranges::equal(view.symbols(), std::array{7u, 9u}), "lattice line projection");
    }
    const clab::RelationalSnapshot singleton(std::array<clab::EntityRow, 1>{{{1, 9}}}, {}, {});
    const auto view = categorical_lattice_view(singleton, 1, 1, std::array<clab::EntityId, 1>{1});
    require(std::ranges::equal(view.symbols(), std::array{9u}), "singleton lattice projection");
}

void project_trace(const std::filesystem::path& source, const std::filesystem::path& output,
                   const SiteMap& map, const clab::Metadata& meta) {
    clab::TraceReader reader(source);
    clab::TraceWriter writer(output, meta);
    while (auto record = reader.next()) {
        const auto& incidence = std::get<clab::RelationalSnapshot>(record->payload);
        writer.append({record->stamp, clab::categorical_lattice_view(incidence, rows, columns, map)});
    }
    require(reader.status() == clab::TraceStatus::complete, "incomplete relational lattice fixture");
    writer.finish();
}
}

void lattice_view_fixtures(const std::filesystem::path& directory) {
    reject_invalid_lattices();
    const auto dense_path = directory / "f12-dense.cltr";
    const auto graph_path = directory / "f12-graph.cltr";
    const auto hypergraph_path = directory / "f12-hypergraph.cltr";
    const auto common_graph = directory / "f12-common-graph.cltr";
    const auto common_hypergraph = directory / "f12-common-hypergraph.cltr";
    const clab::Metadata meta{"f12-common", "categorical-u32", "step", true, true};
    clab::TraceWriter dense(dense_path, meta);
    clab::TraceWriter graph(graph_path, {"f12-graph", "fixture-graph", "step", true, false});
    clab::TraceWriter hypergraph(hypergraph_path, {"f12-hypergraph", "fixture-hypergraph", "step", true, false});
    const std::array<clab::TypeId, 6> a{1, 2, 3, 4, 5, 6}, b{1, 3, 5, 2, 4, 6};
    for (std::size_t i = 0; i < 7; ++i) {
        Frame labels{};
        const auto& pattern = i < 2 || i == 5 ? a : b;
        std::ranges::copy(pattern, labels.begin());
        for (std::size_t site = columns; site < sites; ++site) {
            labels[site] = static_cast<clab::TypeId>(100 + i * sites + site);
        }
        const clab::Stamp stamp{i, double(i), i};
        dense.append({stamp, clab::Snapshot(labels)});
        graph.append({stamp, graph_frame(labels).snapshot()});
        hypergraph.append({stamp, hypergraph_frame(labels).snapshot()});
    }
    dense.finish();
    graph.finish();
    hypergraph.finish();
    project_trace(graph_path, common_graph, graph_ids, meta);
    project_trace(hypergraph_path, common_hypergraph, hypergraph_ids, meta);
    const auto reference = bytes(dense_path);
    require(bytes(graph_path) != bytes(hypergraph_path), "distinct native lattice representations");
    require(bytes(common_graph) == reference && bytes(common_hypergraph) == reference,
            "common lattice encodings differ from dense reference");

    clab::TraceReader snapshot_source(dense_path);
    const auto frame = snapshot_source.next();
    const auto baseline = clab::compare_categorical(std::get<clab::Snapshot>(frame->payload).symbols());
    require(baseline.winner.has_value(), "dense lattice baseline selection");
    clab::TraceReader temporal_source(dense_path);
    const clab::TemporalCategoricalSplit split{4, 1, 2, sites};
    const auto reference_score = clab::score_categorical_motif_dictionary_temporal(temporal_source, split);
    require(reference_score.status == clab::MetricStatus::valid && reference_score.costs &&
            reference_score.costs->dictionary, "dense lattice dictionary admission");
    const auto& expected = *reference_score.costs->dictionary;
    require(expected.occurrences == std::array<std::uint64_t, 2>{1, 1},
            "both motifs must transfer in the lattice fixture");
    for (const auto& path : {common_graph, common_hypergraph}) {
        clab::TraceReader source(path);
        const auto first = source.next();
        const auto score = clab::compare_categorical(std::get<clab::Snapshot>(first->payload).symbols());
        require(score.winner && score.winner->family == baseline.winner->family &&
                score.winner->encoded.length == baseline.winner->encoded.length,
                "projected lattice baseline differs");
        clab::TraceReader temporal(path);
        const auto result = clab::score_categorical_motif_dictionary_temporal(temporal, split);
        require(result.status == reference_score.status && result.costs && result.costs->dictionary &&
                result.costs->baseline_model.length == reference_score.costs->baseline_model.length &&
                result.costs->baseline_test_bits == reference_score.costs->baseline_test_bits,
                "projected lattice comparator differs");
        const auto& dictionary = *result.costs->dictionary;
        require(dictionary.model.bytes == expected.model.bytes && dictionary.test == expected.test &&
                dictionary.first_test_bits == expected.first_test_bits &&
                dictionary.placement_bits == expected.placement_bits &&
                dictionary.occurrences == expected.occurrences, "projected lattice dictionary differs");
    }
}
