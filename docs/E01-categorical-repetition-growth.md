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

With 22 A slots, both smoke pairs admitted one entry early and under repetition, and two under diversification. Both had 197 early bits, 197 repeat bits, and 394 diversify bits. The 24 registered pairs used a separate seed namespace.

```sh
python3 scripts/repetition_growth.py smoke --eval build/release/clab-eval --out build/repetition-growth-smoke
```

| Smoke report | SHA-256 |
|---|---|
| Initial 12-slot design | `2e327121173543953e4eaa44bd2c01d17a0f4b6a7ecfd70c8500d4b426ed6641` |
| Registered 22-slot design | `6c9b018333104b4251458265f5b7b497a37d791e6053cd5718b4b4d686ec0515` |

## Registered panel results

The panel ran once from registration commit `a9ea18af6d0b3357b1cc12b07a8133b1a3702845` on the macOS arm64 release evaluator. All 24 pairs completed. **The panel failed the registered criterion with 20 passes against the required 22.**

| Pairs | Early definition bits | Repeat definition bits | Diversify definition bits | Repeat change | Diversify change |
|---:|---:|---:|---:|---:|---:|
| 20 | 197 | 197 | 394 | 0 | +197 |
| 4 | 264 | 197 | 394 | −67 | +130 |

All 24 repeated traces selected one entry, and all 24 diversified traces selected two. The four failures were pairs 000, 013, 021, and 023. Their early scorers admitted an extra two-symbol entry in the randomly ordered slots. It matched two or three times in the test frames and saved 26–81 charged bits against the first entry. These pairs failed both the early single-entry requirement and the 10% tolerance. The failure identifies a limitation of using positive held-out savings alone as support for a distinct entry in this fixture.

A separate audit decoded all 72 traces, checked matching slot layouts and label counts, reconciled the scorer receipts, and recomputed all 24 decisions. The raw archive is at `build/categorical-repetition-growth-v1`. [Per-pair ledger with definition, placement, and held-out gain bits](evidence/E01-categorical-repetition-growth-v1-pairs.csv)

The follow-up diagnostic recovered the four early candidates and checked their observed counts under the shuffled-slot background. The follow-up registers a conditional support test on fresh layouts. [Conditional support diagnosis and design](E01-categorical-repetition-conditional-support.md)

| Artifact | SHA-256 |
|---|---|
| Protocol | `08658eb1c1cce18ded9e08b2018a502e807c0e4b01c986c5cf0166993b9cabfb` |
| Manifest | `c665763faf8ca7fd44b842894ba43065e9255174122f78f43f41477feb4584ff` |
| Evaluator | `9c3b218df57de646f136333eb911059638e076e8357f85a5bb73abfa0ec6b2af` |
| Runner report | `6ab41f796392a420b16e2478a14974fcc3887b15aad6c5c6cf62ad5df2922329` |
| Per-pair ledger | `59b7007a619cd94772f2afc2381c77af07279ef543a9aae490687aa43ea5da9e` |
