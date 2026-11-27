#pragma once

#include <clab/categorical_code.hpp>

#include <map>
#include <utility>

namespace clab {
inline constexpr std::size_t categorical_training_limit = 4096;
inline constexpr std::string_view categorical_frozen_code_id = "categorical.frozen_huffman.v1";

struct EncodedHeldOut {
    EncodedCategorical code;
    std::size_t unseen_symbols{};
};

class FrozenCategoricalModel {
public:
    static FrozenCategoricalModel fit(std::span<const TypeId> training, CodeLimits limits = {});
    static FrozenCategoricalModel from_bytes(std::span<const std::uint8_t> bytes,
                                             std::uint64_t bits, CodeLimits limits = {});

    [[nodiscard]] const EncodedCategorical& model_code() const & { return model_; }
    const EncodedCategorical& model_code() const && = delete;
    [[nodiscard]] EncodedHeldOut encode(std::span<const TypeId> held_out, CodeLimits limits = {}) const;
    [[nodiscard]] std::vector<TypeId> decode(std::span<const std::uint8_t> bytes,
                                             std::uint64_t bits, CodeLimits limits = {}) const;

private:
    FrozenCategoricalModel(std::span<const std::pair<TypeId, std::uint32_t>> frequencies,
                           CodeLimits limits);

    struct Word {
        std::uint64_t value{};
        unsigned width{};
    };

    EncodedCategorical model_;
    std::vector<TypeId> alphabet_;
    std::map<TypeId, std::size_t> index_;
    std::vector<std::array<std::size_t, 2>> children_;
    std::vector<Word> words_;
};
}
