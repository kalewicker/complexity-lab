# Conditional support for a second motif

## Archived failure diagnosis

A read-only audit recovered the second patterns nominated from the four training frames in the earlier repetition experiment. It checked the slot layouts and reproduced the evaluator’s test counts and marginal code savings for every admitted pair. The four early dictionaries added a two-symbol pattern from the shuffled slots. With the pattern fixed by training, there are six independently shuffled test slots where it could match. [Original experiment](E01-categorical-repetition-growth.md)

| Pair | Second pattern | Test matches | Charged gain over first entry | Exact tail under original background |
|---|---|---:|---:|---:|
| 000 | 6, 2 | 2 | 26 bits | 0.26434 |
| 013 | 5, 3 | 3 | 73 bits | 0.06273 |
| 021 | 3, 1 | 2 | 28 bits | 0.26099 |
| 023 | 1, 2 | 3 | 81 bits | 0.06273 |

All four probabilities exceed 0.05. The exact check counts how often the frozen candidate appears in shuffled slots, where a particular two-symbol order can appear in roughly one slot in six. The 24 archived diversified traces each nominated the planted order and matched in all six test slots. Under the fresh protocol's background, which allows the reverse order, each has exact tail `1 / 719⁶`. These checks reuse the completed experiment and are diagnostic. The recorded result is still 20 of 24 against 22 required. [Machine-readable audit](evidence/E01-categorical-repetition-support-v1-audit.json)

## Fresh calibration

The registered design uses 350 independently seeded matched groups. Each has 24 slots of six labels and a unique separator, four training frames, one guard, and three test frames. The generator fixes A and its 22 slots before producing frames. The two remaining slots contain independent uniform permutations of the six labels excluding A in the null trace, A in the repetition control, and reverse A in the plant. Excluding only A gives the plant order a nonzero null probability. The held-out tail is conditional on the training data and on the generator's known A layout. [Protocol](../protocols/E01-categorical-repetition-conditional-support.json)

The existing training procedure nominates one second pattern. The runner checks that independent recovery of the pattern over the uncovered slots reproduces the code costs. For the fixed candidate, enumerate all 719 legal slot orders to get the probability of containing the candidate. As a slot contains each label once, it contributes at most one match. Count matches across the six test slots and use the exact upper binomial tail. This accounts for the chance of a single frozen nominee conditional on training. The additional held-out gain requirements can only narrow rejection. A two-entry dictionary must pass both the original gain checks and the tail cutoff of 1/20. Otherwise, use the existing single-entry support rule.

Require all three gates.

- The simultaneous one-sided false-positive upper bound must be at most 10%, with tail error 1/40. At most 23 of 350 nulls may be possible false positives.
- The simultaneous one-sided sensitivity lower bound must be at least 80%, with tail error 1/40. At least 295 of 350 plants must retain the second entry.
- At least 315 of 350 matched groups must support a single early entry, gain positive definition bits from diversification, and keep the absolute repetition change within 10% of that gain.

Unresolved or unstarted nulls count as possible false positives. Unresolved or unstarted plants count as misses, and incomplete matched groups fail the growth check. The exact test assumes independent within-slot order under the declared background. Inferred motif locations, correlated slot orders, and other scales require their own calibration.

## Readiness

```sh
python3 scripts/audit_repetition_support.py build/categorical-repetition-growth-v1
python3 scripts/calibrate_repetition_support.py smoke --eval build/release/clab-eval --out build/repetition-conditional-support-smoke
```

Both smoke groups passed the growth ordering. One early trace admitted a second entry on held-out savings and fell back to a single entry after the exact check. Both smoke plants kept the second entry. The smoke report reproduced byte for byte, and separate checks verified exact tails and the registered count boundaries. The 350 reserved groups used a separate seed namespace.

## Registered results

The panel ran once from registration commit `39aa941121cb82934cdafe5e3010e79d0a4d7992` with the macOS arm64 release evaluator. All 350 matched groups and 1,050 conditions completed. Each registered gate passed.

| Gate | Observed result | Registered requirement |
|---|---:|---:|
| Possible false positives | 0 / 350, upper bound 1.0485% | At most 23, upper bound at most 10% |
| Planted second entries retained | 350 / 350, lower bound 98.9515% | At least 295, lower bound at least 80% |
| Matched growth ordering | 350 / 350 | At least 315 |

Held-out savings alone favored a second entry in 63 null traces. All 63 nominees were two-symbol patterns. Of these, 49 matched twice and 14 matched three times across the six test slots. The exact test screened out all 63. Every planted order matched in all six test slots and retained its second entry. In every group, definition cost stayed at 197 bits under repetition and rose to 394 bits under diversification.

A separate audit regenerated the slot assignments from the registered seeds, decoded all traces, reconciled the scorer receipts, and independently recovered the training nominees. It recomputed each exact tail by counting orders that contain the fixed pattern as a contiguous block, and verified the three decisions and outward-rounded bounds. The raw archive is at `build/categorical-repetition-conditional-support-v1`. [Per-condition ledger](evidence/E01-categorical-repetition-conditional-support-v1-conditions.csv) · [Summary and digests](evidence/E01-categorical-repetition-conditional-support-v1-summary.json)

This result supports the conditional test at the registered observation budget with a known first motif and independently shuffled slots. Other backgrounds, inferred first motifs, and representation and scale tests remain open. [G1 status](G1-status.md)
