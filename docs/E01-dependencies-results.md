# E01 tier-0 dependency plans

The [registered protocol](../protocols/E01-dependencies.json) verifies dependency resolution over the built-in tier-0 catalog. `clab-eval --plan-metrics` displays the plan without opening a trace. The `plan_metrics()` API exposes the same order, and the evaluator uses the result to prepare only needed intermediate data while streaming.

| Check | Observed result |
|---|---|
| Full plan | 34 distinct nodes, including 27 metrics and seven intermediate artifacts. |
| Shared prerequisites | Entropy and per-symbol counts needed one histogram. Component count and port-degree distribution needed the entity index followed by one incidence calculation. A single ledger read count needed no intermediate artifact. |
| Ordering and selection | Both requested output orderings gave the same plan. The synthetic graph test checked a transitive closure, one appearance of each shared prerequisite, and exclusion of an unrelated node. |
| Invalid graphs | Duplicate IDs, duplicate prerequisites, a missing prerequisite, an unknown root, a self-cycle, and a cycle outside the selected branch all failed as expected. |
| Compatibility | The mixed-channel default CSV still matched the recorded pre-registry SHA-256. All 27 single-metric projections and the existing status and input validation checks passed. |

The resolver validates all registered nodes before returning a plan. It orders a prerequisite before its dependents and uses declaration order when queuing nodes that become ready together. Evaluation keeps its existing streaming order. Intermediate data live within the record or bounded snapshot history. That gives multiple diagnostics a common input for the current evaluation.

The Release checks passed on macOS arm64. The [conformance script](../scripts/check-conformance.sh) repeats the tests and checks the API through the installed package. The [registry report](E01-registry-results.md) preserves the reference CSV digest.

Persistent artifact caching, parallel execution and scientific metrics remain open.
