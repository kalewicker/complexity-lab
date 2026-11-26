# Complexity Lab

A C++20 trace reader, writer and tier-0 evaluator for the first implementation slice of the revised complexity-growth program. The evaluator takes recorded observations as input. It reports size, type frequencies, entropy, activity, recurrence and byte-RLE diagnostics.

## Build and run

From this directory, with CMake 3.24 or later, a C++20 compiler, Make, and Python 3.9 or later for tests:

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
build/release/clab-fixture build/demo
build/release/clab-inspect build/demo/trace.cltr
build/release/clab-eval build/demo/trace.cltr
```

Choose a fresh directory for each fixture run. The demo records a tiny Rule 90 world under a five-sweep cap. The initial configure fetches the pinned MIT-licensed nlohmann/json 3.11.3 header and verifies its SHA-256. For an offline configure, set `-DCLAB_JSON_HEADER=/path/to/json.hpp` to a verified copy. The evaluator needs only a trace at runtime.

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

- [CLTR draft format and metric definitions](docs/cltr-v1.md)
- [Initial verification protocol](protocols/E00-initial.json)
- [Verification results and scope](docs/E00-initial-results.md)
