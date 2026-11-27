#include <clab/categorical_motif.hpp>

#include "categorical_bits.hpp"

#include <algorithm>
#include <bit>
#include <map>
#include <tuple>
#include <utility>

namespace clab {
namespace {
using categorical_detail::BitReader;
using categorical_detail::BitWriter;
using categorical_detail::read_symbols;
using categorical_detail::symbol_bits;
using categorical_detail::write_symbols;

constexpr std::uint8_t motif_tag = 5;
constexpr std::size_t min_width = 2, max_width = categorical_motif_max_width;

void check_region(std::uint64_t count, CodeLimits limits) {
    if (count > std::min(limits.symbols, categorical_iid_symbol_limit)) {
        throw Error(ErrorCode::limit, "categorical motif region cap exceeded");
    }
}

std::uint64_t gamma_bits(std::uint64_t positive) {
    return 2 * std::bit_width(positive) - 1;
}

struct Occurrences {
    std::size_t first{}, next{}, count{};
    std::uint64_t gap_bits{};
};

std::map<std::vector<TypeId>, Occurrences> scan_motifs(std::span<const TypeId> symbols) {
    std::map<std::vector<TypeId>, Occurrences> occurrences;
    for (std::size_t start = 0; start + min_width <= symbols.size(); ++start) {
        for (std::size_t width = min_width; width <= max_width && start + width <= symbols.size(); ++width) {
            const auto match = symbols.subspan(start, width);
            auto& slot = occurrences.try_emplace(std::vector<TypeId>(match.begin(), match.end()),
                                                  Occurrences{start}).first->second;
            if (start >= slot.next) {
                ++slot.count;
                slot.gap_bits += gamma_bits(start - slot.next + 1);
                slot.next = start + width;
            }
        }
    }
    return occurrences;
}

std::vector<std::size_t> find_placements(std::span<const TypeId> symbols,
                                         std::span<const TypeId> pattern) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + pattern.size() <= symbols.size();) {
        if (std::ranges::equal(symbols.subspan(i, pattern.size()), pattern)) {
            starts.push_back(i);
            i += pattern.size();
        } else {
            ++i;
        }
    }
    return starts;
}

void write_pattern(BitWriter& writer, std::span<const TypeId> pattern) {
    writer.gamma(pattern.size());
    write_symbols(writer, pattern);
}

std::vector<TypeId> read_pattern(BitReader& reader, std::uint64_t count = max_width) {
    const auto width = reader.gamma();
    if (width < min_width || width > max_width || width > count) {
        throw Error(ErrorCode::invalid, "invalid categorical motif width");
    }
    return read_symbols(reader, static_cast<std::size_t>(width));
}

std::uint64_t write_gaps_and_residual(BitWriter& writer, std::span<const TypeId> symbols,
                                      std::span<const std::size_t> starts, std::size_t width) {
    std::size_t cursor = 0;
    for (auto start : starts) {
        writer.gamma(start - cursor + 1);
        cursor = start + width;
    }
    const auto placement_end = writer.position();
    cursor = 0;
    for (auto start : starts) {
        write_symbols(writer, symbols.subspan(cursor, start - cursor));
        cursor = start + width;
    }
    write_symbols(writer, symbols.subspan(cursor));
    return placement_end;
}

std::vector<TypeId> read_gaps_and_residual(BitReader& reader, std::span<const TypeId> pattern,
                                           std::uint64_t count, std::uint64_t placements) {
    const auto width = pattern.size();
    if (placements > count / width) {
        throw Error(ErrorCode::invalid, "invalid categorical motif placement count");
    }
    std::vector<std::size_t> gaps;
    std::uint64_t cursor = 0;
    for (std::uint64_t i = 0; i < placements; ++i) {
        const auto gap = reader.gamma() - 1;
        if (width > count - cursor || gap > count - cursor - width) {
            throw Error(ErrorCode::invalid, "categorical motif placement exceeds region");
        }
        gaps.push_back(static_cast<std::size_t>(gap));
        cursor += gap + width;
    }

    const auto residual = read_symbols(reader, static_cast<std::size_t>(count - placements * width));
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing categorical motif bits");
    }
    std::vector<TypeId> result;
    result.reserve(static_cast<std::size_t>(count));
    auto rest = std::span(residual);
    for (auto gap : gaps) {
        result.insert(result.end(), rest.begin(), rest.begin() + gap);
        result.insert(result.end(), pattern.begin(), pattern.end());
        rest = rest.subspan(gap);
    }
    result.insert(result.end(), rest.begin(), rest.end());
    return result;
}

