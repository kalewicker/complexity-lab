# Calibration of the anchored second-entry test

The [registered protocol](../protocols/E01-categorical-motif-dictionary-anchored-calibration.json) fixes 350 null worlds and 350 planted worlds for the [anchored permutation test](E01-categorical-motif-dictionary-anchored-permutation.md). Each world has four training frames, one guard frame, and three test frames, with 36 sites per frame. The first motif is fixed at zero-based sites 8–13 and contains labels 1–6 in order throughout each world.

Both strata have the same distribution of unordered movable histories. Six histories preserve labels 7–12 across all frames. The other 24 histories get fresh labels each frame from disjoint sets. One assignment of histories to sites applies throughout each world, including the guard.

- The null assigns the 30 movable histories uniformly to the unanchored sites. Chance runs of adjacent persistent histories are possible.
- The plant places labels 7–12 in order at a uniformly sampled contiguous start with at least one nuisance site between the two blocks. It assigns the other histories uniformly to the remaining sites.

The generator samples the site order first, then shuffles each frame’s nuisance labels in frame order. Plant starts are enumerated in ascending order. The SHA-256 counter generator derives distinct seeds by protocol, roster mode, stratum, world index, and purpose.

## Decision

Each world uses 99 permutations that preserve the anchored histories and shuffle the remaining whole site histories. The evaluator refits the second entry on each draw. The statistic is the positive part of the gain over the anchored first entry after model costs. A completed split without a pair has statistic zero, and ties count toward the rank. A world rejects at a rank of at most 1/20.

Two exact one-sided binomial bounds with tail error 1/40 each give simultaneous coverage of at least 95%. The false-positive upper bound must be at most 10%, which allows at most 23 possible false positives out of 350. The sensitivity lower bound must be at least 80%, which requires at least 295 detections out of 350. Unresolved and unstarted nulls count as possible false positives, and unresolved and unstarted plants count as misses.

Inference is scoped to joint exchangeability of movable histories conditional on the fixed anchor, and to the planted block distribution at this observation budget. Selecting an anchor from the scored data would require a separate design.

## Readiness

```sh
python3 scripts/calibrate_motifs.py --protocol dictionary-anchored plan
python3 scripts/calibrate_motifs.py --protocol dictionary-anchored smoke --eval build/release/clab-eval --out build/dictionary-anchored-smoke
python3 scripts/calibrate_motifs.py audit build/dictionary-anchored-smoke
```

The two smoke nulls ranked 100/100 and 28/100. Both smoke plants ranked 1/100. Independent checks decoded the frames, verified the anchor and planted layouts, recomputed the ranks from recorded gains, and confirmed that the audit marked a receipt with a mismatched anchor as unresolved.

The `run` action uses the reserved 700-world roster with a fresh output directory and a 20-second timeout per world. All 700 registered slots are unstarted.
