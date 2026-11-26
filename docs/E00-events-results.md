# E00 detailed event and checkpoint reference verification

The [registered protocol](../protocols/E00-events.json) extends the initial trace conformance checks. Tests ran on macOS 15.7.9 arm64 with Apple Clang 17.0.0, CMake 4.3.1 and Python 3.14.5.

| Check | Observed result |
|---|---|
| C++ round trip | Work stamps, event types, rule ID and its absence, participants, lifecycle lists, both provenance values and exact delta bytes survived the round trip. The reference preserved the checkpoint format, size and all 32 digest bytes. |
| Independent fixture | Two events, two births, one death, one observed event and one reconstructed event matched the hand-counted inputs. The CLI listed the exact independently calculated digest and the 30-byte declared size. |
| Rejection | All 11 malformed event batches and seven malformed references failed inspection before their chunks entered the index. The writer rejected events beyond the chunk work, an invalid checkpoint format and a batch above the byte cap. |
| Interrupted trace | A cut inside the reference retained the earlier validated event chunk and produced no checkpoint row. Recovery exited with status 3. |
| Compatibility | The categorical, aggregate-event, relational and capped mock fixtures retained their recorded SHA-256 values. Existing diagnostic checks passed. |
| Build matrix | Release, ASan/UBSan and evaluator-only builds each passed both CTest suites. The installed substrate-free CLI matched the release fixture receipt. The external CMake consumer built and ran against the install. |

| Independent input | SHA-256 |
|---|---|
| Trace | `602af49f1f3105f4a1db42c61c69e67fdb9235f48a194863f8500bb0725f4e60` |
| Byte string named by the reference | `ce9f95fe6a654065c9c3d5c17ce30bde9b06cf567f1abfbd9c070c6fa404f855` |

Run the commands in the [README](../README.md) to reproduce the checks. The independent fixture generator builds the trace bytes and asserts counts, reference fields, recovery and rejection using the installed or build-tree commands.

G0 remains open. This result covers event and checkpoint-reference interchange. Loading and verifying checkpoint targets, run-wide life-cycle validation, wall-clock watchdog and process-crash reconciliation, and Linux conformance remain pending.
