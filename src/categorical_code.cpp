#include <clab/categorical_code.hpp>

#include <algorithm>
#include <bit>
#include <utility>

namespace clab {
namespace {
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

std::size_t shortest_period(std::span<const TypeId> symbols) {
    // The longest proper prefix that is also a suffix determines the finite period.
    std::vector<std::size_t> prefix(symbols.size());
    for (std::size_t i = 1; i < symbols.size(); ++i) {
        auto matched = prefix[i - 1];
        while (matched && symbols[i] != symbols[matched]) {
            matched = prefix[matched - 1];
        }
        prefix[i] = matched + (symbols[i] == symbols[matched]);
    }
    return symbols.size() - prefix.back();
}

void write_symbols(BitWriter& writer, std::span<const TypeId> symbols) {
    for (const auto symbol : symbols) {
        writer.put(symbol, symbol_bits);
    }
}

std::vector<TypeId> read_symbols(BitReader& reader, std::size_t n) {
    if (n > reader.remaining() / symbol_bits) {
        throw Error(ErrorCode::incomplete, "truncated categorical symbols");
    }
    std::vector<TypeId> result(n);
    for (auto& symbol : result) {
        symbol = static_cast<TypeId>(reader.get(symbol_bits));
    }
    return result;
}

std::size_t read_model_count(BitReader& reader, std::size_t n) {
    const auto model_count = reader.gamma();
    if (model_count > n) {
        throw Error(ErrorCode::invalid, "categorical model count exceeds symbol count");
    }
    return static_cast<std::size_t>(model_count);
}
}

std::string_view categorical_code_id(CategoricalCode code) {
    switch (code) {
    case CategoricalCode::raw: return "categorical.raw.v1";
    case CategoricalCode::uniform: return "categorical.uniform.v1";
    case CategoricalCode::run_length: return "categorical.run_length.v1";
    case CategoricalCode::periodic: return "categorical.periodic.v1";
    }
    throw Error(ErrorCode::invalid, "unknown categorical code");
}

std::uint64_t CodeLength::total_bits() const {
    return checked_add(checked_add(header_bits, model_bits), data_bits);
}

EncodedCategorical encode_categorical(std::span<const TypeId> symbols, CategoricalCode code,
                                       CodeLimits limits) {
    (void)categorical_code_id(code);
    if (symbols.size() > limits.symbols) {
        throw Error(ErrorCode::limit, "categorical symbol cap exceeded");
    }

    BitWriter writer(limits.encoded_bytes);
    writer.put(static_cast<std::uint8_t>(code), 2);
    writer.gamma(checked_add(symbols.size(), 1));
    CodeLength length{writer.position()};
    if (!symbols.empty()) {
        switch (code) {
        case CategoricalCode::raw:
            write_symbols(writer, symbols);
            break;
        case CategoricalCode::uniform: {
            std::vector<TypeId> alphabet(symbols.begin(), symbols.end());
            std::ranges::sort(alphabet);
            alphabet.erase(std::unique(alphabet.begin(), alphabet.end()), alphabet.end());
            writer.gamma(alphabet.size());
            write_symbols(writer, alphabet);
            length.model_bits = writer.position() - length.header_bits;
            const auto width = static_cast<unsigned>(std::bit_width(alphabet.size() - 1));
            for (const auto symbol : symbols) {
                const auto index = std::ranges::lower_bound(alphabet, symbol) - alphabet.begin();
                writer.put(static_cast<std::uint64_t>(index), width);
            }
            break;
        }
        case CategoricalCode::run_length: {
            std::size_t start = 0;
            while (start < symbols.size()) {
                std::size_t end = start + 1;
                while (end < symbols.size() && symbols[end] == symbols[start]) {
                    ++end;
                }
                writer.put(symbols[start], symbol_bits);
                writer.gamma(end - start);
                start = end;
            }
            break;
        }
        case CategoricalCode::periodic: {
            const auto period = shortest_period(symbols);
            writer.gamma(period);
            write_symbols(writer, symbols.first(period));
            length.model_bits = writer.position() - length.header_bits;
            break;
        }
        }
    }
    length.data_bits = writer.position() - length.header_bits - length.model_bits;
    return {writer.take_bytes(), length};
}

std::vector<TypeId> decode_categorical(std::span<const std::uint8_t> bytes,
                                       std::uint64_t bit_count, CodeLimits limits) {
    BitReader reader(bytes, bit_count, limits.encoded_bytes);
    const auto code = static_cast<CategoricalCode>(reader.get(2));
    const auto count = reader.gamma() - 1;
    if (count > limits.symbols) {
        throw Error(ErrorCode::limit, "categorical symbol cap exceeded");
    }
    const auto n = static_cast<std::size_t>(count);
    std::vector<TypeId> result;
    if (n != 0) {
        switch (code) {
        case CategoricalCode::raw:
            result = read_symbols(reader, n);
            break;
        case CategoricalCode::uniform: {
            const auto k = read_model_count(reader, n);
            const auto alphabet = read_symbols(reader, k);
            if (!std::ranges::is_sorted(alphabet) ||
                std::ranges::adjacent_find(alphabet) != alphabet.end()) {
                throw Error(ErrorCode::invalid, "categorical alphabet must be strictly increasing");
            }
            const auto width = static_cast<unsigned>(std::bit_width(k - 1));
            if (checked_multiply(n, width) > reader.remaining()) {
                throw Error(ErrorCode::incomplete, "truncated categorical indices");
            }
            result.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                const auto index = reader.get(width);
                if (index >= k) {
                    throw Error(ErrorCode::invalid, "categorical index outside alphabet");
                }
                result.push_back(alphabet[static_cast<std::size_t>(index)]);
            }
            break;
        }
        case CategoricalCode::run_length:
            while (result.size() < n) {
                const auto symbol = static_cast<TypeId>(reader.get(symbol_bits));
                const auto run = reader.gamma();
                if (run > n - result.size() || (!result.empty() && result.back() == symbol)) {
                    throw Error(ErrorCode::invalid, "invalid categorical run");
                }
                result.insert(result.end(), static_cast<std::size_t>(run), symbol);
            }
            break;
        case CategoricalCode::periodic: {
            const auto period = read_model_count(reader, n);
            result = read_symbols(reader, period);
            result.reserve(n);
            while (result.size() < n) {
                const auto symbol = result[result.size() % period];
                result.push_back(symbol);
            }
            break;
        }
        }
    }
    if (reader.remaining()) {
        throw Error(ErrorCode::invalid, "trailing categorical bits");
    }
    return result;
}
}
