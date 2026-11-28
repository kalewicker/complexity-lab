# F12 common view for rectangular lattices

`clab::categorical_lattice_view` projects a relational observation onto the same row-major categorical representation as a dense grid. It is part of `clab::observation` and uses the existing categorical snapshot and CLTR payload. A caller supplies rectangle dimensions and one entity ID for each site, in row-major order. The entity type is the site's categorical state.

The projection validates the entire open rectangle. Each horizontal and vertical nearest-neighbor edge must appear once as an unordered type-0 relation with two role-0 ports. Invalid or incomplete site maps, unmatched topology, and attributed inputs raise `ErrorCode::invalid`. The declared dimensions and site map supply the geometry as side information. This common view admits graph and arity-two hypergraph encodings with the same meaning for their edges. [Protocol](../protocols/E01-F12-rectangular-lattice.json)

## Verification

The fixture records the same seven 2 × 6 frames as a dense grid, a graph, and an arity-two hypergraph. The graph and hypergraph generators use different entity IDs and different traversal and endpoint orders. Both relational traces pass through the trace reader and projection. Under shared output metadata and frame stamps, both projected traces are byte-for-byte equal to the dense reference.

The categorical baseline selected the same family and bit lengths. The frozen two-entry temporal scorer used both motifs on the held-out frames. The dense and projected inputs yielded matching fitted model bytes, comparator costs, placement costs, and held-out lengths. The scorer used four training frames, one guard, and two test frames.

The negative fixtures reject invalid dimensions or site maps, missing and duplicate edges, row wraps, diagonal edges, loops, and unexpected arity, direction, type, role, or attributes. The positive fixtures also cover a single site and both orientations of a two-site line. The installed evaluator-only package exposes the same projection to an external CMake consumer.

This is a bounded F12 result for open rectangular geometry with a known correspondence between entities and sites. Broader common-view and native-representation comparisons remain part of G1. [G1 status](G1-status.md)
