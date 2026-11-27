# Frozen categorical motif transfer

`clab-eval --categorical-motif-heldout TRAIN GUARD TEST TRACE.cltr` fits a single contiguous motif on the first `TRAIN` symbols of each categorical snapshot. After skipping `GUARD` symbols, it evaluates the next `TEST` symbols. Training and test sizes are positive and each is capped at 4,096 symbols. The guard may be zero. The scorer ignores any remaining tail. It validates the entire trace, and callers must check its exit status before using the CSV.

Training uses the [exact categorical motif comparison](E01-categorical-motif.md). It admits a dictionary only if its complete training frame beats all five exact training baselines. The same training symbols also fit the [frozen Huffman comparator](E01-categorical-heldout.md). Both models are then fixed. The test encoder uses the leftmost disjoint occurrences of the trained pattern, with any uncovered symbols coded as full u32 values.

| Frozen motif frame | Fields in order |
|---|---|
| Model | Elias gamma of the motif width followed by each motif symbol as a u32 word |
| Test header | Elias gamma of the test length plus one |
| Test placements | Elias gamma of the occurrence count plus one, then Elias gamma of each gap plus one |
| Test residual | The uncovered symbols as u32 words in input order |

Gap lengths count uncovered symbols from the previous placement end, or from the start of the test for the first gap. The format supports zero or one occurrence as well as repeated matches. Bits are written most significant first with zero padding in the last byte. The API takes the exact unpadded bit lengths. The two comparison profiles are named externally, as in the frozen Huffman evaluation, so the test frames need no model selector. `FrozenCategoricalMotif` can restore the model from its bytes and decode the test without access to training.

`conditional_gain_bits` is the Huffman test cost minus the motif test cost. `charged_gain_bits` compares both totals after adding each model's definition once. Negative gains remain in the report. When training provides no admissible motif, the row reports `no_training_motif` with Huffman costs and blank motif and gain fields. A fitted motif with zero test occurrences still gives a valid cost. Short snapshots report `insufficient_data`. Coding limits give `budget_exceeded`, and traces without categorical snapshots give `unsupported`.

## Verification

The [registered protocol](../protocols/E01-categorical-motif-heldout.json) trained on 24 repetitions of a six-symbol motif, each followed by a distinct separator. The matched test repeated the learned order 24 times. The reordered test reversed each repetition and preserved all label counts. Huffman charged the same cost for both tests. The C++ fixtures decoded independent golden model and test bytes and checked malformed lengths, placements and budgets. Changing the guard and tail or injectively renaming the labels preserved the reported costs.

| Test fixture | Motif occurrences | Motif model bits | Motif test bits | Huffman model bits | Huffman test bits | Conditional gain | Charged gain |
|---|---:|---:|---:|---:|---:|---:|---:|
| Matched order | 24 | 197 | 48 | 1,047 | 447 | 399 | 1,249 |
| Reordered, same counts | 0 | 197 | 4,624 | 1,047 | 447 | −4,177 | −3,327 |

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `4ec9f04353228cd77b37b7f9e391fb975d2cd8f462f0a130c26ebdab6a162a44` |
| CSV report | `ba6696823a3bd66d7e5f3ab11d8bc28cbd13907740a55951be1f5506248702f9` |

These synthetic within-snapshot fixtures check transfer and exact accounting for a single learned pattern. The [temporal evaluator](E01-categorical-motif-temporal.md) checks transfer across snapshots. Null calibration and structural scoring remain open.
