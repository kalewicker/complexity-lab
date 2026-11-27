#include <clab/categorical_code.hpp>
#include <clab/categorical_heldout.hpp>
#include <clab/categorical_motif.hpp>
#include <clab/categorical_temporal.hpp>
#include <clab/evaluator.hpp>
#include <clab/trace.hpp>
#ifdef CLAB_TEST_SUBSTRATES
#include <clab/substrate.hpp>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <stdlib.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <tuple>
#include <utility>

namespace {
void require(bool ok, const char* description) {
    if (!ok) {
        throw std::runtime_error(description);
    }
}

template<class Function>
void expect_error(clab::ErrorCode code, Function action) {
    try {
        action();
    } catch (const clab::Error& error) {
        require(error.code() == code, "unexpected error code");
        return;
    }
    throw std::runtime_error("expected operation to fail");
}

struct TempDirectory {
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "clab-core-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) {
            throw std::runtime_error("could not create test directory");
        }
        path = pattern;
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

void round_trips(const std::filesystem::path& directory) {
    clab::Rng rng(0);
    require(rng.next() == 0xe220a8397b1dcdafULL, "first RNG vector");
    require(rng.next() == 0x6e789e6aa1b965f4ULL, "second RNG vector");
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::checked_add(max, 1); });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::checked_multiply(max, 2); });

    const auto path = directory / "all.cltr";
    const clab::Metadata meta{"round-trip", "test-u32", "step", true, false};
    clab::TraceWriter writer(path, meta);
    std::vector<std::vector<clab::TypeId>> expected;
    for (std::uint64_t i = 0; i < 100; ++i) {
        std::vector<clab::TypeId> symbols(rng.next() % 40);
        for (auto& symbol : symbols) {
            symbol = static_cast<clab::TypeId>(rng.next());
        }
        const clab::Snapshot immutable(symbols);
        expected.push_back(symbols);
        std::ranges::fill(symbols, 0);
        writer.append({{i, static_cast<double>(i), i}, immutable});
    }
    const clab::EventCounts events{4, 1, 2};
    const clab::WorkLedger ledger{100, 200, 99, 17, 64, 96};
    writer.append({{100, 100, 100}, events});
    writer.append({{101, 100, 100}, ledger});
    require(!std::filesystem::exists(path), "trace published before end");
    writer.finish();
    writer.finish();
    clab::TraceReader reader(path);
    require(reader.metadata() == meta, "metadata round trip");
    for (std::uint64_t i = 0; i < expected.size(); ++i) {
        auto record = reader.next();
        require(record && record->stamp == clab::Stamp{i, static_cast<double>(i), i}, "stamp");
        require(std::ranges::equal(std::get<clab::Snapshot>(record->payload).symbols(), expected[i]),
                "snapshot round trip");
    }
    const auto event_record = reader.next();
    require(event_record && event_record->stamp == clab::Stamp{100, 100, 100} &&
            std::get<clab::EventCounts>(event_record->payload) == events, "aggregate event round trip");
    const auto ledger_record = reader.next();
    require(ledger_record && ledger_record->stamp == clab::Stamp{101, 100, 100} &&
            std::get<clab::WorkLedger>(ledger_record->payload) == ledger, "ledger round trip");
    require(!reader.next() && reader.status() == clab::TraceStatus::complete, "end chunk");
    clab::TraceWriter collision(path, meta);
    expect_error(clab::ErrorCode::io, [&] { collision.finish(); });
    clab::TraceReader original(path);
    require(original.next().has_value(), "publication replaced an existing trace");

    std::filesystem::path partial;
    {
        clab::TraceWriter interrupted(directory / "never-published.cltr", meta);
        interrupted.append({{}, clab::EventCounts{2, 1, 1}});
        partial = interrupted.staging_path();
    }
    clab::TraceReader recovered(partial, clab::TraceMode::recover_prefix);
    require(recovered.next().has_value() && !recovered.next() &&
            recovered.status() == clab::TraceStatus::incomplete, "recover unfinished writer");
}

void relational_round_trip(const std::filesystem::path& directory) {
    const auto path = directory / "relational.cltr";
    std::array<clab::EntityRow, 2> entities{{{42, 1}, {7, 3}}};
    const std::array<clab::RelationRow, 2> relations{{{6, 2, 0, 3, true}, {7, 9, 3, 0, false}}};
    const std::array<clab::PortRow, 3> ports{{{42, 4}, {7, 5}, {42, 5}}};
    const clab::RelationalSnapshot snapshot(entities, relations, ports);
    const auto original = entities;
    entities[0].id = 7;
    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::RelationalSnapshot(entities, relations, ports);
    });
    clab::TraceWriter writer(path, {"relational", "fixture-v1", "step", true, false});
    writer.append({{}, snapshot});
    writer.finish();

    clab::TraceReader reader(path);
    const auto record = reader.next();
    require(record.has_value(), "missing relational snapshot");
    const auto& mapped = std::get<clab::RelationalSnapshot>(record->payload);
    require(std::ranges::equal(mapped.entities(), original), "relational entity round trip");
    require(std::ranges::equal(mapped.relations(), relations), "relation round trip");
    require(std::ranges::equal(mapped.ports(), ports), "relational port round trip");
    require(!reader.next() && reader.status() == clab::TraceStatus::complete, "relational end");
}

void temporal_round_trip(const std::filesystem::path& directory) {
    using clab::EventProvenance;
    const auto path = directory / "temporal.cltr";
    std::array<clab::Event, 2> events{{
        {3, 9, EventProvenance::observed, 7, {10, 12}, {12}, {10}, {1, 0, 255}},
        {8, 10, EventProvenance::reconstructed, std::nullopt, {12}, {}, {}, {}},
    }};
    const clab::EventBatch batch(events);
    const auto expected = events;
    events[0].participants[0] = 12;
    expect_error(clab::ErrorCode::invalid, [&] { (void)clab::EventBatch(events); });
    clab::CheckpointRef ref{"tiny-ca.v1", 125, {}};
    for (std::size_t i = 0; i < ref.sha256.size(); ++i) {
        ref.sha256[i] = static_cast<std::uint8_t>(i * 7);
    }
    clab::TraceWriter writer(path, {"temporal", "fixture-v1", "step", false, false});
    expect_error(clab::ErrorCode::invalid, [&] { writer.append({{0, 7, 7}, batch}); });
    writer.append({{0, 8, 8}, batch});
    auto invalid_ref = ref;
    invalid_ref.format = "../checkpoint";
    expect_error(clab::ErrorCode::invalid, [&] { writer.append({{1, 8, 8}, invalid_ref}); });
    writer.append({{1, 8, 8}, ref});
    writer.finish();
    clab::TraceReader reader(path);
    const auto record = reader.next();
    require(record && std::ranges::equal(std::get<clab::EventBatch>(record->payload).events(),
                                         expected), "event batch round trip");
    const auto checkpoint = reader.next();
    require(checkpoint && std::get<clab::CheckpointRef>(checkpoint->payload) == ref,
            "checkpoint reference round trip");
    require(!reader.next() && reader.status() == clab::TraceStatus::complete, "temporal end");

    clab::TraceWriter capped(directory / "too-small.cltr",
                             {"bounded", "fixture-v1", "step", true, false}, {4096, 64});
    expect_error(clab::ErrorCode::limit, [&] { capped.append({{0, 8, 8}, batch}); });
    capped.append({{}, clab::EventBatch({})});
    capped.finish();
}

