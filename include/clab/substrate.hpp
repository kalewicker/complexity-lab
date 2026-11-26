#pragma once

#include <clab/observation.hpp>

namespace clab {
struct BinaryCheckpoint {
    std::vector<TypeId> cells;
    WorkCount sweeps{};
    std::uint64_t rng_state{};
    bool operator==(const BinaryCheckpoint&) const = default;
};

class BinaryCa {
public:
    // Rule 90 on a periodic ring. One unit of reference work is one sweep.
    BinaryCa(std::size_t sites, std::uint64_t seed, std::uint64_t memory_cap);
    [[nodiscard]] Snapshot observe() const;
    [[nodiscard]] BinaryCheckpoint checkpoint() const;
    void restore(const BinaryCheckpoint& checkpoint);
    [[nodiscard]] bool advance(WorkCount hard_work_cap);
    [[nodiscard]] WorkLedger ledger() const;

private:
    std::vector<TypeId> cells_, next_;
    Rng rng_;
    WorkCount sweeps_{};
};
}
