# Anchored permutation test for the second entry

`clab-eval --categorical-motif-dictionary-anchored-permutation TRAIN GUARD TEST SITES ANCHOR_START ANCHOR_WIDTH DRAWS SEED TRACE.cltr` measures marginal second-entry gain conditional on a prespecified site interval. Specify the zero-based start and a width from two to six before scoring. The interval must fit within the scored prefix and leave at least two movable sites. The anchor must contain the same pattern in every selected training frame, with at least two training frames.

The scorer uses the anchored training pattern as the first definition. It places that pattern using the usual leftmost rule across scored sites, including any additional occurrences. The second entry searches the remaining intervals and pays its own definition and placement costs. The pair must beat the anchored first entry and the per-frame training baseline sum. Each model pays its definition once across the scored test window.

One draw applies a shared shuffle to the unanchored indexes across all scored frames. Anchor histories stay at their original sites. The implementation starts from ascending movable indexes, applies the existing SplitMix64 Fisher–Yates shuffle, and writes the shuffled indexes into those same positions. It refits the second entry and Huffman on each draw. The output records the anchor and uses method ID `categorical.two_motif_anchored_site_permutation.v1`.

The reported statistic is the positive part of charged gain over the anchored first entry. Absence contributes zero, ties count toward the rank, and any failed draw suppresses the rank. The [registered protocol](../protocols/E01-categorical-motif-dictionary-anchored-permutation.json) requires joint exchangeability of the movable histories conditional on the fixed histories and the prespecified anchor.

## Verification

The fixture has two six-symbol blocks, with the later block anchored at sites 8–13. Fixing it as the first definition made the earlier block the added entry. The added entry saved 367 bits and ranked 1/20. Reversing that entry in the test window preserved the anchored pattern and yielded a charged gain of −200 bits and a rank of 20/20.

Independent replay matched all 19 draw gains and verified that anchor histories and the multiset of movable histories were preserved. Guard and tail edits preserved the CSV. Out-of-range anchors and inconsistent anchored training patterns were rejected.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `8dfe3f16fdd14a2b8d381b23cc716d2aff4b68e2d9fe826f6d074d269a26f817` |
| CSV report | `f5ab7b5b4622eec6c28f04beb102608911d3e4b95e93540cae135e66b7ac63b8` |

Population calibration for the conditional test remains open.