void provenance_round_trip(const std::filesystem::path& directory) {
    const auto path = directory / "provenance.cltr";
    const clab::TraceProvenance provenance{
        std::string(64, 'a'), 0, "test-producer.v1", "reference-cpu", "u32-symbols",
        {true, false, true, false, true, false, clab::TimeDomain::discrete},
    };
    const clab::Metadata meta{"provenance", "test-u32", "step", true, true, provenance};
    auto malformed = meta;
    malformed.provenance->config_sha256[0] = 'A';
    const auto entry_count = [&] {
        return std::ranges::distance(std::filesystem::directory_iterator(directory),
                                     std::filesystem::directory_iterator{});
    };
    const auto before = entry_count();
    expect_error(clab::ErrorCode::invalid, [&] { clab::TraceWriter writer(path, malformed); });
    malformed = meta;
    malformed.provenance->capabilities.time_domain = static_cast<clab::TimeDomain>(99);
    expect_error(clab::ErrorCode::invalid, [&] { clab::TraceWriter writer(path, malformed); });
    require(entry_count() == before, "invalid metadata created a staging artifact");

    clab::TraceWriter writer(path, meta);
    writer.finish();
    clab::TraceReader reader(path);
    require(reader.metadata() == meta, "provenance round trip");
    require(clab::encode_metadata_json(reader.metadata()).find("\"seed\":0") != std::string::npos,
            "present zero seed was omitted");
    require(!reader.next() && reader.status() == clab::TraceStatus::complete, "provenance end");
}

void attribute_round_trip(const std::filesystem::path& directory) {
    const auto path = directory / "attributed.cltr";
    using clab::AttributeOwner;
    const std::array<clab::EntityRow, 2> entities{{{9, 1}, {30, 1}}};
    const std::vector<clab::AttributeDescriptor> schema{
        {AttributeOwner::entity, "phase", true, clab::CategoricalAttribute{{"idle", "active"}}},
        {AttributeOwner::entity, "energy", false, clab::ContinuousAttribute{"J", -4, 4, 0.125}},
    };
    const std::vector<clab::AttributeColumn> values{
        {clab::AttributeValue(std::uint32_t{1}), clab::AttributeValue(std::uint32_t{0})},
        {clab::AttributeValue(-2.25), std::nullopt},
    };
    const clab::RelationalSnapshot snapshot(entities, {}, {}, values, schema);
    const clab::Metadata meta{"attributed", "test", "step", true, false, std::nullopt, schema};
    clab::TraceWriter writer(path, meta);
    auto renamed = schema;
    renamed.front().name = "phase-other";
    const clab::RelationalSnapshot mismatched(entities, {}, {}, values, renamed);
    expect_error(clab::ErrorCode::invalid, [&] { writer.append({{}, mismatched}); });
    auto invalid_values = values;
    invalid_values[0][1] = 1.0;
    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::RelationalSnapshot(entities, {}, {}, invalid_values, schema);
    });
    writer.append({{}, snapshot});
    writer.finish();

    clab::TraceReader reader(path);
    require(reader.metadata() == meta, "attribute schema round trip");
    const auto record = reader.next();
    require(record.has_value(), "missing attributed snapshot");
    const auto& graph = std::get<clab::RelationalSnapshot>(record->payload);
    require(std::ranges::equal(graph.attributes(), values), "attribute value round trip");
    require(std::ranges::equal(graph.attribute_schema(), schema), "snapshot schema round trip");
    require(!reader.next() && reader.status() == clab::TraceStatus::complete, "attributed end");
}

class MockGraphSource final : public clab::IObservationSource {
public:
    struct Position {
        clab::WorkCount work;
        std::uint64_t rng_state;
        bool operator==(const Position&) const = default;
    };

    MockGraphSource(std::uint64_t seed, clab::WorkCount count) : rng_(seed), count_(count) {}

    [[nodiscard]] const clab::Metadata& metadata() const noexcept override { return metadata_; }
    [[nodiscard]] Position checkpoint() const { return {work_, rng_.state()}; }
    void restore(Position position) {
        work_ = position.work;
        rng_ = clab::Rng(position.rng_state);
    }

    [[nodiscard]] clab::RelationalSnapshot observe() const {
        const std::array<clab::EntityRow, 3> entities{{{1, 1}, {2, 1}, {3, 2}}};
        const std::array<clab::RelationRow, 1> relations{{{4, 1, 0, 2, true}}};
        auto random = rng_;
        const auto bits = random.next();
        const std::array<clab::PortRow, 2> ports{{
            {1, 0}, {1 + bits % entities.size(), static_cast<std::uint32_t>((bits >> 8) & 3)},
        }};
        return clab::RelationalSnapshot(entities, relations, ports);
    }

    std::optional<clab::ObservationRecord> next() override {
        if (work_ == count_) {
            return std::nullopt;
        }
        auto observation = observe();
        const clab::Stamp stamp{work_, static_cast<double>(work_), work_};
        ++work_;
        (void)rng_.next();
        return clab::ObservationRecord{stamp, std::move(observation)};
    }

private:
    clab::Rng rng_;
    clab::WorkCount work_{}, count_;
    clab::Metadata metadata_{"mock-graph", "mock-relational-v1", "step", true, false};
};

