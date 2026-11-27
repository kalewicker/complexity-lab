#include <clab/histogram_cache.hpp>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>
#include <utility>

namespace clab {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t slot_count = 64;
constexpr std::size_t entry_limit = 256 * 1024;
constexpr std::size_t key_header_bytes = 13;
constexpr std::size_t result_header_bytes = 4;
constexpr std::size_t bin_bytes = 12;
constexpr std::size_t digest_bytes = 8;

template<class UInt>
void append_le(Bytes& bytes, UInt value) {
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

template<class UInt>
UInt read_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

// FNV-1a is a slot selector and corruption check. Hits also compare the complete key.
std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

std::optional<Bytes> make_key(const Metadata& meta, std::span<const TypeId> symbols) {
    constexpr auto available = entry_limit - key_header_bytes - result_header_bytes - digest_bytes;
    if (meta.encoding.size() > available ||
        symbols.size() > (available - meta.encoding.size()) / sizeof(TypeId)) {
        return std::nullopt;
    }
    Bytes key{'C', 'L', 'H', '1', static_cast<std::uint8_t>(meta.exact)};
    key.reserve(key_header_bytes + meta.encoding.size() + symbols.size_bytes());
    append_le(key, static_cast<std::uint32_t>(meta.encoding.size()));
    append_le(key, static_cast<std::uint32_t>(symbols.size()));
    key.insert(key.end(), meta.encoding.begin(), meta.encoding.end());
    for (const auto symbol : symbols) {
        append_le(key, symbol);
    }
    return key;
}

std::optional<SymbolHistogram> read_slot(const std::filesystem::path& slot, const Bytes& key,
                                         std::size_t symbol_count) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(slot, error)) {
        return std::nullopt;
    }
    std::ifstream input(slot, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    const auto minimum = key.size() + result_header_bytes + digest_bytes;
    if (size < static_cast<std::streamoff>(minimum) || size > static_cast<std::streamoff>(entry_limit)) {
        return std::nullopt;
    }
    Bytes bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() || input.bad() ||
        !std::ranges::equal(key, std::span(bytes).first(key.size()))) {
        return std::nullopt;
    }
    const auto body = std::span(bytes).first(bytes.size() - digest_bytes);
    const auto bins = read_le<std::uint32_t>(body, key.size());
    if (bins > symbol_count || bins != (bytes.size() - minimum) / bin_bytes ||
        bytes.size() != minimum + bins * bin_bytes ||
        checksum(body) != read_le<std::uint64_t>(bytes, body.size())) {
        return std::nullopt;
    }
    SymbolHistogram result;
    std::uint64_t total = 0;
    for (std::size_t offset = key.size() + result_header_bytes; offset < body.size(); offset += bin_bytes) {
        const auto code = read_le<TypeId>(body, offset);
        const auto count = read_le<std::uint64_t>(body, offset + sizeof(TypeId));
        if (count == 0 || count > symbol_count - total ||
            (!result.empty() && code <= result.rbegin()->first)) {
            return std::nullopt;
        }
        total += count;
        result.emplace_hint(result.end(), code, count);
    }
    if (total != symbol_count) {
        return std::nullopt;
    }
    return result;
}

bool write_slot(const std::filesystem::path& directory, const std::filesystem::path& slot,
                std::span<const std::uint8_t> bytes) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }
    auto staged = (directory / ".write-XXXXXX").string();
    const int fd = ::mkstemp(staged.data());
    if (fd < 0) {
        return false;
    }
    bool written = true;
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            written = false;
            break;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    if (::close(fd) != 0) {
        written = false;
    }
    if (written) {
        std::filesystem::rename(staged, slot, error);
        if (!error) {
            return true;
        }
    }
    std::filesystem::remove(staged, error);
    return false;
}
}

SymbolHistogram count_symbols(std::span<const TypeId> symbols) {
    SymbolHistogram histogram;
    for (const auto symbol : symbols) {
        ++histogram[symbol];
    }
    return histogram;
}

HistogramCache::HistogramCache(std::filesystem::path workspace)
    : directory_(std::move(workspace) / "symbol-histogram-v1") {}

SymbolHistogram HistogramCache::get(const Metadata& meta, std::span<const TypeId> symbols) {
    auto key = make_key(meta, symbols);
    if (!key) {
        ++stats_.skipped;
        return count_symbols(symbols);
    }
    const auto slot = directory_ / (std::to_string(checksum(*key) % slot_count) + ".bin");
    if (auto cached = read_slot(slot, *key, symbols.size())) {
        ++stats_.hits;
        return std::move(*cached);
    }

    ++stats_.misses;
    auto histogram = count_symbols(symbols);
    const auto overhead = key->size() + result_header_bytes + digest_bytes;
    if (histogram.size() > (entry_limit - overhead) / bin_bytes) {
        ++stats_.skipped;
        return histogram;
    }

    key->reserve(overhead + histogram.size() * bin_bytes);
    append_le(*key, static_cast<std::uint32_t>(histogram.size()));
    for (const auto& [symbol, count] : histogram) {
        append_le(*key, symbol);
        append_le(*key, count);
    }
    append_le(*key, checksum(*key));
    if (write_slot(directory_, slot, *key)) {
        ++stats_.stores;
    } else {
        ++stats_.write_failures;
    }
    return histogram;
}
}
