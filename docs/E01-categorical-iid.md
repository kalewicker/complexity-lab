# Exact categorical IID code

Use `clab-eval --categorical-iid TRACE.cltr` to report the exact bit cost of a histogram-conditioned IID code. The C++ functions `encode_categorical_iid()` and `decode_categorical_iid()` expose the serialized bytes through the installed evaluator library.

An IID categorical model assigns equal probability to arrangements with the same counts, even when category probabilities differ. This two-part code transmits the empirical counts, then distinguishes the observed arrangement among all arrangements with those counts. It charges the model explicitly and uses exact integer arithmetic for the rank. Conditional rank length and the plug-in IID log likelihood are distinct quantities.

## Version 1 layout

The external code ID is `categorical.iid_enumerative.v1`. It identifies the decoder and is outside the payload. Family selection across multiple codes would need its own shared accounting. This layout leaves the previous four baseline formats intact.

| Part | Bits |
|---|---|
| Header | Elias gamma of `n + 1`, where `n` is the observation length |
| Model | For nonempty observations, Elias gamma of the label count, then each distinct u32 label in ascending order followed by Elias gamma of its positive count. The counts sum to `n`. The empty observation has no model bits |
| Data | The sequence's zero-based lexicographic rank among arrangements of the fitted counts, using `ceil(log2(M))` bits for `M = n! / product(count_i!)`. A singleton composition has no data bits |

Bits are packed most significant first and zero padded in the final byte. The length supplied to decode excludes padding. The decoder checks increasing labels, the sum of counts, zero padding, exact bit length and rank range. It reconstructs a sequence by finding the containing prefix block and repeating on the residual rank. For `m` remaining symbols and `T` possible arrangements, a label with remaining count `c` starts `T*c/m` arrangements.

The reference encoder and decoder cap this code at 4,096 symbols per observation. The API accepts smaller symbol and byte limits. The report leaves bit fields empty when it emits `budget_exceeded` and continues to later snapshots. Traces without categorical snapshots report `unsupported`.

## Verification

The [registered protocol](../protocols/E01-iid-enumerative.json) passed Release on macOS arm64. Tests covered 60 exhaustive ranks for one histogram, 128 seeded cases, full-range u32 labels, relabeling, a 600-symbol case, the 4,096-symbol boundary, and malformed payload rejection. The witness `[9, 3, 3]` used 5 header, 71 model and 2 rank bits. The descending arrangement with 40 copies each of labels 9 and 3 serialized a 77-bit rank and matched the independent golden bytes. The independent CLI reference computed widths from integer factorials. It verified seven valid rows and an over-budget row followed by a valid row.

| Independent fixture | SHA-256 |
|---|---|
| CLTR input | `4d037acb4ec05b7442b4fb451032ffbdf86b7f345c8d74d9c5df27d1878f1ad9` |
| IID CSV | `fc4255174f0e0030ac0c82f6da86bfe8ee51f61b3e96a2113668a90c122bb874` |

The [conformance script](../scripts/check-conformance.sh) also builds and tests the installed decoder and checks that the previous [baseline CSV](E01-categorical-codes.md) stays byte-identical on its witness. Common family-selection costs, held-out fitting, and scientific calibration remain open.
