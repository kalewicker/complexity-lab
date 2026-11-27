# Exact categorical motif dictionary

`clab-eval --categorical-motif TRACE.cltr` compares one contiguous repeated motif per categorical snapshot with the shortest of the [five exact baseline codes](E01-baseline-comparison.md). It uses the order of symbols in the recorded snapshot. For each distinct subsequence of length 2–6, the search takes the leftmost match and skips past it before looking for the next match. Candidates need at least two disjoint occurrences. The shortest complete candidate wins the search, with ties resolved by first occurrence offset and then by shorter width.

The report includes the best baseline and the candidate's cost breakdown. A positive `gain_bits` means the candidate uses fewer bits than the baseline and gives `selected=true`. Zero and negative gains retain the baseline. A snapshot with no eligible candidate reports `no_motif` and still includes its baseline. The report gives `budget_exceeded` when a comparison exceeds its coding limits. The default reference cap is 4,096 symbols per snapshot. Event and relational records are skipped for scoring. A trace with no categorical snapshots gives `unsupported`. The command validates the whole trace, and callers must check its exit status before using the CSV.

## Frame layout

The frame uses MSB-first fields and zero padding in the final byte. Bit counts exclude that padding. The fields appear in this order.

| Cost field | Content |
|---|---|
| Header | Three-bit family tag `101` and Elias gamma of the snapshot length plus one |
| Model | Elias gamma of the motif width and each motif symbol as an unsigned 32-bit word |
| Placements | Elias gamma of the occurrence count, then Elias gamma of each gap plus one |
| Residual | All uncovered symbols in input order, each as an unsigned 32-bit word |

Each gap counts uncovered symbols from the previous occurrence end, or from the start of the snapshot for the first gap. The gaps and fixed width determine where the decoder inserts each occurrence. The residual size is the snapshot length minus the occurrence count times the motif width.

`compare_categorical_motif()` returns the exact baseline frame and, when present, the candidate frame with its pattern and zero-based placement offsets. In the candidate's `CodeLength`, `data_bits` includes placements and residual data. Use `decode_categorical_motif()` with the candidate's bytes and total bit count. The existing five-family decoder retains its original tag assignments.

## Verification

The [registered protocol](../protocols/E01-categorical-motif.json) uses a separated repeat, an injectively relabeled copy, a periodic control, and inputs with no eligible candidates. The decoder tests check a hand-built frame, overlapping occurrences, deterministic ties, malformed input and code budgets.

| Fixture | Baseline bits | Header | Model | Placements | Residual | Motif bits | Gain |
|---|---:|---:|---:|---:|---:|---:|---:|
| Six-symbol motif separated by distinct labels, 24 occurrences | 1,596 | 18 | 197 | 79 | 768 | 1,062 | 534 |
| Five-symbol period, 16 occurrences | 181 | 16 | 165 | 25 | 0 | 206 | −25 |

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `79359a9ceb334fa98eac15b30765ef2c16473f6fad129a1a51cef350b9cb86c7` |
| CSV report | `943c6c8a996d6a9bd032bc953df9f5db3c4fbe9ae716698b5149adf009d6e652` |

Run `scripts/check-conformance.sh` to rebuild and verify the installed evaluator. The costs describe in-sample compression under a single-motif search. Held-out dictionary transfer, relational motifs and calibration of a structural metric remain open.
