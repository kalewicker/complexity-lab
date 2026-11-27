# Two-entry categorical motif dictionary

`clab-eval --categorical-motif-dictionary TRACE.cltr` compares the [five exact baselines](E01-baseline-comparison.md), the [best one-entry candidate](E01-categorical-motif.md), and a dictionary with two distinct contiguous motifs. Each snapshot is scored separately up to 4,096 symbols.

The search retains the best one-entry candidate and its leftmost disjoint placements. It searches the uncovered contiguous intervals for a distinct second pattern of width 2–6 and assigns each candidate its own leftmost disjoint placements. Both entries require at least two occurrences. Among eligible second entries it chooses the one yielding the shortest complete frame, with ties resolved by earliest occurrence and then smaller width. Holding the first entry and its placements fixed makes this a greedy search.

The report shows the two-entry cost even when a cheaper alternative wins. It selects two entries only when their complete encoding strictly beats both the best baseline and the one-entry candidate. Otherwise it selects one entry if the single candidate beats the baseline, or zero entries for a baseline. `selected_definition_bits` counts only the bits spent on selected motif definitions. `no_second_motif` and `no_motif` identify completed searches with an absent candidate. Budget limits and traces without categorical snapshots have explicit statuses with blank selection fields.

## Frame layout

The two-entry frame uses the three-bit tag `110` and Elias gamma of the snapshot length plus one. It then writes both motif definitions in order, each as a gamma-coded width followed by its u32 symbols. For each entry it writes a gamma-coded occurrence count and one gamma-coded gap plus one per occurrence. The gap is measured from the preceding end of the **same** entry in the original snapshot coordinates, or from the start of the snapshot for that entry's first occurrence. Explicitly retaining the original coordinates lets the decoder reject placements that overlap across entries.

The final fields are uncovered u32 symbols in input order. All fields are MSB first, with zero byte padding excluded from the bit count. The C++ comparison exposes both patterns, their placement offsets, and the encoded candidate. The decoder accepts its bytes and bit count and reconstructs the original snapshot.

## Verification

The [registered fixture](../protocols/E01-categorical-motif-dictionary.json) repeated one six-symbol tile with distinct separators. A single copy provided no repeat. At 4, 16 and 64 copies, the selected definition cost was 197 bits throughout. A matched pair of 168-symbol inputs used the same six tile labels and the same distinct separators. One input repeated one ordering 24 times. The other alternated two orderings 12 times each.

| Matched fixture | Best baseline bits | One-entry bits | Two-entry bits | Selected entries | Definition bits |
|---|---:|---:|---:|---:|---:|
| One repeated ordering | 1,596 | 1,062 | | 1 | 197 |
| Two recurring orderings | 1,596 | 3,372 | 1,356 | 2 | 394 |

For the second input, the dictionary charged 18 header bits, 394 definition bits, 176 placement bits and 768 residual bits. A periodic control kept its baseline. Injective relabeling preserved the costs. The C++ checks matched a hand-written interleaved frame and rejected duplicate definitions, overlapping or out-of-bounds placements, missing occurrences, and incomplete data.

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `4a56adb4fb4a3d9df9b68db3e631ca574fff8064d909f2d6ad324b6785066323` |
| CSV report | `404facb542e7733cc87e5276a657e38d98102ccfb7888dae206bf402367d66e1` |

These fixtures establish in-sample accounting for one additional motif definition. Two-entry held-out transfer, null calibration, and structural scoring remain open.
