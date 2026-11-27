# Temporal categorical held-out evaluation

`clab-eval --categorical-temporal TRAIN GUARD TEST SITES TRACE.cltr` fits one [frozen categorical model](E01-categorical-heldout.md) on a window of earlier snapshots, skips a guard, and evaluates a later window. `TRAIN`, `GUARD`, `TEST`, and `SITES` are positive counts. The first three count categorical snapshots and the fourth selects the same leading sites in each scored or training snapshot. Interleaved noncategorical chunks do not count toward the windows.

The trace must declare fixed sites, and its categorical snapshots must have uniform size. Simulated time must increase strictly throughout the selected training, guard and test windows. The guard is measured in recorded categorical snapshots. The CSV reports the observed counts, time unit, and sequence, work and time of the last training snapshot and both test boundaries. This makes the actual separation visible even when observation cadence varies.

The training window concatenates its site prefixes in observation order. The evaluator fits once and uses the same model on every test frame. Each frame pays its own gamma length header. The report sums test headers, test data bits, raw reference bits and unseen symbols, then charges the model once. Conditional gain is `raw_bits - test_bits`. Charged gain also subtracts `model_bits`. The C++ `score_categorical_temporal()` API exposes the serialized model bytes and the same separate costs.

Short windows, too few sites or nonincreasing times give `insufficient_data` with an explicit reason. Traces without categorical snapshots or without fixed sites give `unsupported`. The total training symbols must fit the 4,096-symbol reference cap. The API also applies the symbol limit to each aggregate training and test region and the byte limit to each encoded model or frame. Exceeding a limit gives `budget_exceeded`. The evaluator validates the whole trace before printing a row, including any suffix after the last test frame.

## Verification

The [registered protocol](../protocols/E01-categorical-temporal.json) passed Release on macOS arm64. The independent fixture included two interleaved event chunks. The evaluator correctly identified training end at sequence 2 and test boundaries at sequences 5 and 6. Mutating guard data, unused sites and the unselected tail left the CSV identical.

| Fixture | Model bits | Test header bits | Test data bits | Raw bits | Unseen | Conditional gain | Charged gain |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline test window | 71 | 6 | 38 | 134 | 1 | 90 | 19 |
| First test frame shifted to unseen symbols | 71 | 6 | 103 | 134 | 3 | 25 | −46 |

The first test frame's change added 65 data bits. The last frame's independent expected cost remained 35 data bits. The C++ fixture verified identical serialized models across the two cases. It also exercised the byte budget and checked that a limited run still reached the end of the trace. The offline checks covered missing observations, duplicate times, absent fixed-site metadata, varying shape in the tail and truncated input.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `96cb7635ab40af79508ae4a39170545959a6df12ffd6bb8e8cfdb0d47654a54d` |
| Baseline CSV | `c020fb8efd7ec8389a650d3599b992417bffb5b3cf59fe8d96817df0980a8d2a` |

Run `scripts/check-conformance.sh` to rebuild and verify the installed evaluator. This work verifies the temporal split and exact accounting. Statistical independence, temporal prediction models, structural estimation and scientific calibration remain open.
