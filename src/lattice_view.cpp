#include <clab/lattice_view.hpp>

#include <algorithm>
#include <unordered_map>

namespace clab {

Snapshot categorical_lattice_view(const RelationalSnapshot& source, std::size_t rows, std::size_t columns,
                                  std::span<const EntityId> row_major_entities) {
    const auto sites = row_major_entities.size();
    if (!rows || !columns || sites / columns != rows || sites % columns ||
        source.entities().size() != sites) {
        throw Error(ErrorCode::invalid, "lattice dimensions and entity counts disagree");
    }
    if (!source.attribute_schema().empty()) {
        throw Error(ErrorCode::invalid, "lattice projection requires an unattributed snapshot");
    }

    std::unordered_map<EntityId, std::size_t> positions;
    positions.reserve(sites);
    for (std::size_t i = 0; i < sites; ++i) {
        if (!positions.emplace(row_major_entities[i], i).second) {
            throw Error(ErrorCode::invalid, "duplicate entity in lattice site map");
        }
    }
    std::vector<TypeId> labels(sites);
    for (const auto& entity : source.entities()) {
        const auto site = positions.find(entity.id);
        if (site == positions.end()) {
            throw Error(ErrorCode::invalid, "lattice site map does not cover all entities");
        }
        labels[site->second] = entity.type;
    }

    constexpr unsigned right = 1, down = 2;
    std::vector<unsigned char> edges(sites);
    for (const auto& relation : source.relations()) {
        if (relation.ordered || relation.type != 0 || relation.port_count != 2) {
            throw Error(ErrorCode::invalid, "lattice edges must be unordered binary type-0 relations");
        }
        const auto ports = source.ports().subspan(relation.first_port, 2);
        if (ports[0].role || ports[1].role) {
            throw Error(ErrorCode::invalid, "lattice edge ports must have role zero");
        }
        auto a = positions.at(ports[0].entity);
        auto b = positions.at(ports[1].entity);
        if (a > b) {
            std::swap(a, b);
        }
        unsigned direction;
        if (b - a == 1 && a % columns != columns - 1) {
            direction = right;
        } else if (b - a == columns) {
            direction = down;
        } else {
            throw Error(ErrorCode::invalid, "relation does not join neighboring lattice sites");
        }
        if (edges[a] & direction) {
            throw Error(ErrorCode::invalid, "duplicate lattice edge");
        }
        edges[a] |= direction;
    }
    for (std::size_t i = 0; i < sites; ++i) {
        const auto expected = (i % columns != columns - 1 ? right : 0) |
                              (i / columns != rows - 1 ? down : 0);
        if (edges[i] != expected) {
            throw Error(ErrorCode::invalid, "missing lattice edge");
        }
    }
    return Snapshot(labels);
}

}
