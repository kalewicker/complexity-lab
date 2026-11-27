#pragma once

#include <clab/foundation.hpp>

#include <array>
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
[[nodiscard]] std::vector<TypeId> decode_categorical(std::span<const std::uint8_t> bytes,
                                                     std::uint64_t bit_count, CodeLimits limits = {});
}
