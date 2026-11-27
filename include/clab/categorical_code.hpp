#pragma once

#include <clab/foundation.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace clab {
enum class CategoricalCode : std::uint8_t { raw, uniform, run_length, periodic };

inline constexpr std::array categorical_codes{
    CategoricalCode::raw, CategoricalCode::uniform, CategoricalCode::run_length,
    CategoricalCode::periodic,
};

[[nodiscard]] std::string_view categorical_code_id(CategoricalCode code);

struct CodeLimits {
    std::size_t symbols = 1 << 20;
    std::size_t encoded_bytes = 8 << 20;
};

struct CodeLength {
    std::uint64_t header_bits{}, model_bits{}, data_bits{};
    [[nodiscard]] std::uint64_t total_bits() const;
    bool operator==(const CodeLength&) const = default;
};

struct EncodedCategorical {
    std::vector<std::uint8_t> bytes;
    CodeLength length;
};

[[nodiscard]] EncodedCategorical encode_categorical(std::span<const TypeId> symbols,
                                                     CategoricalCode code, CodeLimits limits = {});
inline constexpr std::string_view categorical_iid_code_id = "categorical.iid_enumerative.v1";
inline constexpr std::size_t categorical_iid_symbol_limit = 4096;

[[nodiscard]] EncodedCategorical encode_categorical_iid(std::span<const TypeId> symbols,
                                                        CodeLimits limits = {});
[[nodiscard]] std::vector<TypeId> decode_categorical_iid(std::span<const std::uint8_t> bytes,
                                                        std::uint64_t bit_count,
                                                        CodeLimits limits = {});

[[nodiscard]] std::vector<TypeId> decode_categorical(std::span<const std::uint8_t> bytes,
                                                     std::uint64_t bit_count, CodeLimits limits = {});
enum class CategoricalFamily : std::uint8_t { raw = 0, uniform, run_length, periodic, iid };
inline constexpr std::array categorical_families{
    CategoricalFamily::raw, CategoricalFamily::uniform, CategoricalFamily::run_length,
    CategoricalFamily::periodic, CategoricalFamily::iid,
};
inline constexpr unsigned categorical_family_bits = 3;

[[nodiscard]] std::string_view categorical_family_id(CategoricalFamily family);

struct CategoricalCandidate {
    CategoricalFamily family;
    // Absent when this candidate exceeds a budget. Header includes the common selector.
    std::optional<CodeLength> length;
};

struct CategoricalWinner {
    CategoricalFamily family;
    EncodedCategorical encoded;
};

struct CategoricalComparison {
    std::array<CategoricalCandidate, categorical_families.size()> candidates;
    // No winner is published for an incomplete set.
    std::optional<CategoricalWinner> winner;
};

[[nodiscard]] CategoricalComparison compare_categorical(std::span<const TypeId> symbols,
                                                         CodeLimits limits = {});
[[nodiscard]] std::vector<TypeId> decode_selected_categorical(std::span<const std::uint8_t> bytes,
                                                              std::uint64_t bit_count,
                                                              CodeLimits limits = {});

}
