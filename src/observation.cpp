#include <clab/observation.hpp>

namespace clab {
Snapshot::Snapshot(std::span<const TypeId> symbols) : symbols_(symbols.begin(), symbols.end()) {}
}
