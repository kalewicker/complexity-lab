# Shared-site motif calibration

The [registered protocol](../protocols/E01-categorical-motif-site-calibration.json) fixes 350 null and 350 planted synthetic worlds for the [shared-site motif test](E01-categorical-motif-site-permutation.md). Each world has six training frames, a one-frame guard, and three test frames, with 12 scored sites per frame.

Both strata draw from the same distribution of unordered site histories. Four histories carry their respective labels `1, 2, 3, 4` throughout the trace. The other eight receive a uniformly shuffled set of fresh labels at each frame, with disjoint label sets across frames. A world assigns those histories to sites once, including through the guard.

- The null assigns the 12 histories to sites using a uniform permutation. Adjacent persistent histories are possible under this population.
- The plant draws a uniform start for the contiguous ordered block `[1, 2, 3, 4]` and uniformly orders the other eight histories in the remaining positions.

The shared-site null permutes whole histories, preserving persistence and frame histograms. The planted alternative tests detection of spatial order at this one strength and observation budget.

## Decision and bounds

Each world uses 99 shared-site draws with model refitting. The decision rejects for exact rank at most 1/20, with attainable resolution 1/100. Outcomes are aggregated separately for the two strata. The simultaneous one-sided exact binomial bounds use tail error 1/40 per stratum. Unresolved and unstarted null worlds count as possible false positives. Unresolved and unstarted planted worlds count as misses.

| Criterion | Bound | Registered count at 350 |
|---|---:|---:|
| False-positive rate | Upper bound ≤ 10% | At most 23 possible false positives |
| Sensitivity | Lower bound ≥ 80% | At least 295 detected plants |

As for the [independent-frame design](E01-categorical-motif-calibration.md), planning rates of 5% false positives and 90% sensitivity give respective probabilities .92457 and .99968 of clearing the two bounds. These probabilities describe the planned acceptance rule at those assumed rates.

## Running and auditing

From the source checkout, the panel uses the existing calibration runner with an explicit shared-site selection.

```sh
python3 scripts/calibrate_motifs.py --protocol shared-site plan
python3 scripts/calibrate_motifs.py --protocol shared-site smoke --eval build/release/clab-eval --out build/shared-site-smoke
python3 scripts/calibrate_motifs.py audit build/shared-site-smoke
```

The `run` action uses the registered 700 slots with the same selector, a fresh directory and a 20-second limit per world. The manifest fixes the protocol digest, executable digest and seeds before launching an evaluator. An audit uses the protocol recorded in the manifest and rejects an explicit selection that conflicts with it.

The generator uses the same SHA-256 stream and per-role seed derivation as the independent-frame panel. The new protocol ID separates its seeds from the earlier panel. For each world the generator chooses the site order first. In a plant it chooses the block start before shuffling the nuisance site indexes. It then visits the frames in order and uses the same stream to shuffle each frame's eight fresh labels before applying the site order.

## Readiness verification

The two null smoke worlds ranked 2/100 and 58/100. Both planted smoke worlds ranked 2/100. The first null smoke outcome was a rejection. The smoke distributions check execution and accounting with four separately seeded worlds. Population rates will use the fixed 700-world roster.

Independent checks decoded the raw smoke frames, confirmed the four persistent and eight varying histories, verified the planted block, checked disjoint seed namespaces, and exercised an empty full roster. The audit reproduced the existing independent-frame archive exactly. These checks preceded the registered execution.

## Registered panel results

The panel ran once from registration commit `31c45b418b8067a96696ec818edbfa9226f67b4e` using the macOS arm64 release evaluator. All 700 planned worlds and 69,300 shared-site permutations completed. There were zero unresolved and zero unstarted worlds.

| Stratum | Rejected | Observed rate | Registered confidence bound | Result |
|---|---:|---:|---|---|
| Exchangeable null | 9 / 350 | 2.57% | False-positive upper bound 4.8251% | Pass |
| Planted order | 342 / 350 | 97.71% | Sensitivity lower bound 95.5460% | Pass |

Training admitted no motif in 88 null worlds. Each remained in the denominator with rank 100/100. Training admitted a motif in every planted world. The eight misses had ranks of 6/100 in seven cases and 7/100 in one case, all above the fixed 5/100 cutoff.

The read-only audit reproduced the complete report byte for byte. A separate calculation checked the roster and seed derivation, decoded every frame, verified the persistent histories and planted blocks, recomputed the ranks from the recorded draw gains, and checked the exact binomial decisions and outward rounding. The [per-world ledger](evidence/E01-categorical-motif-site-calibration-v1-worlds.csv) records the input and receipt digests. The [machine-readable summary](evidence/E01-categorical-motif-site-calibration-v1-summary.json) records the rank distributions and the manifest, evaluator, and report digests. The raw traces and receipts are archived locally at `build/categorical-motif-site-calibration-v1`.

Both criteria passed for the registered synthetic site histories, planted block, and observation budget. Other layouts, scales, site-exchangeability assumptions, and structural complexity scoring remain open.