void mock_graph_replay(const std::filesystem::path& directory) {
    for (std::uint64_t seed = 0; seed < 10; ++seed) {
        for (clab::WorkCount checkpoint_at : {0, 5, 31}) {
            MockGraphSource original(seed, checkpoint_at + 12);
            for (clab::WorkCount i = 0; i < checkpoint_at; ++i) {
                require(original.next().has_value(), "mock reached end before checkpoint");
            }
            const auto position = original.checkpoint();
            (void)original.observe();
            require(original.checkpoint() == position, "mock observation changed state");
            MockGraphSource restored(seed + 1, checkpoint_at + 12);
            restored.restore(position);
            for (int i = 0; i < 12; ++i) {
                auto expected = original.next();
                auto actual = restored.next();
                require(expected && actual && expected->stamp == actual->stamp, "mock replay stamp");
                const auto& a = std::get<clab::RelationalSnapshot>(expected->payload);
                const auto& b = std::get<clab::RelationalSnapshot>(actual->payload);
                require(std::ranges::equal(a.entities(), b.entities()) &&
                        std::ranges::equal(a.relations(), b.relations()) &&
                        std::ranges::equal(a.ports(), b.ports()), "mock replay topology");
            }
            require(original.checkpoint() == restored.checkpoint(), "mock replay RNG");
            require(!original.next() && !restored.next(), "mock replay end boundary");
        }
    }

    const auto path = directory / "mock-graph.cltr";
    MockGraphSource source(9, 12);
    clab::TraceWriter writer(path, source.metadata());
    while (const auto record = source.next()) {
        writer.append(*record);
    }
    writer.finish();
    MockGraphSource live(9, 12);
    clab::TraceReader recorded(path);
    std::vector<clab::MetricRow> live_rows, recorded_rows;
    clab::evaluate(live, [&](const auto& row) { live_rows.push_back(row); });
    clab::evaluate(recorded, [&](const auto& row) { recorded_rows.push_back(row); });
    const auto fields = [](const clab::MetricRow& row) {
        return std::tie(row.stamp, row.id, row.symbol, row.value, row.status);
    };
    require(!live_rows.empty() && std::ranges::equal(live_rows, recorded_rows,
            [&](const auto& a, const auto& b) { return fields(a) == fields(b); }),
            "mock graph live/trace diagnostic mismatch");
}

void dependency_resolution() {
    using clab::DependencySpec;
    constexpr std::array<std::string_view, 1> feature{"feature"}, input{"input"};
    const std::array graph{
        DependencySpec{"metric-b", feature}, DependencySpec{"unneeded"},
        DependencySpec{"metric-a", feature}, DependencySpec{"feature", input},
        DependencySpec{"input"},
    };
    constexpr std::array<std::string_view, 3> selected{"metric-a", "metric-b", "metric-a"};
    const std::vector<std::string> shared{"input", "feature", "metric-b", "metric-a"};
    require(clab::resolve_dependencies(graph, selected) == shared, "shared closure or order");
    constexpr std::array<std::string_view, 1> select_a{"metric-a"};
    require(clab::resolve_dependencies(graph, select_a) ==
            std::vector<std::string>{"input", "feature", "metric-a"}, "single closure");
    require(clab::resolve_dependencies(graph) ==
            std::vector<std::string>{"unneeded", "input", "feature", "metric-b", "metric-a"},
            "whole graph order");

    const auto rejected = [](std::span<const DependencySpec> nodes,
                             std::span<const std::string_view> roots = {}) {
        expect_error(clab::ErrorCode::invalid, [&] {
            (void)clab::resolve_dependencies(nodes, roots);
        });
    };
    rejected(std::array{DependencySpec{"same"}, DependencySpec{"same"}});
    rejected(std::array{DependencySpec{"output", input}});
    constexpr std::array<std::string_view, 2> repeated{"input", "input"};
    rejected(std::array{DependencySpec{"output", repeated}, DependencySpec{"input"}});
    rejected(graph, std::array<std::string_view, 1>{"absent"});
    rejected(std::array{DependencySpec{"input", input}});
    constexpr std::array<std::string_view, 1> good{"good"};
    rejected(std::array{DependencySpec{"good"}, DependencySpec{"input", feature},
                        DependencySpec{"feature", input}}, good);
}

void selected_metrics(const std::filesystem::path& directory) {
    const auto path = directory / "metric-selection.cltr";
    const clab::Metadata meta{"selection", "binary-u32", "step", true, true};
    const clab::Snapshot snapshot(std::array<clab::TypeId, 1>{0});
    clab::TraceWriter writer(path, meta);
    writer.append({{}, snapshot});
    writer.append({{1, 1, 1}, snapshot});
    writer.finish();

    clab::TraceReader trace(path);
    const std::array<std::string_view, 1> unknown{"unknown.v1"};
    expect_error(clab::ErrorCode::invalid, [&] {
        clab::evaluate(trace, [](const clab::MetricRow&) {
            throw std::runtime_error("unknown metric produced a row");
        }, {}, unknown);
    });
    require(trace.chunks_read() == 0, "unknown metric selection consumed input");

    const std::array<std::string_view, 2> selected{
        "state.recurrence_lag_snapshots.v1", "state.recurrence_lag_snapshots.v1",
    };
    std::vector<clab::MetricRow> rows;
    clab::evaluate(trace, [&](const auto& row) { rows.push_back(row); }, {0, 0}, selected);
    require(rows.size() == 2 && rows[0].id == selected[0] && rows[1].id == selected[0],
            "duplicate metric selection duplicated rows");
    require(rows[0].status == clab::MetricStatus::insufficient_data && !rows[0].value,
            "selected recurrence missing history");
    require(rows[1].status == clab::MetricStatus::budget_exceeded && !rows[1].value,
            "selected recurrence budget status");
    require(trace.status() == clab::TraceStatus::complete, "selection did not consume stream");
}

std::vector<std::uint8_t> hex_bytes(std::string_view source) {
    require(source.size() % 2 == 0, "odd golden test hex");
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < source.size(); i += 2) {
        unsigned byte{};
        const auto* begin = source.data() + i;
        const auto parsed = std::from_chars(begin, begin + 2, byte, 16);
        require(parsed.ec == std::errc{} && parsed.ptr == begin + 2, "invalid golden test hex");
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    return bytes;
}

