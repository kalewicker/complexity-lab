# Shared-site permutation test for the second entry

`clab-eval --categorical-motif-dictionary-site-permutation TRAIN GUARD TEST SITES DRAWS SEED TRACE.cltr` runs the [two-entry temporal comparison](E01-categorical-motif-dictionary-temporal.md) on the observed split and on shared-site shuffles. On each draw it applies one permutation to every scored site prefix, preserving all scored site histories and every frame histogram. It refits the dictionary and Huffman on the shuffled training frames and checks training admission again. Each admitted pair and its own first entry then encode the same shuffled test frames.

The statistic is the positive part of the charged gain against the first entry alone. To calculate the signed gain, add model and test bits for the first entry, then subtract model and test bits for the pair. A completed split without a training pair contributes zero. The comparison with Huffman stays visible on the observed row. The rank uses the marginal gain against the first entry.

The [registered engineering check](../protocols/E01-categorical-motif-dictionary-site-permutation.json) uses the same Fisher–Yates shuffle, SplitMix64 stream, and 1–199 draw limit as the [one-entry shared-site test](E01-categorical-motif-site-permutation.md). The exact rank is `(1 + number of draw statistics at least as large as observed) / (DRAWS + 1)`, including ties. The report names the method `categorical.two_motif_shared_site_permutation.v1`. The `draw_gains_bits` field retains the signed marginal gain for each draw, `absent` for a completed draw without a pair, and `budget_exceeded` for a failed draw. Any failed draw leaves the rank blank. Missing observed data starts zero draws.

Inference requires joint exchangeability of whole site histories. Both spatial patterns can change under permutation because the training procedure runs afresh on every draw. A null that conditions on a prespecified spatial first motif would require its own design and validation.

## Verification

The first fixture placed two persistent six-symbol motifs among 24 varying labels per scored frame. The observed second entry saved 349 bits after model costs. The 18 shuffled draws that admitted a pair gained 83–159 bits, and one draw admitted no pair. The resulting rank was 1/20. Independent replay matched all 19 draw tokens and verified preservation of every scored history.

The first-entry-only control lost 199 bits by retaining the second definition. All 19 signed draw gains also equaled −199, giving a rank of 20/20 at statistic zero. A no-pair control also ranked 20/20. The tests checked guard and tail isolation, short windows, malformed traces, and retention of scheduled draws after a budget failure.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `8dfe3f16fdd14a2b8d381b23cc716d2aff4b68e2d9fe826f6d074d269a26f817` |
| CSV report | `369249365f5983a4d24a62d69cabec129b2319d091e9e7f8698b49728bf3123f` |

The fixtures verify the test and its accounting. The [registered calibration results](E01-categorical-motif-dictionary-site-calibration.md#registered-panel-results) give false-positive and sensitivity bounds for this statistic under one joint site-exchangeability null and one two-block alternative.
