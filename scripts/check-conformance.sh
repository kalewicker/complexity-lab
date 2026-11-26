#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build
check_root=$(mktemp -d "$PWD/build/conformance.XXXXXX")
json_header="$check_root/deps/nlohmann/json.hpp"
jobs=${CLAB_BUILD_JOBS:-3}

printf 'Conformance workspace: %s\n' "$check_root"
uname -srm
cmake --version
python3 --version

build_and_test() {
    local preset=$1
    local build_dir="$check_root/$preset"
    if [[ "$preset" == release ]]; then
        local links="$check_root/target-links.dot"
        cmake --preset "$preset" -B "$build_dir" -DCLAB_JSON_HEADER="$json_header"             --graphviz="$links"
        python3 tests/check_links.py "$links"
    else
        cmake --preset "$preset" -B "$build_dir" -DCLAB_JSON_HEADER="$json_header"
    fi
    cmake --build "$build_dir" --parallel "$jobs"
    ctest --test-dir "$build_dir" --output-on-failure
}

build_and_test release
CXX="${CLAB_SANITIZER_CXX:-clang++}" build_and_test sanitize
build_and_test offline-evaluator

prefix="$check_root/install"
cmake --install "$check_root/offline-evaluator" --prefix "$prefix"
cmake -S tests/consumer -B "$check_root/consumer" -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$check_root/consumer" --parallel "$jobs"
"$check_root/consumer/consumer" "$check_root/consumer.cltr"
python3 tests/independent.py --eval "$prefix/bin/clab-eval" --inspect "$prefix/bin/clab-inspect" |
    tee "$check_root/installed-results.json"