CategoricalMotif encode_motif(std::span<const TypeId> symbols, std::span<const TypeId> pattern,
                             CodeLimits limits) {
    auto starts = find_placements(symbols, pattern);
    BitWriter writer(limits.encoded_bytes);
    writer.put(motif_tag, categorical_family_bits);
    writer.gamma(symbols.size() + 1);
    CodeLength length{writer.position()};
    write_pattern(writer, pattern);
    length.model_bits = writer.position() - length.header_bits;
    writer.gamma(starts.size());
    const auto placement_end = write_gaps_and_residual(writer, symbols, starts, pattern.size());
    length.data_bits = writer.position() - length.header_bits - length.model_bits;
    return {{writer.take_bytes(), length}, {pattern.begin(), pattern.end()}, std::move(starts),
            placement_end - length.header_bits - length.model_bits};
}
}

CategoricalMotifComparison compare_categorical_motif(std::span<const TypeId> symbols, CodeLimits limits) {
    check_region(symbols.size(), limits);
    auto baseline = compare_categorical(symbols, limits);
    if (!baseline.winner) {
        throw Error(ErrorCode::limit, "incomplete categorical baseline set");
    }
    CategoricalMotifComparison comparison{std::move(*baseline.winner), std::nullopt};

    const auto occurrences = scan_motifs(symbols);

    using Order = std::tuple<std::uint64_t, std::size_t, std::size_t>;
    std::optional<Order> best_cost;
    const std::vector<TypeId>* best_pattern = nullptr;
    for (const auto& [pattern, slot] : occurrences) {
        if (slot.count < 2) {
            continue;
        }
        const auto width = pattern.size();
        const auto bits = categorical_family_bits + gamma_bits(symbols.size() + 1)
                          + gamma_bits(width) + width * symbol_bits + gamma_bits(slot.count)
                          + slot.gap_bits + (symbols.size() - width * slot.count) * symbol_bits;
        const Order order{bits, slot.first, width};
        if (!best_cost || order < *best_cost) {
            best_cost = order;
            best_pattern = &pattern;
        }
    }

    if (best_pattern) {
        auto candidate = encode_motif(symbols, *best_pattern, limits);
        if (candidate.encoded.length.total_bits() != std::get<0>(*best_cost)) {
            throw std::logic_error("categorical motif cost disagrees with encoded bytes");
        }
        comparison.motif = std::move(candidate);
    }
    return comparison;
}

std::vector<TypeId> decode_categorical_motif(std::span<const std::uint8_t> bytes,
                                            std::uint64_t bit_count, CodeLimits limits) {
    BitReader reader(bytes, bit_count, limits.encoded_bytes);
    if (reader.get(categorical_family_bits) != motif_tag) {
        throw Error(ErrorCode::invalid, "invalid categorical motif tag");
    }
    const auto count = reader.gamma() - 1;
    check_region(count, limits);
    const auto pattern = read_pattern(reader, count);
    const auto placements = reader.gamma();
    if (placements < 2) {
        throw Error(ErrorCode::invalid, "invalid categorical motif placement count");
    }
    return read_gaps_and_residual(reader, pattern, count, placements);
}

FrozenCategoricalMotif::FrozenCategoricalMotif(std::span<const TypeId> pattern, CodeLimits limits)
    : width_(pattern.size()) {
    std::ranges::copy(pattern, pattern_.begin());
    BitWriter writer(limits.encoded_bytes);
    write_pattern(writer, pattern);
    const CodeLength length{0, writer.position(), 0};
    model_ = {writer.take_bytes(), length};
}

std::optional<FrozenCategoricalMotif> FrozenCategoricalMotif::fit(std::span<const TypeId> training,
                                                                 CodeLimits limits) {
    auto comparison = compare_categorical_motif(training, limits);
    if (!comparison.improves_baseline()) {
        return std::nullopt;
    }
    return FrozenCategoricalMotif(comparison.motif->pattern, limits);
}

