#include <clab/observation.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

namespace clab {
namespace {
template<class Row>
auto validated_ids(std::span<const Row> rows) {
    std::vector<std::uint64_t> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) {
        ids.push_back(row.id);
    }
    std::ranges::sort(ids);
    if ((!ids.empty() && ids.front() == 0) || std::ranges::adjacent_find(ids) != ids.end()) {
        throw Error(ErrorCode::invalid, "duplicate or zero relational ID");
    }
    return ids;
}
}

void validate_attribute_schema(std::span<const AttributeDescriptor> schema) {
    std::set<std::pair<AttributeOwner, std::string_view>> names;
    for (const auto& field : schema) {
        if ((field.owner != AttributeOwner::entity && field.owner != AttributeOwner::relation) ||
            field.name.empty() || !names.emplace(field.owner, field.name).second) {
            throw Error(ErrorCode::invalid, "invalid or duplicate relational attribute name");
        }
        if (const auto* categorical = std::get_if<CategoricalAttribute>(&field.domain)) {
            std::set<std::string_view> labels;
            for (const auto& label : categorical->labels) {
                if (label.empty() || !labels.insert(label).second) {
                    throw Error(ErrorCode::invalid, "invalid or duplicate attribute category");
                }
            }
            if (labels.empty()) {
                throw Error(ErrorCode::invalid, "attribute has no categories");
            }
        } else {
            const auto& real = std::get<ContinuousAttribute>(field.domain);
            if (real.unit.empty() || !std::isfinite(real.lower) || !std::isfinite(real.upper) ||
                real.lower > real.upper || !std::isfinite(real.resolution) || real.resolution <= 0) {
                throw Error(ErrorCode::invalid, "invalid continuous attribute domain");
            }
        }
    }
}

Snapshot::Snapshot(std::span<const TypeId> symbols) : symbols_(symbols.begin(), symbols.end()) {}

RelationalSnapshot::RelationalSnapshot(std::span<const EntityRow> entities,
                                       std::span<const RelationRow> relations,
                                       std::span<const PortRow> ports,
                                       std::span<const AttributeColumn> attributes,
                                       std::span<const AttributeDescriptor> schema) {
    const auto entity_ids = validated_ids(entities);
    (void)validated_ids(relations);
    std::size_t next_port = 0;
    for (const auto& row : relations) {
        if (row.first_port != next_port || row.port_count > ports.size() - next_port) {
            throw Error(ErrorCode::invalid, "invalid relational port range");
        }
        next_port += row.port_count;
    }
    if (next_port != ports.size()) {
        throw Error(ErrorCode::invalid, "unassigned relational ports");
    }
    for (const auto& port : ports) {
        if (!std::ranges::binary_search(entity_ids, port.entity)) {
            throw Error(ErrorCode::invalid, "dangling relational port");
        }
    }
    entities_.assign(entities.begin(), entities.end());
    relations_.assign(relations.begin(), relations.end());
    ports_.assign(ports.begin(), ports.end());
    attributes_.assign(attributes.begin(), attributes.end());
    validate_attribute_schema(schema);
    schema_.assign(schema.begin(), schema.end());
    validate_attributes();
}

void RelationalSnapshot::validate_attributes() const {
    if (attributes_.size() != schema_.size()) {
        throw Error(ErrorCode::invalid, "relational attribute column count differs from schema");
    }
    for (std::size_t i = 0; i < schema_.size(); ++i) {
        const auto& field = schema_[i];
        const auto& column = attributes_[i];
        const auto rows = field.owner == AttributeOwner::entity ? entities_.size() : relations_.size();
        if (column.size() != rows) {
            throw Error(ErrorCode::invalid, "attribute row count differs from owner count");
        }
        for (const auto& slot : column) {
            if (!slot) {
                if (field.required) {
                    throw Error(ErrorCode::invalid, "required relational attribute is missing");
                }
                continue;
            }
            if (const auto* categorical = std::get_if<CategoricalAttribute>(&field.domain)) {
                const auto* code = std::get_if<std::uint32_t>(&*slot);
                if (!code || *code >= categorical->labels.size()) {
                    throw Error(ErrorCode::invalid, "unknown relational attribute category");
                }
            } else {
                const auto& real = std::get<ContinuousAttribute>(field.domain);
                const auto* value = std::get_if<double>(&*slot);
                if (!value || !std::isfinite(*value) || *value < real.lower || *value > real.upper) {
                    throw Error(ErrorCode::invalid, "invalid continuous attribute value");
                }
            }
        }
    }
}

EventBatch::EventBatch(std::span<const Event> events) {
    std::optional<WorkCount> previous;
    for (const auto& event : events) {
        if ((previous && event.work < *previous) ||
            (event.rule_id && *event.rule_id == 0) ||
            (event.provenance != EventProvenance::observed &&
             event.provenance != EventProvenance::reconstructed)) {
            throw Error(ErrorCode::invalid, "invalid event stamp, rule ID or provenance");
        }
        previous = event.work;
        for (const auto* list : {&event.participants, &event.created, &event.deleted}) {
            auto ids = *list;
            std::ranges::sort(ids);
            if ((!ids.empty() && ids.front() == 0) || std::ranges::adjacent_find(ids) != ids.end()) {
                throw Error(ErrorCode::invalid, "duplicate or zero event entity ID");
            }
        }
        auto created = event.created;
        std::ranges::sort(created);
        if (std::ranges::any_of(event.deleted, [&](EntityId id) {
                return std::ranges::binary_search(created, id);
            })) {
            throw Error(ErrorCode::invalid, "event creates and deletes the same entity");
        }
    }
    events_.assign(events.begin(), events.end());
}
}
