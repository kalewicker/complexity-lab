#include <clab/foundation.hpp>

namespace clab {
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        throw Error(ErrorCode::limit, "unsigned addition overflow");
    }
    return a + b;
}

std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) {
        throw Error(ErrorCode::limit, "unsigned multiplication overflow");
    }
    return a * b;
}

// SplitMix64 with intentionally wrapping unsigned arithmetic.
std::uint64_t Rng::next() noexcept {
    state_ += 0x9e3779b97f4a7c15ULL;
    auto z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
}
