# Initial E00 verification

Verified on macOS 15.7.9 arm64 with Apple Clang 17.0.0 (1700.6.4.2), CMake 4.3.1 and Python 3.14.5. The initial protocol is in [E00-initial.json](../protocols/E00-initial.json).

| Check | Observed result |
|---|---|
| E00a subset | 100 categorical round trips passed. The independent Python writer produced two readable fixtures. The inspector rejected 12 malformed variants. |
| E00b subset | All 30 seed/checkpoint pairs continued identically for 12 sweeps. Observing left cells, RNG state and work unchanged. |
| E00c | An external CMake consumer built and ran against the installed package with substrate targets disabled. Both independent fixtures passed against the installed CLI. |
| E00d subset | Recovery returned three records from a trace cut short inside the end marker and classified it as incomplete. The strict evaluator rejected it. A staged trace left unfinished by the C++ writer also recovered as incomplete. The one planned mock run stopped at work 5 of 8, with one matching status row and a complete trace through work 5. A 71-byte cap rejected the 72-byte CA cell buffers before allocation. |
| Build checks | Release, evaluator-only Release, and Debug with ASan/UBSan each passed both CTest suites. |

The hand-calculated results and CLI outputs agreed on all checked values.

| Diagnostic | Expected | Observed |
|---|---:|---:|
| Symbols in first snapshot | 4 | 4 |
| Counts of symbols 0 and 1 | 2 each | 2 each |
| Entropy in first snapshot, bits per symbol | 1 | 1 |
| RLE bytes per raw byte in first snapshot | 0.625 | 0.625 |
| Sites changed in second snapshot | 2 | 2 |
| Recurrence lag in second and third snapshots | 0, 2 | 0, 2 |
| Events, births and deaths in events-only trace | 7, 2, 3 | 7, 2, 3 |

| Input | SHA-256 |
|---|---|
| Independent categorical trace | `3dda44df0c60342ddd2b6456bbb350ea66a50141d86dc62ba4e887a0000fc9ba` |
| Independent events-only trace | `5f5e31d67be90e5eea677c262b01358df9cc2427ff86a17d975c44f4d34ddac1` |
| Capped mock trace | `890dcb152bc02ec22bb4754fefbd69a6bea966239fbdc87df7d3c72110780ff1` |

Use the commands in the [README](../README.md) to reproduce Release and installed results. For ASan/UBSan, run `cmake --preset sanitize`, `cmake --build --preset sanitize` and `ctest --preset sanitize`. To reproduce the mock receipt and all input hashes, run:

```sh
python3 tests/independent.py --eval build/release/clab-eval \
  --inspect build/release/clab-inspect --fixture build/release/clab-fixture
```

## Remaining milestone work

G0 remains open. The trace supports categorical native snapshots, aggregate event batches and a reduced ledger. Relational observations, participant-level events, checkpoint references, a trace index, and the rest of the planned metadata are pending. The cap fixture covers an exact work cutoff and allocation preflight. A wall-clock watchdog and process-crash reconciliation remain unverified. The fixture suite covers the first diagnostic slice. The full tier-0 registry, the artifact cache, the wider fixture panel and Linux conformance remain pending. These checks establish engineering behavior for the implemented slice. Scientific calibration and world search have yet to start.
