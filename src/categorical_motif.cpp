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
constexpr std::uint8_t dictionary_tag = 6;
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

using OccurrenceTable = std::map<std::vector<TypeId>, Occurrences>;

void scan_motif_region(OccurrenceTable& occurrences, std::span<const TypeId> symbols, std::size_t offset) {
    for (std::size_t start = 0; start + min_width <= symbols.size(); ++start) {
        for (std::size_t width = min_width; width <= max_width && start + width <= symbols.size(); ++width) {
            const auto match = symbols.subspan(start, width);
            const auto absolute = offset + start;
            auto& slot = occurrences.try_emplace(std::vector<TypeId>(match.begin(), match.end()),
                                                  Occurrences{absolute}).first->second;
            if (absolute >= slot.next) {
                ++slot.count;
                slot.gap_bits += gamma_bits(absolute - slot.next + 1);
                slot.next = absolute + width;
            }
        }
    }
}

OccurrenceTable scan_motifs(std::span<const TypeId> symbols) {
    OccurrenceTable occurrences;
    scan_motif_region(occurrences, symbols, 0);
    return occurrences;
}

struct FrameMatches {
    std::size_t first{}, count{};
    std::uint64_t saved{};
};

using FrameMatchTable = std::map<std::vector<TypeId>, FrameMatches>;

void accumulate_frame_matches(FrameMatchTable& matches, const OccurrenceTable& occurrences,
                              std::size_t frame_start) {
    for (const auto& [candidate, hit] : occurrences) {
        auto& total = matches.try_emplace(candidate, FrameMatches{frame_start + hit.first}).first->second;
        total.count += hit.count;
        // The empty placement stream already pays one bit for a zero count.
        const auto added_placement_bits = gamma_bits(hit.count + 1) - 1 + hit.gap_bits;
        total.saved += hit.count * candidate.size() * symbol_bits - added_placement_bits;
    }
}

struct FrameMotifCandidate {
    std::vector<TypeId> pattern;
    std::uint64_t bits;
};

std::optional<FrameMotifCandidate> best_frame_motif(const FrameMatchTable& matches, std::uint64_t empty_bits,
                                                    std::span<const TypeId> excluded = {}) {
    using Order = std::tuple<std::uint64_t, std::size_t, std::size_t>;
    std::optional<Order> best;
    const std::vector<TypeId>* pattern = nullptr;
    for (const auto& [candidate, hit] : matches) {
        if (hit.count < 2 || std::ranges::equal(candidate, excluded)) {
            continue;
        }
        const auto width = candidate.size();
        const auto bits = empty_bits - hit.saved + gamma_bits(width) + width * symbol_bits;
        const Order order{bits, hit.first, width};
        if (!best || order < *best) {
            best = order;
            pattern = &candidate;
        }
    }
    if (!pattern) {
        return std::nullopt;
    }
    return FrameMotifCandidate{*pattern, std::get<0>(*best)};
}

struct FrameTrainingComparison {
    std::uint64_t baseline_bits{};
    std::optional<FrameMotifCandidate> first;
};