void categorical_code_fixtures() {
    const std::array<clab::TypeId, 5> witness{3, 9, 3, 9, 3};
    const std::array golden{
        std::pair{clab::CodeLength{7, 0, 160}, "0c0000000600000012000000060000001200000006"},
        std::pair{clab::CodeLength{7, 67, 5}, "4c80000000c000000254"},
        std::pair{clab::CodeLength{7, 0, 165}, "8c000000070000000980000001c00000026000000070"},
        std::pair{clab::CodeLength{7, 67, 0}, "cc80000000c000000240"},
    };
    for (std::size_t i = 0; i < golden.size(); ++i) {
        const auto encoded = clab::encode_categorical(witness, clab::categorical_codes[i]);
        const auto& [length, hex] = golden[i];
        const auto expected = hex_bytes(hex);
        require(encoded.length == length && encoded.bytes == expected, "categorical golden bytes or bits");
        require(std::ranges::equal(clab::decode_categorical(expected, length.total_bits()), witness),
                "categorical golden decode");
    }

    const auto check = [](std::span<const clab::TypeId> sequence) {
        std::vector<clab::TypeId> relabeled;
        for (const auto label : sequence) {
            relabeled.push_back(label ^ 0xf10ff10fU);
        }
        for (const auto code : clab::categorical_codes) {
            const auto original = clab::encode_categorical(sequence, code);
            const auto changed = clab::encode_categorical(relabeled, code);
            require(original.length == changed.length, "categorical label-dependent cost");
            require(original.bytes.size() == (original.length.total_bits() + 7) / 8,
                    "categorical byte length");
            require(std::ranges::equal(clab::decode_categorical(original.bytes, original.length.total_bits()),
                                       sequence), "categorical round trip");
            require(clab::decode_categorical(changed.bytes, changed.length.total_bits()) == relabeled,
                    "categorical relabeled round trip");
        }
    };
    check({});
    check(std::array{clab::TypeId{0xffffffffU}});
    check(std::vector<clab::TypeId>(257, 7));
    check(witness);
    clab::Rng rng(145);
    for (std::size_t count = 0; count < 128; ++count) {
        std::vector<clab::TypeId> sample(count);
        for (auto& symbol : sample) {
            const auto random = static_cast<clab::TypeId>(rng.next());
            symbol = count % 2 == 0 ? random % 7 : random;
        }
        check(sample);
    }

    const std::array malformed{
        std::tuple{112ULL, "48c00000004000000080000000f0", clab::ErrorCode::invalid}, // index 3 of 3
        std::tuple{71ULL, "980000002c00000016", clab::ErrorCode::invalid}, // split equal runs
        std::tuple{8ULL, "db", clab::ErrorCode::invalid}, // period exceeds length
        std::tuple{5ULL, "10", clab::ErrorCode::incomplete}, // missing symbol
        std::tuple{4ULL, "20", clab::ErrorCode::invalid}, // extra data bit
        std::tuple{3ULL, "21", clab::ErrorCode::invalid}, // bad padding
    };
    for (const auto& [length, hex, code] : malformed) {
        const auto bytes = hex_bytes(hex);
        expect_error(code, [&] { (void)clab::decode_categorical(bytes, length); });
    }
    const std::array<std::uint8_t, 9> overflow_gamma{};
    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::decode_categorical(overflow_gamma, 72);
    });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::encode_categorical(witness, clab::CategoricalCode::raw, {4, 64});
    });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::encode_categorical(witness, clab::CategoricalCode::raw, {8, 1});
    });
    const auto encoded = clab::encode_categorical(witness, clab::CategoricalCode::periodic);
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::decode_categorical(encoded.bytes, encoded.length.total_bits(), {4, 64});
    });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::decode_categorical(encoded.bytes, encoded.length.total_bits(), {8, 1});
    });
}

void iid_code_fixtures() {
    const std::array<clab::TypeId, 3> witness{9, 3, 3};
    const auto small = clab::encode_categorical_iid(witness);
    require(small.length == clab::CodeLength{5, 71, 2} &&
            small.bytes == hex_bytes("22000000034000000138"), "IID small golden");
    const std::array<clab::TypeId, 3> relabeled{1, 0xffffffffU, 0xffffffffU};
    const auto renamed = clab::encode_categorical_iid(relabeled);
    require(renamed.length == small.length, "IID relabel-dependent length");
    require(std::ranges::equal(clab::decode_categorical_iid(renamed.bytes, renamed.length.total_bits()),
                               relabeled), "IID relabeled decode");

    std::vector<clab::TypeId> wide(80, 3);
    std::fill_n(wide.begin(), 40, 9);
    const auto encoded = clab::encode_categorical_iid(wide);
    require(encoded.length == clab::CodeLength{13, 89, 77} &&
            encoded.bytes == hex_bytes("028a00000003050000000120a2d87f47612bb27010e660"),
            "IID wide-rank golden");
    require(clab::decode_categorical_iid(encoded.bytes, encoded.length.total_bits()) == wide,
            "IID wide-rank decode");

    const auto check = [](std::span<const clab::TypeId> sequence) {
        const auto code = clab::encode_categorical_iid(sequence);
        require(std::ranges::equal(clab::decode_categorical_iid(code.bytes, code.length.total_bits()),
                                   sequence), "IID exact round trip");
    };
    check({});
    check(std::array{clab::TypeId{0xffffffffU}});
    check(std::vector<clab::TypeId>(clab::categorical_iid_symbol_limit, 7));
    std::vector<clab::TypeId> multigroup(600, 2);
    std::fill_n(multigroup.begin(), 200, 9);
    std::fill_n(multigroup.begin() + 200, 200, 7);
    check(multigroup);
    std::array<clab::TypeId, 6> arrangement{2, 2, 2, 7, 7, 13};
    std::size_t count = 0;
    do {
        const auto code = clab::encode_categorical_iid(arrangement);
        require(code.length.data_bits == 6, "IID permutation width");
        const auto bits = code.length.total_bits();
        const auto padding = (8 - bits % 8) % 8;
        // Read the six rank bits without using the decoder.
        const auto tail = (std::uint16_t{code.bytes[code.bytes.size() - 2]} << 8) |
                          code.bytes.back();
        require(((tail >> padding) & 63) == count, "IID lexicographic rank");
        require(std::ranges::equal(clab::decode_categorical_iid(code.bytes, bits), arrangement),
                "IID exhaustive decode");
        ++count;
    } while (std::ranges::next_permutation(arrangement).found);
    require(count == 60, "IID permutation count");

    clab::Rng rng(391);
    for (std::size_t n = 0; n < 128; ++n) {
        std::vector<clab::TypeId> symbols(n);
        for (auto& symbol : symbols) {
            symbol = static_cast<clab::TypeId>(rng.next() % (n % 13 + 1));
        }
        check(symbols);
    }

    const std::array malformed{
        std::tuple{78ULL, "2200000003400000013c", clab::ErrorCode::invalid}, // rank 3 of 3
        std::tuple{41ULL, "240000000d00", clab::ErrorCode::invalid}, // frequency total short
        std::tuple{76ULL, "22000000034000000070", clab::ErrorCode::invalid}, // duplicate label
        std::tuple{77ULL, "22000000034000000138", clab::ErrorCode::incomplete}, // truncated rank
        std::tuple{78ULL, "22000000034000000139", clab::ErrorCode::invalid}, // nonzero padding
        std::tuple{25ULL, "00080100", clab::ErrorCode::limit}, // 4097 symbols
    };
    for (const auto& [length, hex, error] : malformed) {
        expect_error(error, [&] { (void)clab::decode_categorical_iid(hex_bytes(hex), length); });
    }
    expect_error(clab::ErrorCode::limit, [] {
        (void)clab::encode_categorical_iid(std::vector<clab::TypeId>(4097));
    });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::encode_categorical_iid(witness, {2, 64}); });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::encode_categorical_iid(witness, {4, 1}); });
}

