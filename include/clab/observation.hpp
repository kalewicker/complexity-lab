#pragma once

#include <clab/foundation.hpp>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace clab {
struct Stamp {
    std::uint64_t sequence{};
    double simulated_time{};
    WorkCount work{};
    bool operator==(const Stamp&) const = default;
};

struct Metadata {
    std::string run_id;
    std::string encoding;
    std::string time_unit;
    bool exact{};
    bool fixed_sites{};
    bool operator==(const Metadata&) const = default;
};

class Snapshot {
public:
    // Copy the input so views from the caller cannot mutate the observation.
    explicit Snapshot(std::span<const TypeId> symbols);
    [[nodiscard]] std::span<const TypeId> symbols() const & { return symbols_; }
    std::span<const TypeId> symbols() const && = delete;

private:
    std::vector<TypeId> symbols_;
};

struct EventCounts {
    std::uint64_t events{}, births{}, deaths{};
    bool operator==(const EventCounts&) const = default;
};

struct WorkLedger {
    std::uint64_t logical_updates{}, reads{}, writes{}, rng_draws{};
    std::uint64_t live_state_bytes{}, peak_state_bytes{};
    bool operator==(const WorkLedger&) const = default;
};

using Payload = std::variant<Snapshot, EventCounts, WorkLedger>;
struct ObservationRecord {
    Stamp stamp;
    Payload payload;
};

class IObservationSource {
public:
    virtual ~IObservationSource() = default;
    [[nodiscard]] virtual const Metadata& metadata() const noexcept = 0;
    // Empty means end of stream. Failed reads throw a typed Error.
    virtual std::optional<ObservationRecord> next() = 0;
};
}
