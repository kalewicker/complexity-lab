# E01 persistent symbol histogram cache

The [registered protocol](../protocols/E01-histogram-cache.json) covers optional reuse of one intermediate artifact. Entropy and per-symbol counts use the same exact categorical histogram. They can reuse an entry across evaluator invocations.

```sh
clab-eval --cache-dir build/eval-cache --metric state.shannon_bits_per_symbol.v1 TRACE.cltr
```

Use the directory as scratch storage. The cache namespace is `symbol-histogram-v1`. Entries retain the exact slice of observation bytes used as the key, including the encoding ID, coverage flag, and ordered little-endian u32 symbols. An FNV-1a lookup chooses a slot. The evaluator compares the full input key and validates the entry length, checksum, sorted bins and count sum before using the histogram. The format and artifact version identify this deterministic counting procedure. A change to the procedure requires a new cache namespace.

There are 64 published slots, each capped at 256 KiB, for 16 MiB of slot data at most. Oversize inputs and histograms are counted directly. A new entry replaces its slot by atomic rename after the write has closed. Separate processes can share the directory. The most recent successful publication occupies a slot, including when two different keys select the same slot. Interrupted writes can leave temporary files that are ignored during lookups.

The CLI sends counters to stderr. A hit reuses a validated entry. A miss computes the histogram after a lookup. A store publishes an entry. A skip means the key or result exceeded the per-entry cap. When the result alone exceeds the cap, the same request counts as both a miss and a skip. A write failure leaves the computed diagnostic usable. The C++ API exposes the same cache and counters through `HistogramCache`.

| Check | Observed result |
|---|---|
| CSV parity | Uncached, cold and warm output matched byte for byte. Selecting entropy and symbol counts together produced one lookup. Entropy alone then reused the same entry. |
| Identity | Changing a symbol, reordering symbols, changing the encoding or changing the coverage flag each missed. One-symbol inputs 0 and 64 mapped to the same slot and each returned its own correct counts. |
| Recovery | Truncated, checksum-damaged and oversized entries were replaced. Two concurrent evaluators produced the expected CSV and left a usable entry. Unavailable storage preserved the result and reported a write failure. |
| Bounds | 128 inputs left 64 published slot files. Oversized inputs and results were computed without publishing entries. An unrelated metric created no cache directory. |

| Independent witness | SHA-256 |
|---|---|
| CLTR input | `9e19943c17e644257aa87a3af82396c2744615be57e1e682afbbb3fd4392c728` |
| Selected CSV | `90ec72875005cb259ffa36f11dba6e8c1c95c19409c68eda397110472fc6c4bb` |
| Single cache entry | `b152d450d4c7c27857ff2e01a7015da827a526a3deb1f15de1fcec6470b01fdf` |

The Release checks passed on macOS arm64. The [conformance script](../scripts/check-conformance.sh) repeats the checks, reaches the CLI through the installed executables, and exercises the API in the external consumer. This milestone covers exact histogram reuse. Other artifacts, performance measurement and scientific calibration remain open.
