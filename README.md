# Complexity Lab

A C++20 trace reader, writer and tier-0 evaluator for the first implementation slice of the revised complexity-growth program. The evaluator takes recorded observations as input. It reports size, type frequencies, entropy, activity, recurrence and byte-RLE diagnostics.

G0 passed on Linux x86-64, Linux arm64, and macOS arm64. The CLTR 1.0 wire contract is frozen. [Audit and scope](docs/G0-closeout.md)

## Build and run

From this directory, with CMake 3.24 or later, a C++20 compiler, Make, and Python 3.9 or later for tests. The inspector uses system CommonCrypto on macOS and OpenSSL's Crypto development library on Linux.

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
build/release/clab-fixture build/demo
build/release/clab-inspect build/demo/trace.cltr
build/release/clab-inspect --index build/demo/trace.cltr
build/release/clab-inspect --metadata build/demo/trace.cltr
build/release/clab-eval build/demo/trace.cltr
build/release/clab-eval --categorical-baselines build/demo/trace.cltr
build/release/clab-eval --categorical-iid build/demo/trace.cltr
build/release/clab-eval --categorical-compare build/demo/trace.cltr
build/release/clab-eval --categorical-heldout 4 1 4 build/demo/trace.cltr
build/release/clab-eval --categorical-temporal 2 1 2 2 build/demo/trace.cltr
build/release/clab-eval --categorical-motif build/demo/trace.cltr
build/release/clab-eval --categorical-motif-heldout 4 1 4 build/demo/trace.cltr
build/release/clab-eval --categorical-motif-temporal 2 1 2 4 build/demo/trace.cltr
build/release/clab-eval --categorical-motif-permutation 2 1 2 4 19 42 build/demo/trace.cltr
build/release/clab-eval --list-metrics
build/release/clab-eval --plan-metrics --metric state.shannon_bits_per_symbol.v1
build/release/clab-eval --metric state.shannon_bits_per_symbol.v1 build/demo/trace.cltr
build/release/clab-eval --cache-dir build/eval-cache build/demo/trace.cltr
```

Choose a fresh directory for each fixture run. For traces containing checkpoint references, `clab-inspect --checkpoints TRACE.cltr` lists their format, size and SHA-256. Use `clab-inspect --verify-checkpoint SEQUENCE CHECKPOINT_FILE TRACE.cltr` to compare one reference against a local file. The demo records a tiny Rule 90 world under a five-sweep cap. Use `clab-fixture --scenario wall-cap NEW_OUTPUT_DIRECTORY` to exercise the watchdog, or `--scenario crash` to exercise a worker failure. In either case, the parent writes one terminal row to `status.csv`. Its trace path is relative to the output directory. For an incomplete trace, use `clab-inspect --recover-prefix` on that path. The initial configure fetches the pinned MIT-licensed nlohmann/json 3.11.3 header and verifies its SHA-256. For an offline configure, set `-DCLAB_JSON_HEADER=/path/to/json.hpp` to a verified copy. The evaluator needs only a trace at runtime.

## Cross-platform conformance

Run `scripts/check-conformance.sh` to build Release, ASan/UBSan and the substrate-free evaluator in a fresh directory under `build/`. The script installs the evaluator-only build, runs the external CMake consumer and checks the installed CLIs against independent fixtures. It uses Clang for the sanitizer build. Set `CLAB_SANITIZER_CXX` to choose a different compatible compiler.

The [E00 conformance workflow](.github/workflows/conformance.yml) runs the script on Linux x86-64, Linux arm64 and macOS arm64. The [protocol](protocols/E00-conformance.json) defines the scope and acceptance checks.

## Install without substrates

```sh
cmake --preset offline-evaluator
cmake --build --preset offline-evaluator --parallel
ctest --preset offline-evaluator
cmake --install build/offline-evaluator --prefix build/install
cmake -S tests/consumer -B build/consumer -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/consumer
build/consumer/consumer build/external.cltr
python3 tests/independent.py --eval build/install/bin/clab-eval --inspect build/install/bin/clab-inspect
```

Applications can link to `clab::trace` and `clab::evaluator` through `find_package(CLab CONFIG REQUIRED)`. Custom providers can implement the observation interface or emit the documented binary format.

For attributed relational traces, `clab-inspect --metadata` lists the schema and `clab-inspect --attributes` lists the slots.

- [CLTR 1.0 format and metric definitions](docs/cltr-v1.md)
- [Initial verification protocol](protocols/E00-initial.json)
- [Initial verification](docs/E00-initial-results.md)
- [Relational snapshot and index verification](docs/E00-relational-results.md)
- [Detailed event and checkpoint reference verification](docs/E00-events-results.md)
- [Checkpoint target verification](docs/E00-checkpoint-target-results.md)
- [Worker watchdog and crash reconciliation](docs/E00-worker-results.md)
- [Tier-0 structural fixture checks](docs/E01-tier0-fixtures.md)
- [Typed trace provenance verification](docs/E00-provenance-results.md)
- [Relational attribute verification](docs/E00-attributes-results.md)
- [Tier-0 metric registry verification](docs/E01-registry-results.md)
- [Tier-0 dependency plan verification](docs/E01-dependencies-results.md)
- [Persistent histogram cache verification](docs/E01-histogram-cache-results.md)
- [Exact categorical baseline codes](docs/E01-categorical-codes.md)
- [Exact categorical IID code](docs/E01-categorical-iid.md)
- [Categorical baseline family comparison](docs/E01-baseline-comparison.md)
- [Frozen categorical held-out evaluation](docs/E01-categorical-heldout.md)
- [Temporal categorical held-out evaluation](docs/E01-categorical-temporal.md)
- [Exact categorical motif dictionary](docs/E01-categorical-motif.md)
- [Frozen categorical motif transfer](docs/E01-categorical-motif-heldout.md)
- [Temporal categorical motif transfer](docs/E01-categorical-motif-temporal.md)
- [Temporal motif permutation test](docs/E01-categorical-motif-permutation.md)
- [Synthetic motif calibration design and runner](docs/E01-categorical-motif-calibration.md)