std::optional<FrozenCategoricalMotif>
FrozenCategoricalMotif::fit_frames(std::span<const TypeId> training, std::size_t sites, CodeLimits limits) {
    if (!sites || training.empty() || training.size() % sites) {
        throw Error(ErrorCode::invalid, "frozen motif training requires whole nonempty frames");
    }
    check_region(training.size(), limits);
    struct TrainingMatches {
        std::size_t first{}, count{};
        std::uint64_t saved{};
    };
    std::map<std::vector<TypeId>, TrainingMatches> matches;
    const auto empty_frame_bits = gamma_bits(sites + 1) + 1 + sites * symbol_bits;
    std::uint64_t baseline_bits = 0;
    for (std::size_t start = 0; start < training.size(); start += sites) {
        const auto frame = training.subspan(start, sites);
        const auto baseline = compare_categorical(frame, limits);
        if (!baseline.winner) {
            throw Error(ErrorCode::limit, "incomplete categorical training baseline set");
        }
        baseline_bits += baseline.winner->encoded.length.total_bits();
        for (const auto& [candidate, hit] : scan_motifs(frame)) {
            const auto width = candidate.size();
            auto& total = matches.try_emplace(candidate, TrainingMatches{start + hit.first}).first->second;
            total.count += hit.count;
            // A zero-match frame already pays one bit for its placement count.
            const auto added_placement_bits = gamma_bits(hit.count + 1) - 1 + hit.gap_bits;
            total.saved += hit.count * width * symbol_bits - added_placement_bits;
        }
    }

    const auto empty_bits = empty_frame_bits * (training.size() / sites);
    auto best = std::tuple{baseline_bits, std::size_t{0}, std::size_t{0}};
    const std::vector<TypeId>* pattern = nullptr;
    for (const auto& [candidate, hit] : matches) {
        if (hit.count < 2) {
            continue;
        }
        const auto width = candidate.size();
        const auto bits = empty_bits - hit.saved + gamma_bits(width) + width * symbol_bits;
        const auto order = std::tuple{bits, hit.first, width};
        if (order < best) {
            best = order;
            pattern = &candidate;
        }
    }
    if (!pattern) {
        return std::nullopt;
    }
    FrozenCategoricalMotif fitted(*pattern, limits);
    auto encoded_bits = fitted.model_code().length.total_bits();
    for (std::size_t start = 0; start < training.size(); start += sites) {
        encoded_bits += fitted.encode(training.subspan(start, sites), limits).code.length.total_bits();
    }
    if (encoded_bits != std::get<0>(best)) {
        throw std::logic_error("frozen motif training cost disagrees with encoded bytes");
    }
    return fitted;
}

FrozenCategoricalMotif FrozenCategoricalMotif::from_bytes(std::span<const std::uint8_t> bytes,
                                                          std::uint64_t bits, CodeLimits limits) {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto pattern = read_pattern(reader);
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing frozen motif model bits");
    }
    return FrozenCategoricalMotif(pattern, limits);
}

EncodedFrozenMotif FrozenCategoricalMotif::encode(std::span<const TypeId> test, CodeLimits limits) const {
    check_region(test.size(), limits);
    auto starts = find_placements(test, std::span(pattern_).first(width_));
    BitWriter writer(limits.encoded_bytes);
    writer.gamma(test.size() + 1);
    CodeLength length{writer.position()};
    writer.gamma(starts.size() + 1);
    const auto placement_end = write_gaps_and_residual(writer, test, starts, width_);
    length.data_bits = writer.position() - length.header_bits;
    return {{writer.take_bytes(), length}, std::move(starts), placement_end - length.header_bits};
}

std::vector<TypeId> FrozenCategoricalMotif::decode(std::span<const std::uint8_t> bytes,
                                                   std::uint64_t bits, CodeLimits limits) const {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto count = reader.gamma() - 1;
    check_region(count, limits);
    return read_gaps_and_residual(reader, std::span(pattern_).first(width_), count, reader.gamma() - 1);
}
}
