# Temporal motif permutation test

`clab-eval --categorical-motif-permutation TRAIN GUARD TEST SITES DRAWS SEED TRACE.cltr` runs the [temporal motif pipeline](E01-categorical-motif-temporal.md) on the observed window and on shuffled copies. It accepts 1–199 draws and an unsigned 64-bit decimal seed, including zero. Fix the seed, draw count, window and site prefix before scoring.

Each draw starts from the original prefixes and independently permutes the positions within each training frame and then within each test frame. This preserves the exact label counts of every frame and keeps boundaries, guard separation and metadata fixed. The evaluator refits both models and reruns motif admission for every shuffled training window. The corresponding shuffled test windows use those frozen models.

The generator uses Fisher–Yates with one SplitMix64 stream initialized by `SEED`. It walks training frames before test frames in each draw and visits positions from last to first. For a remaining prefix of length `k`, it discards words below `2^64 mod k` before reducing the next accepted word modulo `k`. The stream continues across draws. These details allow independent replay without relying on a platform shuffle implementation.

## Statistic and report

The statistic is the positive part of model-charged gain in bits. A completed split that admits no training motif contributes zero. The one-sided rank is `(1 + number of draw statistics at least as large as observed) / (DRAWS + 1)`, counting ties. The CSV reports the exact numerator and denominator, the observed costs and statistic, and the draw counts. `draw_gains_bits` holds one token per draw in generation order, using the signed charged gain, `absent` for a completed draw with no admitted motif, or `budget_exceeded` for a failed draw. Any failed draw leaves the rank blank. The scorer still attempts the full scheduled draw count. An incomplete observed split starts zero draws.

This is a null for joint invariance under independent within-frame site permutations. Such permutations remove spatial arrangement and can disrupt temporal correspondence at fixed sites. Use the rank as a descriptive comparison when the trace does not support that exchangeability assumption. The smallest possible rank with 19 draws is 1/20.

## Verification

The [registered engineering fixture](../protocols/E01-categorical-motif-permutation.json) placed the pair `[1, 2]` in every training and test prefix. The observed charged gain was 1,392 bits. The 19 draws included 12 fitted motifs with gains between 1,270 and 1,331 bits and seven completed draws with no motif. The report gave 1/20. Independent Python shuffling preserved every prefix histogram, reproduced all 19 draw tokens and matched the rank. Changing guard data, unused sites and the tail preserved the full CSV.

Controls with no admissible motif and with a negative charged gain both produced ties and a rank of 20/20. A reduced-byte-budget C++ fixture retained failed draws and left the rank unavailable. The fixture results establish replay and accounting for this null. The [calibration design and runner](E01-categorical-motif-calibration.md) define the population check. Population results and structural scoring remain open.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `98db41acff2a88c5c4661e40bf4f7cda950386c7d04ac36472f121a026111e8f` |
| CSV report | `c77ea4f4cfcf3ebbef6da37325e3f5cb038b394871c02f6d9b5f368bb98e8e16` |
