#include <clab/evaluator.hpp>
#include <clab/trace.hpp>
#ifdef CLAB_TEST_SUBSTRATES
#include <clab/substrate.hpp>
#endif

#include <algorithm>
#include <array>
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
#ifdef CLAB_TEST_SUBSTRATES
        replay();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