void categorical_comparison_fixtures() {
    using clab::CategoricalFamily;
    std::array<std::vector<clab::TypeId>, 5> fixtures;
    for (clab::TypeId i = 0; i < 12; ++i) {
        fixtures[0].push_back(i);
    }
    for (const char symbol : std::string_view("01101000110110101100100111010010")) {
        fixtures[1].push_back(static_cast<clab::TypeId>(symbol - '0'));
    }
    fixtures[2].assign(80, 1);
    fixtures[2].insert(fixtures[2].end(), 60, 2);
    fixtures[3] = {3, 9, 3, 9, 3};
    fixtures[4].assign(128, 3);
    for (const auto i : {2, 7, 15, 29, 32, 58, 70, 79, 83, 99, 104, 119}) {
        fixtures[4][i] = 9;
    }
    constexpr std::array<std::uint64_t, 5> best_bits{394, 113, 106, 75, 160};
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto result = clab::compare_categorical(fixtures[i]);
        require(result.winner && result.winner->family == clab::categorical_families[i],
                "categorical family selection");
        const auto& frame = result.winner->encoded;
        require(frame.length.total_bits() == best_bits[i] && frame.bytes[0] >> 5 == i,
                "selected categorical frame cost or tag");
        require(clab::decode_selected_categorical(frame.bytes, frame.length.total_bits()) == fixtures[i],
                "selected categorical round trip");
    }

    const auto tie = clab::compare_categorical({});
    require(tie.winner && tie.winner->family == CategoricalFamily::raw &&
            tie.winner->encoded.bytes == hex_bytes("10"), "first family wins empty tie");
    for (const auto& candidate : tie.candidates) {
        require(candidate.length && candidate.length->total_bits() == 4, "empty tie cost");
    }

    const auto cap = clab::compare_categorical(std::array<clab::TypeId, 2>{1, 1}, {2, 5});
    require(!cap.winner && cap.candidates[1].length && cap.candidates[1].length->total_bits() == 39 &&
            !cap.candidates[2].length && !cap.candidates[4].length,
            "shared selector must fit encoded byte budget");
    const auto partial = clab::compare_categorical(std::vector<clab::TypeId>(4097, 1));
    require(!partial.winner && partial.candidates[0].length && !partial.candidates[4].length,
            "no selected frame for incomplete comparison");

    for (const auto reserved : {"b0", "d0", "f0"}) {
        expect_error(clab::ErrorCode::invalid, [&] {
            (void)clab::decode_selected_categorical(hex_bytes(reserved), 4);
        });
    }
    expect_error(clab::ErrorCode::invalid, [] {
        (void)clab::decode_selected_categorical(hex_bytes("11"), 4);
    });
    expect_error(clab::ErrorCode::incomplete, [] {
        (void)clab::decode_selected_categorical(hex_bytes("00"), 2);
    });
}

void categorical_motif_fixtures() {
    const std::vector<clab::TypeId> short_sequence{1, 2, 1, 2};
    const auto short_result = clab::compare_categorical_motif(short_sequence);
    const auto golden = hex_bytes("a540000000200000004b");
    require(short_result.motif && !short_result.improves_baseline() &&
            short_result.motif->encoded.bytes == golden &&
            short_result.motif->encoded.length == clab::CodeLength{8, 67, 5},
            "categorical motif golden frame");
    require(clab::decode_categorical_motif(golden, 80) == short_sequence, "categorical motif golden decode");

    const std::vector<clab::TypeId> pattern{1, 2, 3, 4, 5, 6};
    std::vector<clab::TypeId> separated;
    std::vector<std::size_t> placements;
    for (clab::TypeId id = 100; id < 124; ++id) {
        placements.push_back(separated.size());
        separated.insert(separated.end(), pattern.begin(), pattern.end());
        separated.push_back(id);
    }
    const auto result = clab::compare_categorical_motif(separated);
    require(result.motif && result.improves_baseline() && result.motif->pattern == pattern &&
            result.motif->placements == placements && result.motif->placement_bits == 79 &&
            result.motif->encoded.length == clab::CodeLength{18, 197, 847},
            "categorical motif costs and greedy placements");
    require(clab::decode_categorical_motif(result.motif->encoded.bytes, 1062) == separated,
            "categorical motif residual round trip");

    const auto frozen = clab::FrozenCategoricalMotif::fit(separated);
    require(frozen && frozen->model_code().length == clab::CodeLength{0, 197, 0} &&
            !clab::FrozenCategoricalMotif::fit(short_sequence),
            "frozen motif admits strict training wins");
    const auto restored = clab::FrozenCategoricalMotif::from_bytes(frozen->model_code().bytes, 197);
    const std::vector<clab::TypeId> once{9, 1, 2, 3, 4, 5, 6, 8};
    const auto encoded_once = frozen->encode(once);
    require(encoded_once.placements == std::vector<std::size_t>{1} &&
            restored.decode(encoded_once.code.bytes, encoded_once.code.length.total_bits()) == once,
            "frozen motif single test occurrence");

    const std::vector<clab::TypeId> tied{9, 9, 1, 9, 1, 9};
    const auto tie = clab::compare_categorical_motif(tied);
    require(tie.motif && tie.motif->pattern == std::vector<clab::TypeId>{9, 1},
            "categorical motif first offset tie break");
    const auto overlap = clab::compare_categorical_motif(std::vector<clab::TypeId>(13, 7));
    require(overlap.motif && overlap.motif->placements == std::vector<std::size_t>{0, 2, 4, 6, 8, 10},
            "categorical motif disjoint placements for overlapping matches");
    require(!clab::compare_categorical_motif(std::array<clab::TypeId, 4>{1, 2, 3, 4}).motif,
            "categorical motif absence");

    for (const auto& [hex, size] : std::array<std::pair<std::string_view, std::uint64_t>, 6>{{
             {"8540000000200000004b", 80},   // Wrong tag.
             {"a580000000ac", 46},           // Width one.
             {"a540000000200000004f80", 81}, // Too many placements.
             {"a540000000200000004a80", 82}, // Second placement overruns region.
             {"a540000000200000004b01", 81}, // Nonzero padding.
             {"a540000000200000004b00", 81}, // Trailing bit.
         }}) {
        expect_error(clab::ErrorCode::invalid, [&] {
            (void)clab::decode_categorical_motif(hex_bytes(hex), size);
        });
    }
    expect_error(clab::ErrorCode::incomplete, [&] {
        (void)clab::decode_categorical_motif(hex_bytes("a540000000200000004a"), 79);
    });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::decode_categorical_motif(golden, 80, {3, 10}); });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::decode_categorical_motif(golden, 80, {4, 9}); });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::compare_categorical_motif(short_sequence, {4, 4}); });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::compare_categorical_motif(std::vector<clab::TypeId>(4097, 7));
    });
}

