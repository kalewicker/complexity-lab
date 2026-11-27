#include <clab/categorical_code.hpp>

#include "categorical_bits.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace clab {
namespace {
using namespace categorical_detail;

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
