#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace clab {
using WorkCount = std::uint64_t;
using TypeId = std::uint32_t;

enum class ErrorCode { invalid, unsupported, limit, incomplete, io };

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

[[nodiscard]] std::uint64_t checked_add(std::uint64_t a, std::uint64_t b);
[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b);

class Rng {
public:
    explicit Rng(std::uint64_t state) : state_(state) {}
    [[nodiscard]] std::uint64_t next() noexcept;
    [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

private:
    std::uint64_t state_;
};
}