void categorical_dictionary_fixtures() {
    const std::vector<clab::TypeId> input{1, 2, 9, 3, 4, 8, 1, 2, 7, 3, 4, 6};
    const auto comparison = clab::compare_categorical_motif_dictionary(input);
    require(comparison.dictionary && comparison.selects_dictionary(), "two motif fixture selection");
    const auto& pair = *comparison.dictionary;
    const auto golden = hex_bytes("c3500000000800000012000000030000000452a21400000024000000200000001c00000018");
    require(pair.entries[0].pattern == std::vector<clab::TypeId>{1, 2} &&
            pair.entries[0].placements == std::vector<std::size_t>{0, 6} &&
            pair.entries[1].pattern == std::vector<clab::TypeId>{3, 4} &&
            pair.entries[1].placements == std::vector<std::size_t>{3, 9},
            "two motif interleaved original-coordinate placement");
    require(pair.encoded.bytes == golden && pair.encoded.length == clab::CodeLength{10, 134, 150} &&
            pair.placement_bits == 22, "two motif golden frame");
    require(clab::decode_categorical_motif_dictionary(golden, 294) == input, "two motif golden decode");
    for (const auto& [hex, bits] : std::array<std::pair<std::string_view, std::uint64_t>, 4>{{
             {"c3500000000800000012000000010000000252a214", 166}, // Duplicate definitions.
             {"c3500000000800000012000000030000000452a940", 162}, // Across-entry overlap.
             {"c3500000000800000012000000030000000452a21c", 166}, // Beyond the snapshot.
             {"c3500000000800000012000000030000000452c8", 159},   // One occurrence.
         }}) {
        expect_error(clab::ErrorCode::invalid, [&] {
            (void)clab::decode_categorical_motif_dictionary(hex_bytes(hex), bits);
        });
    }
    expect_error(clab::ErrorCode::incomplete, [&] {
        (void)clab::decode_categorical_motif_dictionary(golden, 293);
    });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)clab::decode_categorical_motif_dictionary(golden, 294, {11, 100});
    });

    const auto fitted = clab::FrozenCategoricalMotifDictionary::fit(input);
    require(fitted && !clab::FrozenCategoricalMotifDictionary::fit(std::array<clab::TypeId, 4>{1, 2, 1, 2}),
            "frozen dictionary admission");
    const auto model = hex_bytes("4000000020000000480000000c00000010");
    require(fitted->model_code().bytes == model && fitted->model_code().length == clab::CodeLength{0, 134, 0} &&
            fitted->first_entry().model_code().length.total_bits() == 67, "frozen dictionary golden model");
    const auto restored = clab::FrozenCategoricalMotifDictionary::from_bytes(model, 134);
    const std::vector<clab::TypeId> test{9, 1, 2, 3, 4, 8};
    const auto encoded = fitted->encode(test);
    const auto test_golden = hex_bytes("3a48800000012000000100");
    require(encoded.encoded.bytes == test_golden && encoded.encoded.length == clab::CodeLength{5, 0, 78} &&
            encoded.placement_bits == 14 && encoded.entries[0].placements == std::vector<std::size_t>{1} &&
            encoded.entries[1].placements == std::vector<std::size_t>{3} && restored.decode(test_golden, 83) == test,
            "frozen dictionary exact test frame");
    const auto empty = restored.encode({});
    require(empty.encoded.bytes == hex_bytes("e0") && empty.encoded.length.total_bits() == 3 &&
            restored.decode(empty.encoded.bytes, 3).empty(), "empty frozen dictionary frame");

    const auto overlapping = clab::FrozenCategoricalMotifDictionary::from_bytes(
        hex_bytes("400000002000000048000000080000000c"), 134);
    const std::vector<clab::TypeId> contested{1, 2, 3, 2, 3};
    const auto contested_code = overlapping.encode(contested);
    require(contested_code.entries[0].placements == std::vector<std::size_t>{0} &&
            contested_code.entries[1].placements == std::vector<std::size_t>{3} &&
            overlapping.decode(contested_code.encoded.bytes, contested_code.encoded.length.total_bits()) == contested,
            "first frozen entry keeps priority at overlapping test placements");

    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::FrozenCategoricalMotifDictionary::from_bytes(
            hex_bytes("4000000020000000480000000400000008"), 134);
    });
    expect_error(clab::ErrorCode::invalid, [&] { (void)restored.decode(hex_bytes("3a4980"), 17); });
    expect_error(clab::ErrorCode::invalid, [&] { (void)restored.decode(hex_bytes("3a48c0"), 19); });
    expect_error(clab::ErrorCode::incomplete, [&] { (void)restored.decode(test_golden, 82); });
    expect_error(clab::ErrorCode::limit, [&] { (void)restored.decode(test_golden, 83, {5, 100}); });
}

