# Calibration of the second-entry shared-site test

The [registered protocol](../protocols/E01-categorical-motif-dictionary-site-calibration.json) fixes 350 null worlds and 350 planted worlds for the [second-entry permutation test](E01-categorical-motif-dictionary-site-permutation.md). Each world supplies four training frames, one guard frame, and three test frames with 36 sites per frame.

Both populations share the distribution of unordered site histories. Twelve histories keep their labels throughout all eight frames. Twenty-four varying histories receive uniformly assigned fresh labels from a disjoint set each frame. A single assignment of histories to sites applies throughout the world.

- The null assigns all 36 histories to positions uniformly. Naturally adjacent persistent histories remain possible.
- The plant puts labels 1–6 and 7–12 in two contiguous ordered blocks. Their labeled start pair is drawn uniformly from the disjoint placements that leave at least one nuisance site between the blocks. The remaining sites receive a uniform ordering of the nuisance histories.

The generator chooses the site order first, then shuffles each frame’s nuisance labels in frame order. For a plant, it samples the pair of starts in lexicographic order before shuffling the nuisance site indexes. It uses the existing SHA-256 counter generator, with separate namespaces for the protocol, smoke and run rosters, and input and scorer seeds.

## Decision

Each world uses 99 shared-site draws and refits the full pipeline on every draw. The statistic is the positive part of the charged gain over the fitted pair’s first entry, with zero for a completed split without a pair. The world rejects at an exact rank of at most 1/20, with minimum attainable rank 1/100. The audit reconciles gains with the first-entry costs and counts ties and absent draws.

The exact binomial bounds use tail error 1/40 each for simultaneous coverage of at least 95%. The false-positive upper bound must be at most 10%, which allows at most 23 possible false positives out of 350. The sensitivity lower bound must be at least 80%, which requires at least 295 detections out of 350. Unresolved and unstarted nulls count as possible false positives, and unresolved and unstarted plants count as misses.

Inference is scoped to the joint site-exchangeability null and the registered two-block alternative at this observation budget. A null conditional on a prespecified spatial first motif would require its own calibration.

## Readiness

```sh
python3 scripts/calibrate_motifs.py --protocol dictionary-shared-site plan
python3 scripts/calibrate_motifs.py --protocol dictionary-shared-site smoke --eval build/release/clab-eval --out build/dictionary-site-smoke
python3 scripts/calibrate_motifs.py audit build/dictionary-site-smoke
```

The smoke worlds ranked 14/100 and 53/100 for the nulls, and 1/100 for both plants. The independent checks decoded frame histograms and persistent histories, verified separation of the planted blocks, reconciled recorded ranks and rejected a corrupted first-entry cost. The earlier calibration archives still reproduce their original reports.

The `run` action uses the reserved 700-world roster with a fresh output directory and a 20-second timeout per world. These readiness checks preceded the registered execution.

## Registered panel results

The panel ran once from registration commit `de86c58abf6833fd9603a3ee6a195e52543efbbf` using the macOS arm64 release evaluator. All 700 worlds and 69,300 shared-site permutations completed, with zero unresolved or unstarted worlds.

| Stratum | Rejected | Observed rate | Registered confidence bound | Result |
|---|---:|---:|---|---|
| Exchangeable null | 15 / 350 | 4.29% | False-positive upper bound 6.9702% | Pass |
| Planted order | 350 / 350 | 100% | Sensitivity lower bound 98.9515% | Pass |

Training admitted no pair in 32 null worlds. All stayed in the denominator with rank 100/100. Every plant gained 343–349 bits from the second entry after model costs and ranked 1/100, the finest resolution at 99 draws.

The read-only audit reproduced the runner report byte for byte. A separate audit checked the roster and seed derivation, decoded the site histories and separated plant blocks, recomputed each rank from its recorded marginal gains, and verified both exact binomial decisions and outward-rounded bounds. The [per-world ledger](evidence/E01-categorical-motif-dictionary-site-calibration-v1-worlds.csv) includes input and receipt digests. The [machine-readable summary](evidence/E01-categorical-motif-dictionary-site-calibration-v1-summary.json) records the rank distributions and the protocol, manifest, evaluator, and report digests. The raw archive is at `build/categorical-motif-dictionary-site-calibration-v1`.

Both criteria passed for the registered synthetic layout and observation budget. A null conditional on a known spatial first motif, other effect sizes, and structural scoring remain open.
