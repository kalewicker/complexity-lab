# G0 engineering closeout

**Decision:** Passed for the portable trace and reference-fixture milestone. The [CLTR 1.0 wire specification](cltr-v1.md) is frozen.

The [registered audit](../protocols/E00-g0-audit.json) evaluated commit `d614975887281cbfaff5e094cc94be14771ba13c`. Release, Clang ASan/UBSan, and substrate-free Release builds passed on Linux x86-64, Linux arm64, and macOS arm64. The conformance script also verified the standalone installed CMake consumer and exercised the installed evaluator and inspector with independent trace bytes. [Hosted evidence](https://github.com/kalewicker/complexity-lab/actions/runs/45903178806)

| Check | Evidence |
|---|---|
| E00a wire conformance | 100 randomized categorical snapshots round-tripped. Aggregate events, work ledger, both relational payload versions, detailed events and checkpoint references each had semantic round-trip coverage. The independent minor-version pair agreed on diagnostics. Two unknown optional chunks were checked and skipped. Four incompatible or corrupt variants were rejected. |
| E00b replay and observation | The Rule 90 reference and synthetic graph provider each passed 30 seed/checkpoint cases with 12-step continuations. Observing left their states and RNG positions unchanged. The graph provider and its 12-record trace produced identical evaluator rows. |
| E00c dependency boundary | The substrate-enabled CMake link graph showed `clab-eval` depending on evaluator and trace, which both depend on observation, which depends on foundation. The evaluator-only install exported precisely these library edges and no substrate target. The Linux inspector uses OpenSSL Crypto privately. The installed evaluator and imported targets carry no OpenSSL dependency. |
| E00d termination and recovery | All five fixture workers produced a matching terminal row. Work, wall and cell-buffer caps, normal completion, and worker failure matched the recorded outcomes. Prior trace and prefix hashes matched. The incomplete traces were readable in recovery mode. |
| Tier-0 evidence | Hand-calculated categorical and event fixtures, the relational fixtures, and the structural diagnostic panel passed through the installed analyzer. |

Use `scripts/check-conformance.sh` for an independent rebuild. The audit compares the generated link graph and tests the installed exported package. CTest and the installed fixture receipt record the observed outcomes. Earlier input hashes and expected values are recorded in the [initial verification](E00-initial-results.md), [worker panel](E00-worker-results.md), and [tier-0 report](E01-tier0-fixtures.md).

| Independent compatibility fixture | SHA-256 |
|---|---|
| CLTR 1.0 baseline | `1098fd4ea69f94b9534261919a4df10c32c11c375c2609056e3a6cd7873a4c90` |
| Higher minor, with optional unknown kind and version | `fd34b02acc51c3a719341dc684c58a3593ab1d4ad5b684ab66bc4ccbdd5ab2ff` |

The freeze applies to the documented envelope, recognized metadata semantics and published payload layouts. Future compatible extensions follow the [wire compatibility rules](cltr-v1.md#compatibility). The broader T05 fixture families, evaluator registry and artifact cache, native encodings beyond the current reference coverage, and scientific metric development remain open. Scientific calibration has yet to begin.
