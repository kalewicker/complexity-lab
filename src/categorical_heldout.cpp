#include <clab/categorical_heldout.hpp>

#include "categorical_bits.hpp"

#include <algorithm>
#include <functional>
#include <queue>

namespace clab {
namespace {
using categorical_detail::BitReader;
using categorical_detail::BitWriter;
using categorical_detail::symbol_bits;

void check_training_size(std::uint64_t count, CodeLimits limits) {
    if (count == 0) {
        throw Error(ErrorCode::invalid, "categorical training region is empty");
    }
    if (count > std::min(limits.symbols, categorical_training_limit)) {
        throw Error(ErrorCode::limit, "categorical training region cap exceeded");
    }
}
}

FrozenCategoricalModel FrozenCategoricalModel::fit(std::span<const TypeId> training, CodeLimits limits) {
    check_training_size(training.size(), limits);
    std::vector<std::pair<TypeId, std::uint32_t>> frequencies;
    std::map<TypeId, std::size_t> index;
    for (auto label : training) {
        const auto [it, inserted] = index.try_emplace(label, frequencies.size());
        if (inserted) {
            frequencies.emplace_back(label, 0);
        }
        ++frequencies[it->second].second;
    }
    return FrozenCategoricalModel(frequencies, limits);
}

FrozenCategoricalModel FrozenCategoricalModel::from_bytes(std::span<const std::uint8_t> bytes,
                                                          std::uint64_t bits, CodeLimits limits) {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto labels = reader.gamma();
    check_training_size(labels, limits);
    std::vector<std::pair<TypeId, std::uint32_t>> frequencies;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < labels; ++i) {
        const auto label = static_cast<TypeId>(reader.get(symbol_bits));
        const auto count = reader.gamma();
        total = checked_add(total, count);
        check_training_size(total, limits);
        frequencies.emplace_back(label, static_cast<std::uint32_t>(count));
    }
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing bits in frozen categorical model");
    }
    return FrozenCategoricalModel(frequencies, limits);
}

FrozenCategoricalModel::FrozenCategoricalModel(
    std::span<const std::pair<TypeId, std::uint32_t>> frequencies, CodeLimits limits) {
    BitWriter writer(limits.encoded_bytes);
    writer.gamma(frequencies.size());
    const auto leaves = frequencies.size() + 1;
    children_.resize(leaves);
    words_.resize(2 * leaves - 1);

    // Ties use node insertion order, beginning with first appearances in training.
    using Entry = std::pair<std::uint32_t, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (const auto& [label, count] : frequencies) {
        const auto i = alphabet_.size();
        if (!index_.emplace(label, i).second) {
            throw Error(ErrorCode::invalid, "duplicate label in frozen categorical model");
        }
        alphabet_.push_back(label);
        writer.put(label, symbol_bits);
        writer.gamma(count);
        queue.emplace(count, i);
    }
    queue.emplace(1, alphabet_.size()); // Last leaf is the unseen-label escape.
    while (queue.size() > 1) {
        const auto [left_weight, left] = queue.top();
        queue.pop();
        const auto [right_weight, right] = queue.top();
        queue.pop();
        queue.emplace(left_weight + right_weight, children_.size());
        children_.push_back({left, right});
    }
    // Children always precede their parent in the tree. The root word is empty.
    for (std::size_t node = children_.size(); node-- > leaves;) {
        const auto [left, right] = children_[node];
        const auto prefix = words_[node];
        if (prefix.width == 64) {
            throw Error(ErrorCode::limit, "categorical prefix depth cap exceeded");
        }
        words_[left] = {prefix.value << 1, prefix.width + 1};
        words_[right] = {(prefix.value << 1) | 1, prefix.width + 1};
    }

    const auto bits = writer.position();
    model_ = {writer.take_bytes(), {0, bits, 0}};
}

EncodedHeldOut FrozenCategoricalModel::encode(std::span<const TypeId> held_out, CodeLimits limits) const {
    if (held_out.size() > limits.symbols) {
        throw Error(ErrorCode::limit, "held-out categorical region cap exceeded");
    }
    BitWriter writer(limits.encoded_bytes);
    writer.gamma(checked_add(held_out.size(), 1));
    const auto header = writer.position();
    std::size_t unseen = 0;
    for (auto label : held_out) {
        const auto known = index_.find(label);
        const bool escape = known == index_.end();
        const auto word = words_[escape ? alphabet_.size() : known->second];
        writer.put(word.value, word.width);
        if (escape) {
            ++unseen;
            writer.put(label, symbol_bits);
        }
    }
    const auto data = writer.position() - header;
    return {{writer.take_bytes(), {header, 0, data}}, unseen};
}

std::vector<TypeId> FrozenCategoricalModel::decode(std::span<const std::uint8_t> bytes,
                                                    std::uint64_t bits, CodeLimits limits) const {
    BitReader reader(bytes, bits, limits.encoded_bytes);
    const auto count = reader.gamma() - 1;
    if (count > limits.symbols) {
        throw Error(ErrorCode::limit, "held-out categorical region cap exceeded");
    }
    if (count > reader.remaining()) {
        throw Error(ErrorCode::incomplete, "truncated frozen categorical payload");
    }
    std::vector<TypeId> result;
    result.reserve(static_cast<std::size_t>(count));
    const auto escape = alphabet_.size();
    for (std::uint64_t i = 0; i < count; ++i) {
        auto node = children_.size() - 1;
        while (node > escape) {
            node = children_[node][reader.get(1)];
        }
        if (node == escape) {
            const auto label = static_cast<TypeId>(reader.get(symbol_bits));
            if (index_.contains(label)) {
                throw Error(ErrorCode::invalid, "known label used a categorical escape");
            }
            result.push_back(label);
        } else {
            result.push_back(alphabet_[node]);
        }
    }
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing bits in frozen categorical payload");
    }
    return result;
}
}
