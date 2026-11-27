# Temporal transfer of a two-entry motif dictionary

`clab-eval --categorical-motif-dictionary-temporal TRAIN GUARD TEST SITES TRACE.cltr` fits a dictionary on earlier categorical snapshots and scores later snapshots across a guard window. It uses the same fixed-site and strictly increasing time requirements as the [one-entry temporal test](E01-categorical-motif-temporal.md). The four sizes are positive. The first three count categorical snapshots, and `SITES` specifies the leading prefix used in each scored snapshot. Each aggregate training or test window is capped at 4,096 symbols.

Training selects the best first motif using exact code costs and frame-local placements. It keeps those placements while searching uncovered contiguous intervals for the second motif. Each definition needs at least two placements across the training frames. Each candidate pays its definition once and its placement counts and gaps separately in each frame. The complete pair training cost must strictly beat the first-entry training cost and the sum of the best [five-family baselines](E01-baseline-comparison.md) selected separately in each training frame. Ties in the search prefer the earliest occurrence in snapshot and site order, then smaller width.

The test uses the [frozen two-entry format](E01-categorical-motif-dictionary-heldout.md) with a separate length header and two placement streams per snapshot. The first motif has placement priority. Both patterns and their order remain fixed through testing. The two comparators encode the same test prefixes separately using the pair's first entry alone and Huffman fitted on the same training prefixes. Conditional gains compare test bits. Charged gains also include each model's definition once. The report includes the signed gains, occurrence counts, actual frame counts, and stamps at the training end and both test boundaries.

A completed split that admits no pair gives `valid` with reason `no_training_dictionary` and reports Huffman costs. Missing input and exceeded limits give explicit statuses without costs. The evaluator validates the full trace before emitting a row. The C++ scoring API exposes the serialized pair, first-entry, and Huffman models along with the aggregate costs.

## Verification

The [registered fixture](../protocols/E01-categorical-motif-dictionary-temporal.json) uses two training snapshots of 84 scored sites each. Each frame alternates two orderings of the same six labels with distinct separators. After one guard frame, the scorer evaluates two 84-site snapshots. The matched test contains both orderings. The control repeats just the first ordering at the same label counts and frame sizes. All model costs and the Huffman test cost agree across the two layouts.

| Later layout | First matches | Second matches | Pair test bits | First test bits | Charged gain vs. first | Charged gain vs. Huffman |
|---|---:|---:|---:|---:|---:|---:|
| Both orderings | 14 | 14 | 186 | 2,790 | 2,407 | 997 |
| First ordering only | 28 | 0 | 70 | 68 | −199 | 1,113 |

The pair paid 394 model bits, the first entry paid 197, and Huffman paid 1,047 model bits and 530 test bits. The matched pair test paid 26 header bits and 160 placement bits across both snapshots. The control paid one bit for the absent second entry in each frame. Changing guard data, unused sites, and the tail preserved the entire CSV. A C++ fixture split the second motif across two test frames and verified zero matches. The checks also covered an absent training pair, unsupported metadata, incomplete windows, limits, and an invalid suffix.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `1dfe8acc287df2d0047b153ccdde9f3e749b8dac7856e264ce2f9246da938268` |
| CSV report | `2329ed5eb01be0edd4603a9c97a74062bec519ae9a935257e811282e064c55af` |

These results cover exact accounting and synthetic temporal transfer. Null calibration and structural scoring for the two-entry dictionary remain open.
