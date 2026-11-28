#pragma once

#include <clab/observation.hpp>

namespace clab {

// Project entity types in declared row-major order after validating the full
// open rectangular lattice. Edges must be unordered type-0 relations with
// two role-0 ports. Attributes and other relation semantics are unsupported.
// Invalid layouts throw ErrorCode::invalid.
[[nodiscard]] Snapshot categorical_lattice_view(const RelationalSnapshot& source,
                                                std::size_t rows, std::size_t columns,
                                                std::span<const EntityId> row_major_entities);

}
