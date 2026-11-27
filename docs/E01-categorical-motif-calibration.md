# Synthetic temporal motif calibration

The [registered protocol](../protocols/E01-categorical-motif-calibration.json) fixes two strata of 350 independent synthetic worlds. Each world has six training frames, one guard frame, and three test frames, with 12 categorical sites per frame. Every frame contains the same four motif labels and eight nuisance labels unique to that frame. The two strata share the per-frame label counts.

- The exchangeable null independently permutes all 12 positions in each frame.
- The planted stratum places one contiguous `[1, 2, 3, 4]` at a uniform valid start in each scored frame. It independently shuffles the eight nuisance labels around the motif. Its guard frame uses the exchangeable layout.

The full population contains 700 worlds at this one window size and planted effect. Each world gets 99 within-frame permutations and the [complete temporal refit](E01-categorical-motif-permutation.md). The decision rejects when the exact rank is at most 1/20. The minimum attainable rank is 1/100. Worlds are the units for the two rates, and each world contributes one decision.

## Acceptance

Simultaneous coverage is at least 95% under the registered world populations. The two one-sided exact binomial bounds each use tail error 1/40. The false-positive upper bound must be at most .10, and the sensitivity lower bound must be at least .80. Unresolved and unstarted null worlds count as possible false positives. Unresolved and unstarted planted worlds count as misses. The report rounds the bounds outward to millionths and uses exact binomial tail comparisons for the decisions.

| Quantity | Registered criterion | Count at 350 |
|---|---:|---:|
| False-positive upper bound | ≤ .10 | At most 23 possible false positives |
| Sensitivity lower bound | ≥ .80 | At least 295 detected plants |

At planning rates of .05 false positives and .90 sensitivity, the respective probabilities of passing the bounds are .92457 and .99968. These are design calculations. Inference is confined to the two prespecified synthetic strata.

## Running and auditing

From the source checkout, with a built evaluator, print the plan and exercise the smoke roster with these commands.

```sh
python3 scripts/calibrate_motifs.py plan
python3 scripts/calibrate_motifs.py smoke --eval build/release/clab-eval --out build/motif-calibration-smoke
python3 scripts/calibrate_motifs.py audit build/motif-calibration-smoke
```

The `run` subcommand uses the 700-world roster with the same options and a fresh output directory. The manifest fixes the protocol digest, binary digest and both seeds for every slot before launching the first evaluator. The runner snapshots the supplied evaluator and invokes it once per world with a 20-second timeout. It records the CLTR and evaluator receipt in each world's directory. The audit regenerates each input, checks the archived binary and roster, and recomputes ranks from the recorded gain tokens. Incomplete archives retain every slot in the denominator.

Seed namespaces include the protocol ID, mode, world ID and role. The input key is SHA-256 of the newline-delimited namespace followed by `input`. The permutation seed uses the first eight bytes of the corresponding `scorer` digest as a big-endian integer. The generator hashes the input key followed by an eight-byte big-endian counter starting at zero. It uses the first eight digest bytes with rejection sampling for bounded choices. Separate smoke and run namespaces keep the four smoke worlds outside the calibration population.

## Readiness verification

The four smoke ranks were 68/100 and 100/100 for the null worlds, and 1/100 for both planted worlds. The conformance fixture verified their frame histograms and planted placements, the exact acceptance count boundaries, and reconciliation of a missing receipt, a bad rank, and an unstarted world. An empty full roster audited as 700 unstarted worlds and failed both conservative bounds. The registered calibration panel remains unstarted. The smoke check establishes readiness for this panel. Population results and broader structural scoring remain open.
