#include <clab/evaluator.hpp>
#include <clab/trace.hpp>
#ifdef CLAB_TEST_SUBSTRATES
#include <clab/substrate.hpp>
#endif

#include <algorithm>
#include <stdlib.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>

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
#ifdef CLAB_TEST_SUBSTRATES
        replay();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
