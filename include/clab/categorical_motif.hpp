#pragma once

#include <clab/categorical_code.hpp>

namespace clab {
inline constexpr std::string_view categorical_motif_code_id = "categorical.single_motif.v1";

struct CategoricalMotif {
    EncodedCategorical encoded;
    std::vector<TypeId> pattern;
    std::vector<std::size_t> placements;
    std::uint64_t placement_bits{};
};

struct CategoricalMotifComparison {
    CategoricalWinner baseline;
    std::optional<CategoricalMotif> motif;

    [[nodiscard]] bool improves_baseline() const {
        return motif && motif->encoded.length.total_bits() < baseline.encoded.length.total_bits();
    }
};

[[nodiscard]] CategoricalMotifComparison
compare_categorical_motif(std::span<const TypeId> symbols, CodeLimits limits = {});

[[nodiscard]] std::vector<TypeId>
decode_categorical_motif(std::span<const std::uint8_t> bytes, std::uint64_t bit_count,
                         CodeLimits limits = {});
}
