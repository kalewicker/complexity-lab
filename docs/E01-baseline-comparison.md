# Categorical baseline family comparison

`clab-eval --categorical-compare TRACE.cltr` reports all five code costs for each categorical snapshot. When all five are available, `selected` marks the smallest total. Ties resolve by ascending family tag. The C++ `compare_categorical()` API also returns the selected frame, which `decode_selected_categorical()` decodes without an external family ID.

## Common frame and accounting

| Tag | Candidate |
|---|---|
| `000` | Raw |
| `001` | Uniform alphabet |
| `010` | Run length |
| `011` | Finite period |
| `100` | Histogram-conditioned IID |
| `101`–`111` | Reserved. The decoder rejects them |

A frame starts with the common three-bit tag, followed by gamma of `n + 1`, then the model and data bits of that family's code. Bits are most significant first. The decoder receives the unpadded bit count and checks the zero padding. The four original family codes previously included a two-bit tag. The standalone IID code relies on its external ID. In this frame, all five pay exactly three selector bits.

`header_bits` includes the three `family_bits` and the gamma-coded length. The reported total is `header_bits + model_bits + data_bits`. The older [unified report for the original four codes](E01-categorical-codes.md) and [standalone IID report](E01-categorical-iid.md) retain their original accounting and formats.

The comparison uses the declared five-family set. When any member exceeds the budget, the CSV retains the available costs, reports `comparison_status=budget_exceeded`, and leaves every candidate unselected. The API returns an empty winner in this case. Later snapshots still receive their own results. The IID limit is 4,096 symbols. An input with no categorical snapshots returns one unsupported row per family.

## Verification

The [registered protocol](../protocols/E01-baseline-comparison.json) passed Release on macOS arm64. Independent formulas matched all 40 candidate rows from eight snapshots. The fixture selected each family at least once, resolved an empty five-way tie, withheld a winner for an oversized IID candidate, and selected a winner again on the following snapshot. The C++ tests decoded a selected frame from each family, rejected all three reserved tags and invalid padding or truncation, and checked the byte budget after adding the shared selector.

| Fixture | Selected family | Total bits |
|---|---|---:|
| Empty, all candidates tied | Raw | 4 |
| Twelve distinct symbols | Raw | 394 |
| Balanced binary sequence | Uniform alphabet | 113 |
| Two long runs | Run length | 106 |
| `[3, 9, 3, 9, 3]` | Finite period | 75 |
| Sparse, irregular binary sequence | Histogram-conditioned IID | 160 |

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `878caaa22deef63faf885b70f8e22c773265b1994e5607b08cbc0859bb65be86` |
| Comparison CSV | `978a9ee7ff5f2a2d6f6a884163c236872315addccfcd81e0f6013d488f8955c4` |

The [conformance script](../scripts/check-conformance.sh) verifies the reports and the installed decoder. This is an in-sample coding comparison on fixed fixtures. Held-out fitting, structural gain, and scientific calibration remain open.
