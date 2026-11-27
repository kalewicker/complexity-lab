# Held-out two-entry motif dictionary

`clab-eval --categorical-motif-dictionary-heldout TRAIN GUARD TEST TRACE.cltr` fits a dictionary on the first `TRAIN` symbols of each categorical snapshot. It skips `GUARD` symbols, scores the next `TEST` symbols, and ignores the tail. The guard may be zero. Training and test counts are positive and each capped at 4,096. The command consumes the whole trace, and callers must check its exit status before using the CSV.

The [exact training comparison](E01-categorical-motif-dictionary.md) admits the pair only when its complete training code beats both the best one-entry candidate and the best of the five exact baselines. The pair's definitions and their order then freeze. The evaluator compares three models on the same test symbols.

| Model | Training and test treatment |
|---|---|
| Frozen pair | Keep both admitted patterns. Place the first leftmost and disjoint, then the second within the remaining contiguous intervals |
| First entry alone | Keep the pair's first pattern and the same first-entry placements. Charge its first definition once and encode the remaining symbols literally |
| Frozen Huffman | Fit on the same training symbols and score the same test sequence |

The first-entry comparison measures the marginal effect of adding the second definition. This comparison is available whenever a pair was admitted, including when the one-entry candidate alone would have lost the training gate. Conditional gain subtracts the pair test bits from the corresponding comparator test bits. Charged gain adds each model's own definition cost once before taking that difference. Signed gains and zero occurrence counts remain visible. When no pair is admitted, the row reports `no_training_dictionary` and retains Huffman costs.

## Model and test bits

`categorical.frozen_two_motif.v1` writes the two definitions in order as gamma-coded widths followed by u32 symbols. The test frame begins with Elias gamma of the test length plus one. Each entry gets gamma of its own occurrence count plus one, followed by gamma of each gap plus one in original coordinates. A gap starts at the previous end of the same entry, or at the start of the test for its first placement. The uncovered u32 literals follow both placement streams. The externally named profile supplies the model identity. The fields are MSB first with zero padding excluded from the exact bit count.

The C++ API `FrozenCategoricalMotifDictionary` fits, serializes and restores the model, exposes its first entry, and encodes or decodes a test frame. The test decoder accepts zero or one occurrence per entry and checks bounds and overlap across entries.

## Verification

The [registered fixtures](../protocols/E01-categorical-motif-dictionary-heldout.json) used two orderings of the same six labels separated by distinct literals during training. Three held-out layouts used identical histograms. Huffman cost 1,047 model bits and 447 test bits in all three. The pair paid 394 model bits and the first entry alone paid 197.

| Test layout | First matches | Second matches | First test bits | Pair test bits | Charged gain vs. first | Charged gain vs. Huffman |
|---|---:|---:|---:|---:|---:|---:|
| Both trained orderings | 12 | 12 | 2,382 | 145 | 2,040 | 955 |
| First ordering only | 24 | 0 | 48 | 49 | −198 | 1,051 |
| Second ordering only | 0 | 24 | 4,624 | 49 | 4,378 | 1,051 |

The first-order-only control still beat Huffman. Its negative charged gain against the first entry isolates the cost of retaining an unused second pattern. Guard and tail mutations and injective relabeling preserved the costs. The decoder tests checked exact independent model and test bytes, overlapping candidate priority, malformed placements, duplicate definitions, and budgets.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `151eb33bedc23b189cc2b633649c5050dd931af57dd6b18f1e30d053a734c492` |
| CSV report | `f44490f9cd7d40de21ed36c5885fd70e1ed8b101f7cc185a83cb3b90d3006767` |

The result covers synthetic within-snapshot transfer. The [temporal dictionary test](E01-categorical-motif-dictionary-temporal.md) scores later snapshots across a guard window. Null calibration and structural scoring for the two-entry dictionary remain open.
