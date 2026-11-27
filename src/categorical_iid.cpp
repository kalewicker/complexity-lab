#include <clab/categorical_code.hpp>
#include <clab/histogram_cache.hpp>

#include "categorical_bits.hpp"

#include <algorithm>
#include <numeric>

namespace clab {
namespace {
using namespace categorical_detail;

// Little-endian 32-bit limbs. At the region cap a rank is bounded by 4096!.
class Rank {
public:
    explicit Rank(std::uint32_t value = 0) {
        if (value) {
            words_.push_back(value);
        }
    }

    void multiply(std::uint32_t factor) {
        std::uint64_t carry = 0;
        for (auto& word : words_) {
            const auto value = std::uint64_t{word} * factor + carry;
            word = static_cast<std::uint32_t>(value);
            carry = value >> 32;
        }
        if (carry) {
            words_.push_back(static_cast<std::uint32_t>(carry));
        }
        trim();
    }

    void divide(std::uint32_t divisor) {
        std::uint64_t remainder = 0;
        for (auto it = words_.rbegin(); it != words_.rend(); ++it) {
            const auto value = (remainder << 32) | *it;
            *it = static_cast<std::uint32_t>(value / divisor);
            remainder = value % divisor;
        }
        if (remainder != 0) {
            throw std::logic_error("nonintegral categorical IID block");
        }
        trim();
    }

    void add(const Rank& rhs) {
        words_.resize(std::max(words_.size(), rhs.words_.size()));
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            const auto value = std::uint64_t{words_[i]} + rhs.word(i) + carry;
            words_[i] = static_cast<std::uint32_t>(value);
            carry = value >> 32;
        }
        if (carry) {
            words_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    // The caller has established *this >= rhs.
    void subtract(const Rank& rhs) {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            const auto subtrahend = std::uint64_t{rhs.word(i)} + borrow;
            borrow = words_[i] < subtrahend;
            words_[i] = static_cast<std::uint32_t>(std::uint64_t{words_[i]} - subtrahend);
        }
        trim();
    }

    [[nodiscard]] bool less_than(const Rank& rhs) const {
        if (words_.size() != rhs.words_.size()) {
            return words_.size() < rhs.words_.size();
        }
        return std::lexicographical_compare(words_.rbegin(), words_.rend(),
                                            rhs.words_.rbegin(), rhs.words_.rend());
    }

    [[nodiscard]] std::size_t bit_width() const {
        return words_.empty() ? 0 : 32 * (words_.size() - 1) + std::bit_width(words_.back());
    }

    void write(BitWriter& writer, std::size_t width) const {
        while (width) {
            const auto i = (width - 1) / 32;
            writer.put(word(i), static_cast<unsigned>(width - 32 * i));
            width = 32 * i;
        }
    }

    static Rank read(BitReader& reader, std::size_t width) {
        Rank value;
        value.words_.resize((width + 31) / 32);
        while (width) {
            const auto i = (width - 1) / 32;
            value.words_[i] = static_cast<std::uint32_t>(
                reader.get(static_cast<unsigned>(width - 32 * i)));
            width = 32 * i;
        }
        value.trim();
        return value;
    }

private:
    [[nodiscard]] std::uint32_t word(std::size_t i) const {
        return i < words_.size() ? words_[i] : 0;
    }

    void trim() {
        while (!words_.empty() && words_.back() == 0) {
            words_.pop_back();
        }
    }

    std::vector<std::uint32_t> words_;
};

Rank permutations(std::span<const std::uint32_t> counts) {
    Rank value(1);
    std::uint32_t seen = 0;
    for (auto count : counts) {
        for (std::uint32_t i = 1; i <= count; ++i) {
            value.multiply(++seen);
            value.divide(i);
        }
    }
    return value;
}

Rank scaled(Rank value, std::uint32_t numerator, std::uint32_t denominator) {
    value.multiply(numerator);
    value.divide(denominator);
    return value;
}

std::size_t rank_width(Rank size) {
    size.subtract(Rank(1));
    return size.bit_width();
}

void check_region(std::uint64_t count, CodeLimits limits) {
    if (count > std::min(limits.symbols, categorical_iid_symbol_limit)) {
        throw Error(ErrorCode::limit, "categorical IID region cap exceeded");
    }
}

struct RankModel {
    explicit RankModel(std::span<const std::uint32_t> counts)
        : choices(permutations(counts)), running_counts(counts.size()) {
        std::partial_sum(counts.begin(), counts.end(), running_counts.begin());
        remaining = running_counts.back();
    }

