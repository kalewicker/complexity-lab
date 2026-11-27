#pragma once

#include <clab/observation.hpp>

#include <filesystem>
#include <map>

namespace clab {
using SymbolHistogram = std::map<TypeId, std::uint64_t>;

[[nodiscard]] SymbolHistogram count_symbols(std::span<const TypeId> symbols);

struct HistogramCacheStats {
    std::uint64_t hits{}, misses{}, stores{}, skipped{}, write_failures{};
};

// One cache instance belongs to one evaluator. Separate processes may share its directory.
class HistogramCache {
public:
    explicit HistogramCache(std::filesystem::path workspace);
    [[nodiscard]] SymbolHistogram get(const Metadata& metadata, std::span<const TypeId> symbols);
    [[nodiscard]] HistogramCacheStats stats() const noexcept { return stats_; }

private:
    std::filesystem::path directory_;
    HistogramCacheStats stats_;
};
}