void frozen_motif_fixtures() {
    const auto model = hex_bytes("400000002000000040");
    const auto frozen = clab::FrozenCategoricalMotif::from_bytes(model, 67);
    require(frozen.model_code().length == clab::CodeLength{0, 67, 0} && frozen.model_code().bytes == model,
            "frozen motif golden model");
    const std::vector<clab::TypeId> test{9, 1, 2, 1, 2, 8};
    const auto encoded = frozen.encode(test);
    const auto golden = hex_bytes("3b500000009000000080");
    require(encoded.code.bytes == golden && encoded.code.length == clab::CodeLength{5, 0, 71} &&
            encoded.placements == std::vector<std::size_t>{1, 3} && encoded.placement_bits == 7,
            "frozen motif golden test frame");
    require(frozen.decode(golden, 76) == test, "frozen motif exact reconstruction");

    const auto empty = frozen.encode({});
    require(empty.code.bytes == hex_bytes("c0") && empty.code.length.total_bits() == 2 &&
            frozen.decode(empty.code.bytes, 2).empty(), "frozen motif empty frame");
    const std::vector<clab::TypeId> absent{2, 1, 0xffffffffu};
    const auto encoded_absent = frozen.encode(absent);
    require(encoded_absent.placements.empty() && encoded_absent.placement_bits == 1 &&
            frozen.decode(encoded_absent.code.bytes, encoded_absent.code.length.total_bits()) == absent,
            "frozen motif zero test occurrences preserve residual");

    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::FrozenCategoricalMotif::from_bytes(hex_bytes("80"), 1);
    });
    expect_error(clab::ErrorCode::invalid, [&] {
        (void)clab::FrozenCategoricalMotif::from_bytes(model, 68);
    });
    expect_error(clab::ErrorCode::invalid, [&] { (void)frozen.decode(hex_bytes("3940"), 10); });
    expect_error(clab::ErrorCode::invalid, [&] { (void)frozen.decode(hex_bytes("3b340000002400000020"), 78); });
    expect_error(clab::ErrorCode::incomplete, [&] { (void)frozen.decode(golden, 75); });
    expect_error(clab::ErrorCode::limit, [&] { (void)frozen.decode(golden, 76, {5, 10}); });
    expect_error(clab::ErrorCode::limit, [&] { (void)frozen.encode(test, {6, 9}); });
    expect_error(clab::ErrorCode::limit, [&] { (void)clab::FrozenCategoricalMotif::from_bytes(model, 67, {6, 8}); });

    std::vector<clab::TypeId> training;
    for (clab::TypeId i = 100; i < 116; i += 4) {
        const std::array<clab::TypeId, 10> frame{4, 5, 6, i, i + 1, i + 2, i + 3, 1, 2, 3};
        training.insert(training.end(), frame.begin(), frame.end());
    }
    const auto flat = clab::compare_categorical_motif(training);
    const auto framed = clab::FrozenCategoricalMotif::fit_frames(training, 10);
    const std::array<clab::TypeId, 6> crossing{1, 2, 3, 4, 5, 6};
    require(flat.motif && std::ranges::equal(flat.motif->pattern, crossing),
            "flattened fixture should nominate the cross-frame pattern");
    require(framed && framed->model_code().length.total_bits() == 99 &&
            framed->encode(crossing).placements == std::vector<std::size_t>{3},
            "frame training must learn a pattern wholly within a snapshot");
    expect_error(clab::ErrorCode::invalid, [&] { (void)clab::FrozenCategoricalMotif::fit_frames(training, 7); });
}

void categorical_heldout_fixtures() {
    const std::array<clab::TypeId, 4> train{9, 3, 9, 9};
    const auto fitted = clab::FrozenCategoricalModel::fit(train);
    const auto& model = fitted.model_code();
    require(model.length == clab::CodeLength{0, 71, 0} &&
            model.bytes == hex_bytes("400000012c0000000e"), "frozen categorical model bytes");
    const auto restored = clab::FrozenCategoricalModel::from_bytes(model.bytes, model.length.total_bits());
    const std::array<clab::TypeId, 4> test{9, 3, 100, 9};
    const auto heldout = fitted.encode(test);
    require(heldout.unseen_symbols == 1 && heldout.code.length == clab::CodeLength{5, 0, 38} &&
            heldout.code.bytes == hex_bytes("2c4000001920"), "frozen categorical test bytes");
    require(std::ranges::equal(restored.decode(heldout.code.bytes, heldout.code.length.total_bits()), test),
            "standalone frozen model decode");
    const auto shifted = restored.encode(std::array<clab::TypeId, 4>{100, 100, 100, 100});
    require(shifted.unseen_symbols == 4 && shifted.code.length.data_bits == 136,
            "unseen labels must pay an escape every time");
    require(fitted.encode(test).code.bytes == heldout.code.bytes &&
            restored.model_code().bytes == model.bytes, "scoring updated the fitted model");

    const std::array<clab::TypeId, 3> tied{90, 2, 8}, renamed_train{1, 80, 9};
    const std::array<clab::TypeId, 5> probe{2, 90, 2, 8, 100}, renamed_test{80, 1, 80, 9, 101};
    const auto renamed = clab::FrozenCategoricalModel::fit(renamed_train);
    const auto a = clab::FrozenCategoricalModel::fit(tied).encode(probe);
    const auto b = renamed.encode(renamed_test);
    require(a.code.length == b.code.length && b.unseen_symbols == 1 &&
            std::ranges::equal(renamed.decode(b.code.bytes, b.code.length.total_bits()), renamed_test),
            "relabeling changed frozen tie order or decode");

    const auto empty = fitted.encode({});
    require(empty.code.bytes == hex_bytes("80") && empty.code.length.total_bits() == 1 &&
            fitted.decode(empty.code.bytes, 1).empty(), "frozen categorical empty test");

    expect_error(clab::ErrorCode::invalid, [] {
        (void)clab::FrozenCategoricalModel::fit({});
    });
    expect_error(clab::ErrorCode::limit, [] {
        (void)clab::FrozenCategoricalModel::fit(std::vector<clab::TypeId>(4097, 3));
    });
    expect_error(clab::ErrorCode::invalid, [] {
        (void)clab::FrozenCategoricalModel::from_bytes(hex_bytes("400000012c00000026"), 71);
    });
    expect_error(clab::ErrorCode::invalid, [&] {
        (void)restored.decode(hex_bytes("4800000048"), 37);
    });
    expect_error(clab::ErrorCode::incomplete, [&] {
        (void)restored.decode(hex_bytes("2c40000019"), 40);
    });
    expect_error(clab::ErrorCode::limit, [&] {
        (void)restored.decode(heldout.code.bytes, heldout.code.length.total_bits(), {3, 64});
    });

    clab::Rng rng(507);
    for (std::size_t n = 1; n <= 64; ++n) {
        std::vector<clab::TypeId> training(n), evaluation(n + 3);
        for (auto& symbol : training) {
            symbol = static_cast<clab::TypeId>(rng.next() % 17);
        }
        for (auto& symbol : evaluation) {
            symbol = static_cast<clab::TypeId>(rng.next() % 41);
        }
        const auto fit = clab::FrozenCategoricalModel::fit(training);
        const auto& bytes = fit.model_code();
        const auto decode = clab::FrozenCategoricalModel::from_bytes(bytes.bytes, bytes.length.total_bits());
        const auto trial = fit.encode(evaluation);
        require(decode.decode(trial.code.bytes, trial.code.length.total_bits()) == evaluation,
                "frozen categorical seeded round trip");
    }
}

