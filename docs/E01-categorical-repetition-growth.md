# Repetition and distinct motif growth

This development experiment tests whether a categorical dictionary’s supported definition cost distinguishes extra copies of one motif from growth in distinct reusable motifs. It adapts the research plan’s F02 repetition control to a fixed observation budget and uses the plan’s provisional 10% tolerance. The result applies to this generator and categorical scoring rule. [Registered protocol](../protocols/E01-categorical-repetition-growth.json)

## Matched windows

Each of 24 paired fixtures contains three eight-frame traces. The early trace is shared by both comparisons. All frames have 24 slots at the same sites. Each slot contains six symbols and a separator. A pair draws a common label ordering for motif A, uses its reversal as motif B, and selects 22 of the 24 slots for A in the early and diversified traces.

| Trace | Selected 22 slots | Other 2 slots |
|---|---|---|
| Early | A | Shuffled label orders other than A and B |
| Repeat | A | A |
| Diversify | A | B |

In the early trace the shuffle is independent for each other slot and frame, with rejection sampling to exclude A and B. All traces use the same frame-specific separators, with one fresh label per slot per frame. This holds frame size, per-frame histograms, and early A positions fixed across the matched comparison. The two late traces also have equal numbers of ordered slots. Each trace trains on four frames and scores three later frames across one guard frame. The seed namespace contains the protocol ID, roster mode, and zero-based pair index.

## Decision

Run the existing one-entry and two-entry temporal scorers on each trace. A one-entry dictionary is eligible when it beats its frozen Huffman comparator in both conditional and charged test bits. A pair must beat its embedded first entry and Huffman on both comparisons. When the one-entry scorer also supplies a fitted dictionary, the pair must beat that dictionary on both comparisons as well. The two scorers must agree on their Huffman costs.

Use the selected dictionary’s model-definition bits once per trace for the development score. Select the pair when eligible, otherwise use the eligible one-entry model, otherwise report zero for a completed check. Record model bits, placement bits, test bits, and gains against Huffman separately for the candidates. An unavailable scorer, invalid split, or missing result leaves the pair unresolved. This experiment uses the raw definition cost of a dictionary that transfers to held-out data. The research plan’s full structural estimand also requires an eligible baseline and calibrated support.

Compute both changes from the shared early score. A passing pair requires a supported single entry in the early trace, an increase of at least one bit in the diversified trace, and an absolute change due to repetition of at most one tenth of that increase. Require at least 22 of 24 passing pairs. Unresolved pairs count against that requirement. Report every pair and both changes, including cases that fail the ordering.

The two smoke pairs use separate seeds for checking trace construction and accounting. The reserved development panel covers one scale and two known six-symbol orderings. Blind fixtures, cross-representation invariance, and sensitivity to weaker motifs remain separate G1 requirements.

## Readiness

An initial development generator used 12 A slots in the early traces. The single-entry scorer admitted neither early motif. The early score was 0 bits in both smoke pairs, and the repeat and diversify scores were 197 and 394 bits. The repeated motif crossed the admission threshold as its frequency rose. The higher initial support in the registered design targets growth after an early motif is already admitted.

With 22 A slots, both smoke pairs admitted one entry early and under repetition, and two under diversification. Both had 197 early bits, 197 repeat bits, and 394 diversify bits. The 24 registered pairs have a separate seed namespace and remain unstarted.

```sh
python3 scripts/repetition_growth.py smoke --eval build/release/clab-eval --out build/repetition-growth-smoke
```

| Smoke report | SHA-256 |
|---|---|
| Initial 12-slot design | `2e327121173543953e4eaa44bd2c01d17a0f4b6a7ecfd70c8500d4b426ed6641` |
| Registered 22-slot design | `6c9b018333104b4251458265f5b7b497a37d791e6053cd5718b4b4d686ec0515` |
