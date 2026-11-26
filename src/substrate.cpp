#include <clab/substrate.hpp>

#include <algorithm>

namespace clab {
BinaryCa::BinaryCa(std::size_t sites, std::uint64_t seed, std::uint64_t memory_cap) : rng_(seed) {
    if (sites == 0) {
        throw Error(ErrorCode::invalid, "CA ring must have at least one site");
    }
    if (checked_multiply(sites, 2 * sizeof(TypeId)) > memory_cap) {
        throw Error(ErrorCode::limit, "CA cell buffer cap exceeded");
    }
    cells_.resize(sites);
    next_.resize(sites);
    for (auto& cell : cells_) {
        cell = static_cast<TypeId>(rng_.next() & 1);
    }
}

Snapshot BinaryCa::observe() const { return Snapshot(cells_); }
BinaryCheckpoint BinaryCa::checkpoint() const { return {cells_, sweeps_, rng_.state()}; }

void BinaryCa::restore(const BinaryCheckpoint& checkpoint) {
    if (checkpoint.cells.size() != cells_.size() ||
        std::ranges::any_of(checkpoint.cells, [](auto cell) { return cell > 1; })) {
        throw Error(ErrorCode::invalid, "incompatible binary CA checkpoint");
    }
    const auto max_count = checked_multiply(checked_multiply(cells_.size(), 2), checkpoint.sweeps);
    (void)max_count;
    std::ranges::copy(checkpoint.cells, cells_.begin());
    sweeps_ = checkpoint.sweeps;
    rng_ = Rng(checkpoint.rng_state);
}

bool BinaryCa::advance(WorkCount hard_work_cap) {
    if (sweeps_ >= hard_work_cap) {
        return false;
    }
    const auto next_sweep = checked_add(sweeps_, 1);
    const auto reads = checked_multiply(checked_multiply(cells_.size(), 2), next_sweep);
    (void)reads;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const auto left = i == 0 ? cells_.size() - 1 : i - 1;
        const auto right = i + 1 == cells_.size() ? 0 : i + 1;
        next_[i] = cells_[left] ^ cells_[right];
    }
    cells_.swap(next_);
    sweeps_ = next_sweep;
    return true;
}

WorkLedger BinaryCa::ledger() const {
    const auto writes = checked_multiply(sweeps_, cells_.size());
    const auto live = checked_multiply(cells_.size(), 2 * sizeof(TypeId));
    return {sweeps_, checked_multiply(writes, 2), writes, cells_.size(), live, live};
}
}
