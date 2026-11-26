# E00 relational snapshot and index verification

The [registered protocol](../protocols/E00-relational.json) extends the initial E00 engineering slice. Tests ran on macOS 15.7.9 arm64 with Apple Clang 17.0.0, CMake 4.3.1 and Python 3.14.5.

| Check | Expected and observed |
|---|---|
| C++ round trip | Two entity rows, two relation rows and three port rows preserved. An ordered relation retained a repeated endpoint and distinct roles. The second relation had zero ports. |
| Independent fixture | Three entities, two relations and five ports. Entity types 2 and 3 occurred twice and once. Relation type 7 occurred twice. Arities 2 and 3 occurred once each. |
| Relabeling and order | All eight diagnostics agreed after changing IDs, reordering entity rows and exchanging the unordered relation's ports. |
| Index | Header offsets 129, 329, 381 and 581 matched the independent byte calculation. Entries included an unknown optional chunk and the end marker. |
| Interrupted index | Cutting the third chunk inside its payload yielded two validated index rows with recovery exit 3. Strict inspection failed. |
| Malformed relational payloads | All eleven invalid variants failed inspection and produced zero index rows. |
| Compatibility | Original categorical, events-only and capped mock fixture hashes match the initial receipt. Existing diagnostic checks passed. |
| Builds | Both CTest suites passed in Release, ASan/UBSan and evaluator-only builds. The external consumer and the independent fixtures passed against the substrate-free install. |

Independent relational trace SHA-256 is `b9d66dd6527a31254bd27f97840a345fde40c5b100507d0def7720ecd410412d`. The test harness generates the source bytes and verifies numeric outputs and offsets. The [README](../README.md) contains the commands for local and installed checks.

G0 remains open. The index is a CSV export from a validated sequential scan. Direct seeking, descriptor and attribute coverage, participant-level events, checkpoint references, watchdog and process-crash reconciliation, the broader fixture panel and Linux conformance remain pending. The results cover the wire format and diagnostics exercised here.
