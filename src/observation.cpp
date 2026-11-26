#include <clab/observation.hpp>

#include <algorithm>

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

Snapshot::Snapshot(std::span<const TypeId> symbols) : symbols_(symbols.begin(), symbols.end()) {}

RelationalSnapshot::RelationalSnapshot(std::span<const EntityRow> entities,
                                       std::span<const RelationRow> relations,
                                       std::span<const PortRow> ports) {
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
}
}
