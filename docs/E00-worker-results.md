# E00 worker watchdog and crash reconciliation

The [registered five-case panel](../protocols/E00-worker.json) exercised one worker per fresh output directory on macOS 15.7.9 arm64 with Apple Clang 17.0.0, CMake 4.3.1 and Python 3.14.5. The parent publishes `planned.csv` before starting the worker. After reaping it, the parent publishes one matching terminal row with an atomic rename. The row separately records the worker outcome, evidence available in the trace, and elapsed monotonic time.

| Scenario | Status | Reason | Last paired work | Trace | Worker result | Fixture exit |
|---|---|---|---:|---|---|---:|
| Default work cap | capped | work | 5 | complete | exited 0 | 0 |
| Normal completion | completed | | 8 | complete | exited 0 | 0 |
| Wall cap | capped | wall | 2 | incomplete | SIGKILL after watchdog request | 0 |
| Worker crash | failed | signal | 3 | incomplete | self-issued SIGKILL | 1 |
| Cell-buffer cap | capped | memory | | missing | exited 2 | 0 |

All five planned rows had exactly one matching terminal row. The wall case reaped the blocked worker in 1,500 ms in the recorded Release check. The deadline governs when the parent requests termination. Scheduling and reaping can add latency. The memory case stopped at allocation preflight with a 71-byte cell-buffer cap for a 72-byte request.

A snapshot followed by a ledger with the same work and matching update count establishes a recorded boundary. The reported `completed_work` is the last such boundary. The exact point of interruption can lie after it. An empty value means the trace supplies no accepted boundary. The incomplete traces kept six and eight records respectively. The Python oracle independently decoded every recorded chunk, verified its CRC32C, recomputed Rule 90 from each preceding snapshot and checked its matched work ledger. Strict inspection rejected both incomplete traces. Prefix recovery returned exit 3.

| Trace | SHA-256 |
|---|---|
| Work cap | `890dcb152bc02ec22bb4754fefbd69a6bea966239fbdc87df7d3c72110780ff1` |
| Completed | `98ad5e901b0e96ef29279dc7cc1073b94af79bd85a3b290d204e66f1149426d4` |
| Wall cap prefix | `f81de7a7abd230b6846ae7d9d7297323a07cca041916bada4ec87845e3cf7c0c` |
| Crash prefix | `0ab5bdc3857c879cb044606b004a5c52195f897752aba0260ec8c191829bf92f` |

Both CTest suites passed in Release, ASan/UBSan and evaluator-only builds. The installed evaluator and inspector matched the Release receipt for the shared trace fixtures. The external CMake consumer ran against the substrate-free install. Run `ctest --preset release` after the [README build steps](../README.md) to reproduce the panel.

G0 remains open. The supervisor owns one child with no descendants. Recovery from supervisor death or power loss and general campaign resumption are outside this check. Linux conformance and the wider trace and diagnostic fixture panel remain pending.
