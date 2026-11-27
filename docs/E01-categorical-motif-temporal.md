# Temporal categorical motif transfer

`clab-eval --categorical-motif-temporal TRAIN GUARD TEST SITES TRACE.cltr` fits one motif and one Huffman model on earlier snapshots, skips a guard window and uses both frozen models on later snapshots. All four sizes are positive. The first three count categorical snapshots. `SITES` selects the same leading site prefix in each training or test snapshot. The trace must declare fixed sites with uniform snapshot sizes, and simulated times must increase strictly in the selected windows. The CSV includes the observed window counts and the sequence, work and time at the training end and both test boundaries.

Training enumerates patterns of width 2–6 within each site prefix. A candidate needs at least two disjoint placements across the training frames, using the leftmost available match in each frame. The search minimizes the cost of serializing the dictionary once plus each training prefix as a separate [frozen motif frame](E01-categorical-motif-heldout.md). Ties go to the earliest occurrence in snapshot and site order and then the shorter pattern. The resulting cost must beat the sum of the [five-family comparisons](E01-baseline-comparison.md), each selecting its best baseline and paying its family selector on one training frame.

The trained Huffman comparator uses all symbols in the same training prefixes. Both models encode each test prefix as a separate frame with its own length header. Motif placements stop at each frame boundary. `conditional_gain_bits` compares the summed test frame costs. `charged_gain_bits` adds each model definition once before taking the difference. Positive gain favors the motif. The C++ result exposes both serialized model definitions when a motif is admitted.

The scorer requires at most 4,096 symbols in each aggregate scored window. It reports an explicit reason when a split is unsupported, incomplete or over budget. A completed split with no admitted motif reports `valid` with reason `no_training_motif`, displays the Huffman costs and leaves motif and gain fields blank. The evaluator validates the complete trace before producing a row.

## Verification

The [registered protocol](../protocols/E01-categorical-motif-temporal.json) trained on two snapshots containing 12 repetitions of a six-symbol pattern each, separated by distinct labels. It scored two later snapshots of 84 sites each after one guard snapshot. Reversing the pattern in the first later snapshot preserved the Huffman cost and removed 14 motif placements. Mutating guard data, unselected sites and the trace tail preserved the entire CSV row.

| Later snapshots | Occurrences | Motif model bits | Motif test bits | Huffman model bits | Huffman test bits | Conditional gain | Charged gain |
|---|---:|---:|---:|---:|---:|---:|---:|
| Both in learned order | 28 | 197 | 68 | 1,047 | 530 | 462 | 1,312 |
| First reordered, same label counts | 14 | 197 | 2,736 | 1,047 | 530 | −2,206 | −1,356 |

A C++ fixture put the first and second halves of a six-symbol pattern on opposite sides of frame boundaries. Flattening nominated that pattern, while frame-aware training selected a three-symbol pattern wholly inside the frames. The checks also exercised short windows, duplicate times, coding limits, an absent motif, an inconsistent suffix shape and truncated input.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `aa2189d2c32f2ad9ea48bf5f747f21a0a386ea70b7d2611c504e2c70e505d2fc` |
| CSV report | `f5a595e5f583341b4319c6b97b290b1b7f2941c55a69b7c3f413a5a92c09781f` |

The fixtures test sequence compression on synthetic later snapshots. Statistical independence, null calibration and structural scoring remain open.