FrameTrainingComparison compare_training_frames(std::span<const TypeId> training, std::size_t sites,
                                                 CodeLimits limits) {
    if (!sites || training.empty() || training.size() % sites) {
        throw Error(ErrorCode::invalid, "frozen motif training requires whole nonempty frames");
    }
    check_region(training.size(), limits);
    FrameTrainingComparison result;
    FrameMatchTable matches;
    for (std::size_t start = 0; start < training.size(); start += sites) {
        const auto frame = training.subspan(start, sites);
        const auto baseline = compare_categorical(frame, limits);
        if (!baseline.winner) {
            throw Error(ErrorCode::limit, "incomplete categorical training baseline set");
        }
        result.baseline_bits += baseline.winner->encoded.length.total_bits();
        accumulate_frame_matches(matches, scan_motifs(frame), start);
    }
    const auto empty_frame_bits = gamma_bits(sites + 1) + 1 + sites * symbol_bits;
    result.first = best_frame_motif(matches, empty_frame_bits * (training.size() / sites));
    return result;
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

void write_gaps(BitWriter& writer, std::span<const std::size_t> starts, std::size_t width) {
    std::size_t cursor = 0;
    for (auto start : starts) {
        writer.gamma(start - cursor + 1);
        cursor = start + width;
    }
}

template <typename Visit>
void visit_uncovered(std::span<const TypeId> symbols, std::span<const std::size_t> starts,
                     std::size_t width, Visit visit) {
    std::size_t cursor = 0;
    for (auto start : starts) {
        visit(cursor, symbols.subspan(cursor, start - cursor));
        cursor = start + width;
    }
    visit(cursor, symbols.subspan(cursor));
}

std::vector<std::size_t> find_uncovered_placements(std::span<const TypeId> symbols,
                                                    std::span<const std::size_t> first, std::size_t first_width,
                                                    std::span<const TypeId> pattern) {
    std::vector<std::size_t> starts;
    visit_uncovered(symbols, first, first_width, [&](std::size_t offset, std::span<const TypeId> region) {
        for (auto start : find_placements(region, pattern)) {
            starts.push_back(offset + start);
        }
    });
    return starts;
}

std::uint64_t write_gaps_and_residual(BitWriter& writer, std::span<const TypeId> symbols,
                                      std::span<const std::size_t> starts, std::size_t width) {
    write_gaps(writer, starts, width);
    const auto placement_end = writer.position();
    visit_uncovered(symbols, starts, width, [&](std::size_t, std::span<const TypeId> region) {
        write_symbols(writer, region);
    });
    return placement_end;
}

std::vector<std::size_t> read_motif_starts(BitReader& reader, std::size_t width, std::uint64_t count,
                                          std::uint64_t placements) {
    if (placements > count / width) {
        throw Error(ErrorCode::invalid, "invalid categorical motif placement count");
    }
    std::vector<std::size_t> starts;
    std::uint64_t cursor = 0;
    for (std::uint64_t i = 0; i < placements; ++i) {
        const auto gap = reader.gamma() - 1;
        if (width > count - cursor || gap > count - cursor - width) {
            throw Error(ErrorCode::invalid, "categorical motif placement exceeds region");
        }
        starts.push_back(static_cast<std::size_t>(cursor + gap));
        cursor += gap + width;
    }
    return starts;
}

std::vector<TypeId> read_gaps_and_residual(BitReader& reader, std::span<const TypeId> pattern,
                                           std::uint64_t count, std::uint64_t placements) {
    const auto width = pattern.size();
    const auto starts = read_motif_starts(reader, width, count, placements);
    const auto residual = read_symbols(reader, static_cast<std::size_t>(count - placements * width));
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing categorical motif bits");
    }
    std::vector<TypeId> result;
    result.reserve(static_cast<std::size_t>(count));
    auto rest = std::span(residual);
    std::size_t cursor = 0;
    for (auto start : starts) {
        const auto gap = start - cursor;
        result.insert(result.end(), rest.begin(), rest.begin() + gap);
        result.insert(result.end(), pattern.begin(), pattern.end());
        rest = rest.subspan(gap);
        cursor = start + width;
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

std::vector<std::pair<std::size_t, std::size_t>>
ordered_placements(const std::array<CategoricalMotifEntry, 2>& entries) {
    std::vector<std::pair<std::size_t, std::size_t>> placements;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (auto start : entries[i].placements) {
            placements.emplace_back(start, i);
        }
    }
    std::ranges::sort(placements);
    return placements;
}

std::uint64_t write_dictionary_data(BitWriter& writer, std::span<const TypeId> symbols,
                                      const std::array<CategoricalMotifEntry, 2>& entries, bool allow_absent) {
    const auto data_start = writer.position();
    for (const auto& entry : entries) {
        writer.gamma(entry.placements.size() + static_cast<std::size_t>(allow_absent));
        write_gaps(writer, entry.placements, entry.pattern.size());
    }
    const auto placement_bits = writer.position() - data_start;
    std::size_t cursor = 0;
    for (auto [start, index] : ordered_placements(entries)) {
        write_symbols(writer, symbols.subspan(cursor, start - cursor));
        cursor = start + entries[index].pattern.size();
    }
    write_symbols(writer, symbols.subspan(cursor));
    return placement_bits;
}

CategoricalMotifDictionary encode_dictionary(std::span<const TypeId> symbols,
                                             std::array<CategoricalMotifEntry, 2> entries, CodeLimits limits) {
    BitWriter writer(limits.encoded_bytes);
    writer.put(dictionary_tag, categorical_family_bits);
    writer.gamma(symbols.size() + 1);
    CodeLength length{writer.position()};
    for (const auto& entry : entries) {
        write_pattern(writer, entry.pattern);
    }
    length.model_bits = writer.position() - length.header_bits;
    const auto placement_bits = write_dictionary_data(writer, symbols, entries, false);
    length.data_bits = writer.position() - length.header_bits - length.model_bits;
    return {{writer.take_bytes(), length}, std::move(entries), placement_bits};
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

bool CategoricalMotifDictionaryComparison::selects_dictionary() const {
    if (!dictionary || !single.motif) {
        return false;
    }
    const auto bits = dictionary->encoded.length.total_bits();
    return bits < single.motif->encoded.length.total_bits() && bits < single.baseline.encoded.length.total_bits();
}

CategoricalMotifDictionaryComparison compare_categorical_motif_dictionary(std::span<const TypeId> symbols,
                                                                          CodeLimits limits) {
    CategoricalMotifDictionaryComparison result{compare_categorical_motif(symbols, limits), std::nullopt};
    if (!result.single.motif) {
        return result;
    }
    const auto& first = *result.single.motif;
    const auto first_width = first.pattern.size();
    OccurrenceTable occurrences;
    visit_uncovered(symbols, first.placements, first_width, [&](std::size_t offset, std::span<const TypeId> region) {
        scan_motif_region(occurrences, region, offset);
    });

    using Order = std::tuple<std::uint64_t, std::size_t, std::size_t>;
    std::optional<Order> best;
    const std::vector<TypeId>* pattern = nullptr;
    for (const auto& [candidate, hit] : occurrences) {
        if (hit.count < 2 || candidate == first.pattern) {
            continue;
        }
        const auto width = candidate.size();
        // The first entry's placement stream stays the same when the second entry is added.
        const auto bits = first.encoded.length.total_bits() - hit.count * width * symbol_bits
                          + gamma_bits(width) + width * symbol_bits + gamma_bits(hit.count) + hit.gap_bits;
        const Order order{bits, hit.first, width};
        if (!best || order < *best) {
            best = order;
            pattern = &candidate;
        }
    }
    if (pattern) {
        std::array<CategoricalMotifEntry, 2> entries{{
            {first.pattern, first.placements},
            {*pattern, find_uncovered_placements(symbols, first.placements, first_width, *pattern)},
        }};
        auto encoded = encode_dictionary(symbols, std::move(entries), limits);
        if (encoded.encoded.length.total_bits() != std::get<0>(*best)) {
            throw std::logic_error("categorical dictionary cost disagrees with encoded bytes");
        }
        result.dictionary = std::move(encoded);
    }
    return result;
}

namespace {
std::array<CategoricalMotifEntry, 2> read_dictionary_model(BitReader& reader, std::uint64_t count = max_width) {
    std::array<CategoricalMotifEntry, 2> entries;
    for (auto& entry : entries) {
        entry.pattern = read_pattern(reader, count);
    }
    if (entries[0].pattern == entries[1].pattern) {
        throw Error(ErrorCode::invalid, "duplicate categorical dictionary entry");
    }
    return entries;
}

std::vector<TypeId> read_dictionary_data(BitReader& reader, std::uint64_t count,
                                          std::array<CategoricalMotifEntry, 2> entries, bool allow_absent) {
    for (auto& entry : entries) {
        const auto placements = reader.gamma() - static_cast<std::uint64_t>(allow_absent);
        if (!allow_absent && placements < 2) {
            throw Error(ErrorCode::invalid, "invalid categorical motif placement count");
        }
        entry.placements = read_motif_starts(reader, entry.pattern.size(), count, placements);
    }
    const auto ordered = ordered_placements(entries);
    std::size_t cursor = 0;
    std::size_t covered = 0;
    for (auto [start, index] : ordered) {
        if (start < cursor) {
            throw Error(ErrorCode::invalid, "overlapping categorical dictionary placements");
        }
        const auto width = entries[index].pattern.size();
        cursor = start + width;
        covered += width;
    }
    const auto residual = read_symbols(reader, static_cast<std::size_t>(count) - covered);
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing categorical dictionary bits");
    }
    std::vector<TypeId> result;
    result.reserve(static_cast<std::size_t>(count));
    auto rest = std::span(residual);
    cursor = 0;
    for (auto [start, index] : ordered) {
        const auto gap = start - cursor;
        const auto& pattern = entries[index].pattern;
        result.insert(result.end(), rest.begin(), rest.begin() + gap);
        result.insert(result.end(), pattern.begin(), pattern.end());
        rest = rest.subspan(gap);
        cursor = start + pattern.size();
    }
    result.insert(result.end(), rest.begin(), rest.end());
    return result;
}
}

std::vector<TypeId> decode_categorical_motif_dictionary(std::span<const std::uint8_t> bytes,
                                                       std::uint64_t bit_count, CodeLimits limits) {
    BitReader reader(bytes, bit_count, limits.encoded_bytes);
    if (reader.get(categorical_family_bits) != dictionary_tag) {
        throw Error(ErrorCode::invalid, "invalid categorical dictionary tag");
    }
    const auto count = reader.gamma() - 1;
    check_region(count, limits);
    return read_dictionary_data(reader, count, read_dictionary_model(reader, count), false);
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
    const auto comparison = compare_training_frames(training, sites, limits);
    if (!comparison.first || comparison.first->bits >= comparison.baseline_bits) {
        return std::nullopt;
    }
    FrozenCategoricalMotif fitted(comparison.first->pattern, limits);
    auto encoded_bits = fitted.model_code().length.total_bits();
    for (std::size_t start = 0; start < training.size(); start += sites) {
        encoded_bits += fitted.encode(training.subspan(start, sites), limits).code.length.total_bits();
    }
    if (encoded_bits != comparison.first->bits) {
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

FrozenCategoricalMotifDictionary::FrozenCategoricalMotifDictionary(std::span<const TypeId> first,
                                                                   std::span<const TypeId> second,
                                                                   CodeLimits limits)
    : first_(first, limits), second_width_(second.size()) {
    std::ranges::copy(second, second_.begin());
    BitWriter writer(limits.encoded_bytes);
    write_pattern(writer, first);
    write_pattern(writer, second);
    const CodeLength length{0, writer.position(), 0};
    model_ = {writer.take_bytes(), length};
}

std::optional<FrozenCategoricalMotifDictionary>
FrozenCategoricalMotifDictionary::fit(std::span<const TypeId> training, CodeLimits limits) {
    const auto comparison = compare_categorical_motif_dictionary(training, limits);
    if (!comparison.selects_dictionary()) {
        return std::nullopt;
    }
    const auto& entries = comparison.dictionary->entries;
    return FrozenCategoricalMotifDictionary(entries[0].pattern, entries[1].pattern, limits);
}

std::optional<FrozenCategoricalMotifDictionary>
FrozenCategoricalMotifDictionary::fit_frames(std::span<const TypeId> training, std::size_t sites, CodeLimits limits) {
    const auto comparison = compare_training_frames(training, sites, limits);
    if (!comparison.first) {
        return std::nullopt;
    }
    const auto& first = *comparison.first;
    FrameMatchTable matches;
    for (std::size_t start = 0; start < training.size(); start += sites) {
        const auto frame = training.subspan(start, sites);
        const auto covered = find_placements(frame, first.pattern);
        OccurrenceTable remaining;
        visit_uncovered(frame, covered, first.pattern.size(), [&](std::size_t offset, std::span<const TypeId> region) {
            scan_motif_region(remaining, region, offset);
        });
        accumulate_frame_matches(matches, remaining, start);
    }
    const auto empty_second_bits = first.bits + training.size() / sites;
    const auto second = best_frame_motif(matches, empty_second_bits, first.pattern);
    if (!second || second->bits >= std::min(first.bits, comparison.baseline_bits)) {
        return std::nullopt;
    }
    FrozenCategoricalMotifDictionary fitted(first.pattern, second->pattern, limits);
    auto encoded_bits = fitted.model_code().length.total_bits();
    for (std::size_t start = 0; start < training.size(); start += sites) {
        encoded_bits += fitted.encode(training.subspan(start, sites), limits).encoded.length.total_bits();
    }
    if (encoded_bits != second->bits) {
        throw std::logic_error("frozen motif dictionary training cost disagrees with encoded bytes");
    }
    return fitted;
}

FrozenCategoricalMotifDictionary FrozenCategoricalMotifDictionary::from_bytes(
    std::span<const std::uint8_t> bytes, std::uint64_t bits, CodeLimits limits) {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto entries = read_dictionary_model(reader);
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing frozen motif dictionary model bits");
    }
    return FrozenCategoricalMotifDictionary(entries[0].pattern, entries[1].pattern, limits);
}

CategoricalMotifDictionary FrozenCategoricalMotifDictionary::encode(std::span<const TypeId> test,
                                                                    CodeLimits limits) const {
    check_region(test.size(), limits);
    const auto first = std::span(first_.pattern_).first(first_.width_);
    const auto second = std::span(second_).first(second_width_);
    auto starts = find_placements(test, first);
    auto second_starts = find_uncovered_placements(test, starts, first.size(), second);
    std::array<CategoricalMotifEntry, 2> entries{{
        {{first.begin(), first.end()}, std::move(starts)},
        {{second.begin(), second.end()}, std::move(second_starts)},
    }};
    BitWriter writer(limits.encoded_bytes);
    writer.gamma(test.size() + 1);
    CodeLength length{writer.position()};
    const auto placement_bits = write_dictionary_data(writer, test, entries, true);
    length.data_bits = writer.position() - length.header_bits;
    return {{writer.take_bytes(), length}, std::move(entries), placement_bits};
}

std::vector<TypeId> FrozenCategoricalMotifDictionary::decode(std::span<const std::uint8_t> bytes, std::uint64_t bits,
                                                             CodeLimits limits) const {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto count = reader.gamma() - 1;
    check_region(count, limits);
    std::array<CategoricalMotifEntry, 2> entries{{
        {{first_.pattern_.begin(), first_.pattern_.begin() + first_.width_}, {}},
        {{second_.begin(), second_.begin() + second_width_}, {}},
    }};
    return read_dictionary_data(reader, count, std::move(entries), true);
}
}