    [[nodiscard]] std::uint32_t before(std::size_t index) const {
        return index ? running_counts[index - 1] : 0;
    }

    [[nodiscard]] Rank offset(std::size_t index) const {
        return scaled(choices, before(index), remaining);
    }

    [[nodiscard]] std::size_t locate(Rank rank) const {
        // Compare cumulative block ends against the rank without a rounded division.
        rank.multiply(remaining);
        const auto found = std::ranges::partition_point(running_counts, [&](std::uint32_t prefix) {
            auto end = choices;
            end.multiply(prefix);
            return !rank.less_than(end);
        });
        return static_cast<std::size_t>(found - running_counts.begin());
    }

    void consume(std::size_t index) {
        choices = scaled(choices, running_counts[index] - before(index), remaining);
        for (std::size_t i = index; i < running_counts.size(); ++i) {
            --running_counts[i];
        }
        --remaining;
    }

    Rank choices;
    std::vector<std::uint32_t> running_counts;
    std::uint32_t remaining{};
};
}

EncodedCategorical encode_categorical_iid(std::span<const TypeId> symbols, CodeLimits limits) {
    check_region(symbols.size(), limits);
    BitWriter writer(limits.encoded_bytes);
    writer.gamma(symbols.size() + 1);
    CodeLength length{writer.position()};
    if (!symbols.empty()) {
        const auto histogram = count_symbols(symbols);
        writer.gamma(histogram.size());
        std::vector<TypeId> alphabet;
        std::vector<std::uint32_t> counts;
        for (const auto& [label, count] : histogram) {
            alphabet.push_back(label);
            counts.push_back(static_cast<std::uint32_t>(count));
            writer.put(label, symbol_bits);
            writer.gamma(count);
        }
        length.model_bits = writer.position() - length.header_bits;
        RankModel model(counts);
        const auto width = rank_width(model.choices);
        Rank rank;
        for (auto label : symbols) {
            const auto i = static_cast<std::size_t>(std::ranges::lower_bound(alphabet, label) -
                                                   alphabet.begin());
            rank.add(model.offset(i));
            model.consume(i);
        }
        rank.write(writer, width);
        length.data_bits = writer.position() - length.header_bits - length.model_bits;
    }
    return {writer.take_bytes(), length};
}

std::vector<TypeId> decode_categorical_iid(std::span<const std::uint8_t> bytes,
                                          std::uint64_t bit_count, CodeLimits limits) {
    BitReader reader(bytes, bit_count, limits.encoded_bytes);
    const auto count = reader.gamma() - 1;
    check_region(count, limits);
    std::vector<TypeId> result;
    if (count != 0) {
        const auto k = read_model_count(reader, count);
        std::vector<TypeId> alphabet;
        std::vector<std::uint32_t> counts;
        std::uint64_t tallied = 0;
        for (std::size_t i = 0; i < k; ++i) {
            const auto label = static_cast<TypeId>(reader.get(symbol_bits));
            const auto frequency = reader.gamma();
            if (frequency > count - tallied || (!alphabet.empty() && alphabet.back() >= label)) {
                throw Error(ErrorCode::invalid, "invalid categorical IID histogram");
            }
            alphabet.push_back(label);
            counts.push_back(static_cast<std::uint32_t>(frequency));
            tallied += frequency;
        }
        if (tallied != count) {
            throw Error(ErrorCode::invalid, "categorical IID histogram does not sum to length");
        }
        RankModel model(counts);
        auto rank = Rank::read(reader, rank_width(model.choices));
        if (!rank.less_than(model.choices)) {
            throw Error(ErrorCode::invalid, "categorical IID rank outside composition");
        }
        result.reserve(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto index = model.locate(rank);
            result.push_back(alphabet[index]);
            rank.subtract(model.offset(index));
            model.consume(index);
        }
    }
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing categorical IID bits");
    }
    return result;
}
}
