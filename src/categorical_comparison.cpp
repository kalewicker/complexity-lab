#include <clab/categorical_code.hpp>

#include "categorical_bits.hpp"

#include <algorithm>
#include <utility>

namespace clab {
namespace {
using categorical_detail::BitReader;
using categorical_detail::BitWriter;
constexpr unsigned legacy_selector_bits = 2;

CategoricalCode legacy_code(CategoricalFamily family) {
    const auto index = static_cast<std::size_t>(family);
    if (index >= categorical_codes.size()) {
        throw Error(ErrorCode::invalid, "unknown categorical family");
    }
    return categorical_codes[index];
}

void copy_remaining(BitReader& source, BitWriter& destination) {
    while (source.remaining()) {
        const auto width = static_cast<unsigned>(std::min<std::uint64_t>(64, source.remaining()));
        destination.put(source.get(width), width);
    }
}
}

std::string_view categorical_family_id(CategoricalFamily family) {
    if (family == CategoricalFamily::iid) {
        return categorical_iid_code_id;
    }
    return categorical_code_id(legacy_code(family));
}

CategoricalComparison compare_categorical(std::span<const TypeId> symbols, CodeLimits limits) {
    CategoricalComparison comparison{};
    std::optional<std::size_t> best;
    EncodedCategorical best_source;
    bool complete = true;
    for (std::size_t i = 0; i < categorical_families.size(); ++i) {
        const auto family = categorical_families[i];
        auto& candidate = comparison.candidates[i];
        candidate.family = family;
        try {
            const bool iid = family == CategoricalFamily::iid;
            auto source = iid ? encode_categorical_iid(symbols, limits)
                              : encode_categorical(symbols, legacy_code(family), limits);
            auto length = source.length;
            const auto original_selector = iid ? 0 : legacy_selector_bits;
            // Try the common frame's budget before publishing this candidate's cost.
            length.header_bits = checked_add(length.header_bits - original_selector, categorical_family_bits);
            const auto total = length.total_bits();
            if (total / 8 + (total % 8 != 0) > limits.encoded_bytes) {
                complete = false;
                continue;
            }
            candidate.length = length;
            if (!best || total < comparison.candidates[*best].length->total_bits()) {
                best = i;
                best_source = std::move(source);
            }
        } catch (const Error& error) {
            if (error.code() != ErrorCode::limit) {
                throw;
            }
            complete = false;
        }
    }
    if (complete && best) {
        const auto& winner = comparison.candidates[*best];
        BitReader reader(best_source.bytes, best_source.length.total_bits(), limits.encoded_bytes);
        if (winner.family != CategoricalFamily::iid) {
            (void)reader.get(legacy_selector_bits);
        }
        BitWriter frame(limits.encoded_bytes);
        frame.put(static_cast<std::uint8_t>(winner.family), categorical_family_bits);
        copy_remaining(reader, frame);
        comparison.winner = CategoricalWinner{winner.family, {frame.take_bytes(), *winner.length}};
    }
    return comparison;
}

std::vector<TypeId> decode_selected_categorical(std::span<const std::uint8_t> bytes,
                                                 std::uint64_t bit_count, CodeLimits limits) {
    BitReader frame(bytes, bit_count, limits.encoded_bytes);
    const auto family = static_cast<CategoricalFamily>(frame.get(categorical_family_bits));
    BitWriter source(limits.encoded_bytes);
    if (family != CategoricalFamily::iid) {
        source.put(static_cast<std::uint8_t>(legacy_code(family)), legacy_selector_bits);
    }
    copy_remaining(frame, source);
    const auto source_bits = source.position();
    const auto source_bytes = source.take_bytes();
    if (family == CategoricalFamily::iid) {
        return decode_categorical_iid(source_bytes, source_bits, limits);
    }
    return decode_categorical(source_bytes, source_bits, limits);
}
}
