#pragma once

#include <clab/foundation.hpp>

#include <array>
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

enum class TimeDomain { discrete, continuous };

struct ObservationCapabilities {
    bool native{}, relational{}, temporal{};
    bool stable_entity_ids{}, implicit_relations{}, interventions{};
    TimeDomain time_domain{TimeDomain::discrete};
    bool operator==(const ObservationCapabilities&) const = default;
};

struct TraceProvenance {
    std::string config_sha256;
    std::optional<std::uint64_t> seed;
    std::string producer;
    std::string backend;
    std::string precision;
    ObservationCapabilities capabilities;
    bool operator==(const TraceProvenance&) const = default;
};

enum class AttributeOwner { entity, relation };

struct CategoricalAttribute {
    std::vector<std::string> labels;
    bool operator==(const CategoricalAttribute&) const = default;
};

struct ContinuousAttribute {
    std::string unit;
    double lower{}, upper{}, resolution{};
    bool operator==(const ContinuousAttribute&) const = default;
};

struct AttributeDescriptor {
    AttributeOwner owner{};
    std::string name;
    bool required{};
    std::variant<CategoricalAttribute, ContinuousAttribute> domain;
    bool operator==(const AttributeDescriptor&) const = default;
};

using AttributeValue = std::variant<std::uint32_t, double>;
using AttributeColumn = std::vector<std::optional<AttributeValue>>;

void validate_attribute_schema(std::span<const AttributeDescriptor> schema);

struct Metadata {
    std::string run_id;
    std::string encoding;
    std::string time_unit;
    bool exact{};
    bool fixed_sites{};
    std::optional<TraceProvenance> provenance{};
    std::vector<AttributeDescriptor> relational_attributes{};
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

using EntityId = std::uint64_t;
using RelationId = std::uint64_t;

struct EntityRow {
    EntityId id{};
    TypeId type{};
    bool operator==(const EntityRow&) const = default;
};

struct RelationRow {
    RelationId id{};
    TypeId type{};
    std::uint32_t first_port{}, port_count{};
    bool ordered{};
    bool operator==(const RelationRow&) const = default;
};

struct PortRow {
    EntityId entity{};
    std::uint32_t role{};
    bool operator==(const PortRow&) const = default;
};

class RelationalSnapshot {
public:
    RelationalSnapshot(std::span<const EntityRow> entities,
                       std::span<const RelationRow> relations,
                       std::span<const PortRow> ports,
                       std::span<const AttributeColumn> attributes = {},
                       std::span<const AttributeDescriptor> schema = {});
    [[nodiscard]] std::span<const EntityRow> entities() const & { return entities_; }
    [[nodiscard]] std::span<const RelationRow> relations() const & { return relations_; }
    [[nodiscard]] std::span<const PortRow> ports() const & { return ports_; }
    std::span<const EntityRow> entities() const && = delete;
    std::span<const RelationRow> relations() const && = delete;
    std::span<const PortRow> ports() const && = delete;
    [[nodiscard]] std::span<const AttributeColumn> attributes() const & { return attributes_; }
    std::span<const AttributeColumn> attributes() const && = delete;
    [[nodiscard]] std::span<const AttributeDescriptor> attribute_schema() const & { return schema_; }
    std::span<const AttributeDescriptor> attribute_schema() const && = delete;

private:
    std::vector<EntityRow> entities_;
    std::vector<RelationRow> relations_;
    std::vector<PortRow> ports_;
    std::vector<AttributeColumn> attributes_;
    std::vector<AttributeDescriptor> schema_;

    void validate_attributes() const;
};

struct EventCounts {
    std::uint64_t events{}, births{}, deaths{};
    bool operator==(const EventCounts&) const = default;
};

enum class EventProvenance { observed, reconstructed };

struct Event {
    WorkCount work{};
    TypeId type{};
    EventProvenance provenance{EventProvenance::observed};
    std::optional<std::uint64_t> rule_id;
    std::vector<EntityId> participants, created, deleted;
    std::vector<std::uint8_t> delta;
    bool operator==(const Event&) const = default;
};

class EventBatch {
public:
    explicit EventBatch(std::span<const Event> events);
    [[nodiscard]] std::span<const Event> events() const & { return events_; }
    std::span<const Event> events() const && = delete;

private:
    std::vector<Event> events_;
};

struct CheckpointRef {
    std::string format;
    std::uint64_t size_bytes{};
    std::array<std::uint8_t, 32> sha256{};
    bool operator==(const CheckpointRef&) const = default;
};

struct WorkLedger {
    std::uint64_t logical_updates{}, reads{}, writes{}, rng_draws{};
    std::uint64_t live_state_bytes{}, peak_state_bytes{};
    bool operator==(const WorkLedger&) const = default;
};

using Payload = std::variant<Snapshot, EventCounts, WorkLedger, RelationalSnapshot,
                             EventBatch, CheckpointRef>;
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
