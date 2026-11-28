# F11 categorical label invariance

Categorical code lengths, motif placement coordinates, and permutation outcomes agreed exactly under three bijections over the 32-bit label space. The mappings reverse numeric order, toggle the high bit, and reorder labels via multiplication by an odd constant modulo 2³². One mapping sends label 1 to zero. Site order, frame order, and the permutation seed are fixed. [Protocol](../protocols/E01-F11-categorical-relabeling.json)

| Fixture | CLI modes | Relabelings | Exact CSV matches |
|---|---:|---:|---:|
| Two persistent motifs with fresh nuisance labels | 12 | 3 | 36 / 36 |
| Globally unique labels with no training motif | 11 | 3 | 33 / 33 |

The comparisons include all four baseline families and IID, baseline selection, one- and two-entry motif fitting, temporal costs, and all applicable permutation statistics and draw gains. In the unique-label fixture, the two-entry scorer reported no training dictionary and the frozen Huffman scorer counted 72 unseen test symbols under every mapping. The anchored permutation check used the same site interval across the three relabelings. [Trace and CSV digests](evidence/E01-F11-categorical-relabeling-v1.json)

The C++ checks additionally inspected the exact fitted patterns. In the tied single-entry fixture, the chosen pattern was the relabeling of `[9, 1]`. In the interleaved snapshot and the frame-aware fit, each pattern mapped to its counterpart in the same entry order, with identical placement coordinates and code lengths. This checks motif identity directly because equal bit lengths can occur for different models.

This covers categorical label bijections on the declared fixtures and advances the categorical portion of F11. The relational Tier-0 fixture separately covers entity IDs and insertion order. Cross-representation scoring, site reorderings, byte-level diagnostics, and the wider G1 invariance requirements have separate scopes. [Tier-0 fixtures](E01-tier0-fixtures.md) · [G1 status](G1-status.md)
