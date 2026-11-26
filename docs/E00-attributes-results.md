# E00 relational attributes

The [registered protocol](../protocols/E00-attributes.json) covers categorical and continuous attributes attached to entity and relation rows. The independent Release fixture check passed on macOS arm64.

| Check | Observed result |
|---|---|
| Independent reader | In a two-entity, one-relation graph, the inspector emitted five slots exactly. It preserved two category codes, two continuous values and one missing slot. The empty graph emitted no slots. |
| Schema | Metadata inspection preserved column order, owner kinds, category labels, units, inclusive bounds and declared resolutions. |
| Writer | Typed C++ values and the schema round-tripped. The writer rejected a snapshot with different schema semantics, and the snapshot constructor rejected a real value in a categorical slot. |
| Rejection | All 25 malformed schema and payload cases failed before their chunks entered the index. These included malformed lengths, an optional flag on version 2, category-code overflow, missing required values, and nonfinite and out-of-bounds reals. |
| Compatibility | Topology diagnostics were byte-identical for the attributed fixture and a version 1 trace with the same topology. Earlier fixture hashes remained valid. The previous reader rejected the version 2 trace written by the installed external consumer as an unknown required version. |

The independently encoded trace SHA-256 is `3945b8c6da462673f23ec9b302d9b65c2b1132d7a0cdab7d6625c0dfad27dd1d`. The Python fixture generator computes the digest and checks the visible slot values, schema and chunk headers. The [wire specification](cltr-v1.md) defines version 2 and the [CI matrix](../.github/workflows/conformance.yml) reruns the checks on all supported hosts.

This slice covers categorical and continuous entity and relation attributes. Port-level attributes, other native encodings, and use of attributes in scientific estimators remain outside the check. G0 is still open pending the remaining conformance audit and schema freeze.
