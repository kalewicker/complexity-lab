# Temporal motif test preserving site histories

`clab-eval --categorical-motif-site-permutation TRAIN GUARD TEST SITES DRAWS SEED TRACE.cltr` shuffles the order of site histories within a temporal window. On each draw it generates one index order and applies it to the leading `SITES` positions of every scored training and test snapshot. This preserves each frame histogram and keeps the full sequence of values at each scored site together. Guard frames, unscored sites, and the trailing part of the trace are excluded from the shuffle.

The [registered method](../protocols/E01-categorical-motif-site-permutation.json) uses the same Fisher–Yates and SplitMix64 procedure as the [independent frame permutation](E01-categorical-motif-permutation.md). It initializes the index order from zero through `SITES - 1` for each draw and advances the random stream across draws. Both models refit for each draw. The split, budgets, draw count limits, signed gains, and tie and missing-draw rules are shared with the independent frame test. The CSV identifies this null as `categorical.shared_site_permutation.v1`.

The shared-site null assumes the joint distribution is invariant to applying one permutation to all scored sites across time. A comparison under this null asks whether spatial arrangement adds motif gain after conditioning on the site histories. Spatially distinct site roles or boundaries can invalidate unrestricted site exchangeability. The calibrated [independent frame result](E01-categorical-motif-calibration.md#registered-panel-results) applies to its own two populations and permutation scheme.

## Verification

A stationary trace repeated one six-label frame at fixed sites. The frozen motif gained 69 bits after model costs. Every shared-site draw preserved the ability to fit and reuse the entire frame and tied the original gain. Independent frame draws broke that recurrence.

| Permutation scheme | Draws at least as large as observed | Rank |
|---|---:|---:|
| Independent frame order | 0 / 19 | 1/20 |
| One order across scored sites | 19 / 19 | 20/20 |

A second trace kept two labels at fixed sites while changing the four other scored labels each frame. Independent replay verified every shuffled frame histogram and the multiset of full scored site histories. It reproduced all 19 draw scores and the shared-site rank of 3/20. Mutating the guard, unscored sites, and tail preserved the CSV. The C++ tests also verified that either permutation scheme retains failed draws and suppresses the rank after a byte limit violation.

| Independent artifact | SHA-256 |
|---|---|
| Stationary CLTR | `441365af31d49bcb33b31d57b2cee58994d80c532cf9cdf6cec9d8d531fdd5d0` |
| Stationary CSV | `afff4d4923a6c3cb46553f2f54bbff7efbe113d7b382b7450087549a662eb790` |
| Changing CLTR | `110cdb85ee68832f1ecea52e1349558636e37f14d418b7c8d1178639b7582ee2` |
| Changing CSV | `de0f845ac094ccf809c0396ebeea80c8235684c2152414bf2f9576cad88cab02` |

The [shared-site calibration design](E01-categorical-motif-site-calibration.md) fixes the populations and criteria for measuring false positives and sensitivity. The population panel remains unstarted.
