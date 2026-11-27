# Frozen categorical held-out evaluation

`clab-eval --categorical-heldout TRAIN GUARD TEST TRACE.cltr` applies the same split to each categorical snapshot in serialized symbol order. The first `TRAIN` symbols fit the model. The next `GUARD` symbols are ignored. The following `TEST` symbols are encoded with the frozen model. Any tail is ignored. Training and test lengths must be positive. The guard may be zero. The split defines disjoint regions within a snapshot. Establishing statistical independence or temporal generalization requires separate controls.

## Model and code

The model ID is `categorical.frozen_huffman.v1`. The training histogram and the fixed escape weight determine a static Huffman tree. The model assigns the escape weight one, creates leaves for observed labels in first-appearance order, then creates the escape leaf. It merges nodes by increasing weight, breaking ties by node insertion order. The first removed node is the left child. This makes tie order independent of numeric labels under injective relabeling.

| Part | Bits |
|---|---|
| Model | Elias gamma of the distinct training label count, followed by a u32 label and gamma-coded positive frequency for each label, in first-appearance order |
| Test header | Elias gamma of `test length + 1` |
| Test data | One fixed codeword for each seen label. Each unseen label uses the escape word followed by its full u32 value |
| Raw comparator | The same test header plus 32 bits per symbol |

Bits are written most significant first. Decoding requires the exact unpadded bit lengths and the supplied model. The C++ `FrozenCategoricalModel` API fits and serializes the model, reconstructs it from model bytes, and encodes or decodes individual test regions. Scoring uses the trained tree for the entire test. The explicit code ID is supplied outside these bit counts.

The CSV reports model bits, test header and data bits, unseen symbols, and both gains in signed bits. Conditional gain is `raw_bits - test_bits`. Charged gain also subtracts the full model cost once for the one training/test pair. A negative gain means the fitted code cost more. A short snapshot reports `insufficient_data` with blank costs. Training is capped at 4,096 symbols. An over-budget row has blank costs and later snapshots still receive their own results.

## Verification

The [registered protocol](../protocols/E01-heldout-categorical.json) passed Release on macOS arm64. The C++ tests matched independent model and test bytes, restored the decoder without training data, and round-tripped 64 seeded pairs. The malformed-input checks covered duplicate model labels, escaping a known label, truncated payloads, and limits. An initial local truncation fixture supplied six bytes for a 40-bit payload and reached the byte-length error. The corrected fixture supplies the five-byte prefix and exercises the intended truncation path.

| Held-out sample with training `[9, 3, 9, 9]` | Model bits | Test bits | Raw bits | Unseen | Conditional gain | Charged gain |
|---|---:|---:|---:|---:|---:|---:|
| `[9, 3, 100, 9]` | 71 | 43 | 133 | 1 | 90 | 19 |
| `[100, 100, 100, 100]` | 71 | 141 | 133 | 4 | −8 | −79 |
| `[9, 9, 9, 9]` | 71 | 9 | 133 | 0 | 124 | 53 |

The independent heap reference matched all six valid rows and one short row. Changing the guard and tail produced identical costs. Relabeling a model with tied weights preserved the lengths. The shifted fixture paid for every escape, and neither that fixture nor the matched fixture changed the model bytes.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `c9e0f9c55f81fc9ba223fb818fe52d48964cb8478aaadfdedbec13910f5a5df6` |
| Held-out CSV | `40349a5dbeb3ed61cd316f6c3f29b019bbeecd929937c2f51c7287c90664d37e` |

Run `scripts/check-conformance.sh` to rebuild and check the installed CLI and decoder. This step establishes exact held-out accounting for one fitted categorical code and its raw comparator. Held-out family selection, temporal validation, structural estimation and scientific calibration remain open.
