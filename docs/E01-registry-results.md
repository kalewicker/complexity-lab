# E01 tier-0 metric registry

The [registered protocol](../protocols/E01-registry.json) covers discovery and selection of the existing tier-0 diagnostics. `clab-eval --list-metrics` lists 27 versioned IDs with their observation channel and unit. The [wire and CSV documentation](cltr-v1.md#tier-0-output) describes the CLI and API.

| Check | Observed result |
|---|---|
| Catalog | Exactly 27 unique IDs, grouped into categorical, relational, event and ledger channels. Listing required no trace. |
| Default compatibility | The mixed-channel fixture produced byte-identical default CSV with the pre-registry G0 executable and the current Release build. Explicitly selecting all catalog IDs produced the same CSV. |
| Filtering | All 27 single-ID outputs matched the respective rows of the full output. The mixed selection produced eight rows, retaining valid, unsupported and insufficient-data statuses. A repeated ID produced no extra rows. |
| Budget status | With history disabled by the API budget, selected recurrence yielded `insufficient_data` on the first snapshot and `budget_exceeded` on the second. Both values were empty. |
| Input validation | Unknown IDs failed before an observation was read. Selection still rejected a truncated trace suffix and changing fixed-site snapshot size. |
| Execution | The evaluator skips unused diagnostic groups, including recurrence history and relational incidence analysis when their metrics are absent from the request. |

| Independent fixture | SHA-256 |
|---|---|
| Mixed-channel CLTR | `af9fb08aceca09423024acbe9c05405e8aa02daac02e58c4ba237bbe04806a53` |
| Default CSV from the pre-registry G0 executable and current Release | `a1129d06e5de0595b308416bebc7fc7a1ee81029b107504ac6f5dd4ab8b2001b` |

Run `scripts/check-conformance.sh` to rebuild and exercise the installed executables and CMake consumer. The [hosted workflow](../.github/workflows/conformance.yml) repeats the checks on Linux x86-64, Linux arm64 and macOS arm64.

This is the built-in tier-0 catalog and selection milestone. Artifact caching, general dependency scheduling and scientific estimators remain open.
