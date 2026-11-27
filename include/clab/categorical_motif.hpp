#pragma once

#include <clab/categorical_code.hpp>

namespace clab {
inline constexpr std::string_view categorical_motif_code_id = "categorical.single_motif.v1";
inline constexpr std::string_view categorical_frozen_motif_code_id = "categorical.frozen_motif.v1";
inline constexpr std::size_t categorical_motif_max_width = 6;

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

struct EncodedFrozenMotif {
    EncodedCategorical code;
    std::vector<std::size_t> placements;
    std::uint64_t placement_bits{};
};

class FrozenCategoricalMotif {
public:
    // An empty result means the training motif did not strictly beat the full baseline set.
    static std::optional<FrozenCategoricalMotif> fit(std::span<const TypeId> training,
                                                     CodeLimits limits = {});
    static FrozenCategoricalMotif from_bytes(std::span<const std::uint8_t> bytes, std::uint64_t bits,
                                              CodeLimits limits = {});

    [[nodiscard]] const EncodedCategorical& model_code() const & { return model_; }
    const EncodedCategorical& model_code() const && = delete;
    [[nodiscard]] EncodedFrozenMotif encode(std::span<const TypeId> test, CodeLimits limits = {}) const;
    [[nodiscard]] std::vector<TypeId> decode(std::span<const std::uint8_t> bytes,
                                             std::uint64_t bits, CodeLimits limits = {}) const;

private:
    explicit FrozenCategoricalMotif(std::span<const TypeId> pattern, CodeLimits limits);

    std::array<TypeId, categorical_motif_max_width> pattern_{};
    std::size_t width_;
    EncodedCategorical model_;
};
}