void temporal_categorical_fixtures(const std::filesystem::path& directory) {
    using Sites = std::array<clab::TypeId, 2>;
    const auto make_trace = [&](std::string_view name, Sites test_start) {
        const auto path = directory / name;
        clab::TraceWriter writer(path, {"temporal", "fixture", "step", true, true});
        const std::array frames{Sites{9, 3}, Sites{9, 9}, Sites{700, 701}, test_start, Sites{100, 9}};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            writer.append({{i, double(i), i}, clab::Snapshot(frames[i])});
        }
        writer.finish();
        return path;
    };

    const auto baseline = make_trace("baseline-temporal.cltr", {9, 3});
    const auto changed = make_trace("changed-temporal.cltr", {100, 100});
    const clab::TemporalCategoricalSplit split{2, 1, 2, 2};
    clab::TraceReader a(baseline), b(changed);
    const auto before = clab::score_categorical_temporal(a, split);
    const auto after = clab::score_categorical_temporal(b, split);
    require(before.status == clab::MetricStatus::valid && after.status == clab::MetricStatus::valid &&
            before.costs && after.costs, "temporal fixture status");
    require(before.costs->model.bytes == hex_bytes("400000012c0000000e") &&
            before.costs->model.length.total_bits() == 71 &&
            after.costs->model.bytes == before.costs->model.bytes, "test data changed the fitted model");
    require(before.costs->test_header_bits == 6 && before.costs->test_data_bits == 38 &&
            after.costs->test_data_bits == 103 && before.costs->raw_bits == 134 &&
            before.costs->unseen_symbols == 1 && after.costs->unseen_symbols == 3,
            "temporal frame accounting or frozen test cost");
    require(before.train_end == clab::Stamp{1, 1, 1} &&
            before.test_start == clab::Stamp{3, 3, 3} &&
            before.test_end == clab::Stamp{4, 4, 4}, "temporal boundaries");

    clab::TraceReader bounded(baseline);
    const auto limit = clab::score_categorical_temporal(bounded, split, {4, 8});
    require(limit.status == clab::MetricStatus::budget_exceeded && !limit.costs &&
            limit.test_seen == 2 && bounded.status() == clab::TraceStatus::complete,
            "temporal budget must drain the trace");

    clab::TraceReader no_motif(baseline);
    const auto motif = clab::score_categorical_motif_temporal(no_motif, split);
    require(motif.status == clab::MetricStatus::valid && motif.reason == "no_training_motif" &&
            motif.costs && !motif.costs->motif &&
            motif.costs->baseline_model.bytes == before.costs->model.bytes &&
            motif.costs->baseline_test_bits == 44 && no_motif.status() == clab::TraceStatus::complete,
            "temporal run without an admitted motif must report the frozen comparator");
}

void temporal_permutation_budget(const std::filesystem::path& directory) {
    const auto path = directory / "permutation-budget.cltr";
    std::array<clab::TypeId, 64> labels{};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        labels[i] = static_cast<clab::TypeId>(i / 4);
    }
    clab::TraceWriter writer(path, {"permutation", "fixture", "step", true, true});
    for (std::size_t i = 0; i < 3; ++i) {
        writer.append({{i, double(i), i}, clab::Snapshot(labels)});
    }
    writer.finish();
    // Shuffling can exceed the byte budget for run-length coding when runs are broken.
    for (auto score : {clab::score_categorical_motif_permutations, clab::score_categorical_motif_site_permutations}) {
        clab::TraceReader trace(path);
        const auto result = score(trace, {1, 1, 1, 64}, 19, 2, {64, 260});
        const auto failed = std::ranges::count_if(result.draws, [](const auto& draw) {
            return draw.status == clab::MetricStatus::budget_exceeded && !draw.charged_gain_bits;
        });
        require(result.observed.costs && !result.at_least_observed && result.draws.size() == 19 &&
                failed > 0 && failed < 19 && trace.status() == clab::TraceStatus::complete,
                "a failed permutation must remain scheduled and suppress the rank");
    }
}

#ifdef CLAB_TEST_SUBSTRATES
void replay() {
    expect_error(clab::ErrorCode::limit, [] { clab::BinaryCa world(9, 0, 71); });
    for (std::uint64_t seed = 0; seed < 10; ++seed) {
        for (clab::WorkCount checkpoint_at : {0, 5, 31}) {
            clab::BinaryCa original(9, seed, 72);
            while (original.advance(checkpoint_at)) {}
            const auto saved = original.checkpoint();
            (void)original.observe();
            require(original.checkpoint() == saved, "observing changed state or RNG");
            require(!original.advance(checkpoint_at) && original.checkpoint() == saved,
                    "cap changed state");
            clab::BinaryCa replayed(9, seed + 1, 72);
            replayed.restore(saved);
            const auto final = checkpoint_at + 12;
            while (original.advance(final)) {
                require(replayed.advance(final), "replay reached cap early");
                (void)original.observe();
                require(original.checkpoint() == replayed.checkpoint(), "replay mismatch");
                require(original.ledger() == replayed.ledger(), "ledger mismatch");
            }
        }
    }
}
#endif
}

int main() {
    try {
        TempDirectory temp;
        round_trips(temp.path);
        relational_round_trip(temp.path);
        temporal_round_trip(temp.path);
        provenance_round_trip(temp.path);
        attribute_round_trip(temp.path);
        mock_graph_replay(temp.path);
        dependency_resolution();
        selected_metrics(temp.path);
        categorical_code_fixtures();
        iid_code_fixtures();
        categorical_comparison_fixtures();
        categorical_motif_fixtures();
        categorical_dictionary_fixtures();
        frozen_motif_fixtures();
        categorical_heldout_fixtures();
        temporal_categorical_fixtures(temp.path);
        temporal_permutation_budget(temp.path);
#ifdef CLAB_TEST_SUBSTRATES
        replay();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
