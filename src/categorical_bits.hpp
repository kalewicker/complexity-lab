#pragma once

#include <clab/categorical_code.hpp>

#include <bit>
#include <utility>

namespace clab::categorical_detail {
constexpr unsigned symbol_bits = 32;

class BitWriter {
public:
    explicit BitWriter(std::size_t max_bytes) : max_bits_(checked_multiply(max_bytes, 8)) {}

    void put(std::uint64_t value, unsigned width) {
        if (width > max_bits_ - position_) {
            throw Error(ErrorCode::limit, "categorical code byte cap exceeded");
        }
        for (unsigned i = width; i > 0; --i) {
            const auto shift = 7 - position_ % 8;
            if (shift == 7) {
                bytes_.push_back(0);
            }
            bytes_.back() |= static_cast<std::uint8_t>(((value >> (i - 1)) & 1) << shift);
            ++position_;
        }
    }

    void gamma(std::uint64_t positive) {
        const auto width = static_cast<unsigned>(std::bit_width(positive));
        put(0, width - 1);
        put(positive, width);
    }

    [[nodiscard]] std::uint64_t position() const { return position_; }
    [[nodiscard]] std::vector<std::uint8_t> take_bytes() { return std::move(bytes_); }

private:
    std::uint64_t max_bits_, position_{};
    std::vector<std::uint8_t> bytes_;
};

class BitReader {
public:
    BitReader(std::span<const std::uint8_t> bytes, std::uint64_t count, std::size_t max_bytes)
        : bytes_(bytes), count_(count) {
        if (bytes.size() > max_bytes) {
            throw Error(ErrorCode::limit, "categorical code byte cap exceeded");
        }
        if (count / 8 + (count % 8 != 0) != bytes.size()) {
            throw Error(ErrorCode::invalid, "categorical byte length differs from bit length");
        }
        if (count % 8 && (bytes.back() & ((1U << (8 - count % 8)) - 1))) {
            throw Error(ErrorCode::invalid, "nonzero categorical padding");
        }
    }

    [[nodiscard]] std::uint64_t remaining() const { return count_ - position_; }

    std::uint64_t get(unsigned width) {
        if (width > remaining()) {
            throw Error(ErrorCode::incomplete, "truncated categorical code");
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }

    std::uint64_t gamma() {
        unsigned zeros = 0;
        while (get(1) == 0) {
            if (++zeros == 64) {
                throw Error(ErrorCode::invalid, "categorical integer exceeds u64");
            }
        }
        return (std::uint64_t{1} << zeros) | get(zeros);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::uint64_t count_, position_{};
};

inline void write_symbols(BitWriter& writer, std::span<const TypeId> symbols) {
    for (const auto symbol : symbols) {
        writer.put(symbol, symbol_bits);
    }
}

inline std::vector<TypeId> read_symbols(BitReader& reader, std::size_t n) {
    if (n > reader.remaining() / symbol_bits) {
        throw Error(ErrorCode::incomplete, "truncated categorical symbols");
    }
    std::vector<TypeId> result(n);
    for (auto& symbol : result) {
        symbol = static_cast<TypeId>(reader.get(symbol_bits));
    }
    return result;
}

inline std::size_t read_model_count(BitReader& reader, std::size_t n) {
    const auto model_count = reader.gamma();
    if (model_count > n) {
        throw Error(ErrorCode::invalid, "categorical model count exceeds symbol count");
    }
    return static_cast<std::size_t>(model_count);
}
}
