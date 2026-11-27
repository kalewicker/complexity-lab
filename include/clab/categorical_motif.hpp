#pragma once

#include <clab/categorical_code.hpp>

namespace clab {
inline constexpr std::string_view categorical_motif_code_id = "categorical.single_motif.v1";
inline constexpr std::string_view categorical_motif_dictionary_code_id = "categorical.two_motif.v1";
inline constexpr std::string_view categorical_frozen_motif_code_id = "categorical.frozen_motif.v1";
inline constexpr std::string_view categorical_frozen_dictionary_code_id = "categorical.frozen_two_motif.v1";
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

struct CategoricalMotifEntry {
    std::vector<TypeId> pattern;
    std::vector<std::size_t> placements;
};

struct CategoricalMotifDictionary {
    EncodedCategorical encoded;
    std::array<CategoricalMotifEntry, 2> entries;
    std::uint64_t placement_bits{};
};

struct CategoricalMotifDictionaryComparison {
    CategoricalMotifComparison single;
    std::optional<CategoricalMotifDictionary> dictionary;

    [[nodiscard]] bool selects_dictionary() const;
};

[[nodiscard]] CategoricalMotifDictionaryComparison
compare_categorical_motif_dictionary(std::span<const TypeId> symbols, CodeLimits limits = {});

[[nodiscard]] std::vector<TypeId>
decode_categorical_motif_dictionary(std::span<const std::uint8_t> bytes, std::uint64_t bit_count,
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
    // Fits on equal-width frames, with placements confined to each frame.
    static std::optional<FrozenCategoricalMotif> fit_frames(std::span<const TypeId> training,
                                                            std::size_t sites, CodeLimits limits = {});
    static FrozenCategoricalMotif from_bytes(std::span<const std::uint8_t> bytes, std::uint64_t bits,
                                              CodeLimits limits = {});

    [[nodiscard]] const EncodedCategorical& model_code() const & { return model_; }
    const EncodedCategorical& model_code() const && = delete;
    [[nodiscard]] EncodedFrozenMotif encode(std::span<const TypeId> test, CodeLimits limits = {}) const;
    [[nodiscard]] std::vector<TypeId> decode(std::span<const std::uint8_t> bytes,
                                             std::uint64_t bits, CodeLimits limits = {}) const;

private:
    friend class FrozenCategoricalMotifDictionary;
    explicit FrozenCategoricalMotif(std::span<const TypeId> pattern, CodeLimits limits);

    std::array<TypeId, categorical_motif_max_width> pattern_{};
    std::size_t width_;
    EncodedCategorical model_;
};

class FrozenCategoricalMotifDictionary {
public:
    static std::optional<FrozenCategoricalMotifDictionary> fit(std::span<const TypeId> training,
                                                               CodeLimits limits = {});
    static std::optional<FrozenCategoricalMotifDictionary> fit_frames(std::span<const TypeId> training,
                                                                      std::size_t sites, CodeLimits limits = {});
    static FrozenCategoricalMotifDictionary from_bytes(std::span<const std::uint8_t> bytes, std::uint64_t bits,
                                                        CodeLimits limits = {});

    [[nodiscard]] const EncodedCategorical& model_code() const & { return model_; }
    const EncodedCategorical& model_code() const && = delete;
    // The one-entry comparator uses the same first pattern as the fitted dictionary.
    [[nodiscard]] const FrozenCategoricalMotif& first_entry() const & { return first_; }
    const FrozenCategoricalMotif& first_entry() const && = delete;
    [[nodiscard]] CategoricalMotifDictionary encode(std::span<const TypeId> test, CodeLimits limits = {}) const;
    [[nodiscard]] std::vector<TypeId> decode(std::span<const std::uint8_t> bytes,
                                             std::uint64_t bits, CodeLimits limits = {}) const;

private:
    FrozenCategoricalMotifDictionary(std::span<const TypeId> first, std::span<const TypeId> second,
                                     CodeLimits limits);

    FrozenCategoricalMotif first_;
    std::array<TypeId, categorical_motif_max_width> second_{};
    std::size_t second_width_;
    EncodedCategorical model_;
};
}
