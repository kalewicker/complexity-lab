"""Audit direct edges in the substrate-enabled CMake link graph."""
import re
import sys
from pathlib import Path

expected = {
    "foundation": set(),
    "observation": {"foundation"},
    "evaluator": {"observation"},
    "trace": {"observation"},
    "substrate_api": {"observation"},
    "clab-eval": {"evaluator", "trace"},
    "clab-fixture": {"substrate_api", "trace"},
    "clab-inspect": {"trace"},
}
if sys.platform != "darwin":
    expected["clab-inspect"].add("OpenSSL::Crypto")

graph = Path(sys.argv[1]).read_text()
observed = {target: set() for target in expected}
for caller, dependency in re.findall(r'^.*"node\d+" -> "node\d+".*// (.+) -> (.+)$',
                                    graph, re.MULTILINE):
    if caller in observed:
        observed[caller].add(dependency)
if observed != expected:
    raise SystemExit(f"Unexpected CLab link graph: {observed}")
for caller, dependencies in observed.items():
    print(f"{caller}: {', '.join(sorted(dependencies)) or '(no dependencies)'}")
