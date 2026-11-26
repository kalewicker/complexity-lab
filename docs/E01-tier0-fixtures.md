# Tier-0 structural fixture checks

The [registered panel](../protocols/E01-tier0.json) gives independently encoded examples for the categorical and relational diagnostics. The Python test writes each CLTR file from the wire specification and checks the evaluator's CSV against fixed expected values.

| Fixture | Check |
|---|---|
| Categorical edge cases | Longest runs were 8, 0 and 1 in uniform, empty and all-distinct snapshots. The uniform and all-distinct snapshots gave entropy 0 and 4 bits per symbol. The empty snapshot reported `insufficient_data` for entropy. |
| Repeated relation | At 1, 4, 16 and 64 copies, component count equaled copy count. Every entity had one port incidence. |
| Growing path | Each of the four sizes had one component. The singleton had degree zero. Larger paths had two endpoints of degree one and the remaining entities of degree two. |
| Frozen modules | After an empty graph, both identical states had three components, including one isolated entity. The zero-port relation added no component. A repeated endpoint counted twice in the degree histogram. |
| Period three | The first sample had insufficient recurrence history. Subsequent lags were 0, 0, 3, 3, 3 and 3. Every consecutive pair differed at two sites. |
| Transport framing | Interleaving unknown optional chunks changed wire sequence numbers and the file digest. All diagnostic rows agreed when matched by observation work. |
| Relabeled hypergraph | The previous golden pair matched on all eleven diagnostics, including component count and degree bins, after changing IDs, entity row order and unordered ports. |

The Release fixture check passed on macOS arm64. The six new input hashes are recorded below. The [conformance workflow](../.github/workflows/conformance.yml) reruns the panel together with the existing tests.

| Trace | SHA-256 |
|---|---|
| Categorical edge cases | `9e56962ea8611ad06bf916273cdf7f3df85b7fe806cafb122e5a94fe2f347891` |
| Repeated relation | `f37da71e52c60a95e07d46a529906555582d360369a6330a87c2957dc2467916` |
| Growing path | `29a2e43a1f0ffcfde8b2fb5acf5c69d7d9d365732487397bbca862d35034fbf5` |
| Frozen modules | `df174bcc280e9b07939bfbcaaa965ba7a7c4a0478c8f983a5745afe1833677ac` |
| Period three | `7476a0d24bbf216e8d3841e37c514b3285a225f8844e695ed141e71256d61e10` |
| Period three, interleaved frames | `add6ea86e4beaa00aa4dbcac521ebadddd41e51ea455a3f05ecbbf4c3be0f23b` |

These are the categorical and relational slices of the planned fixture panel. Their outputs describe the supplied observations. Remaining trace metadata and attributes, other native encodings, and the scientific estimators and controls remain outside this check.
