# E00 typed trace provenance

The [registered protocol](../protocols/E00-provenance.json) covers the optional CLTR provenance object and normalized header inspection. The Release fixture check passed on macOS arm64.

| Check | Observed result |
|---|---|
| Independent fixture | Preserved producer, backend, precision, configuration digest, six capability flags, a continuous time domain and seed 18446744073709551615. The unrecognized extension was omitted from normalized output. |
| Legacy and optional fields | Legacy metadata still read without a provenance object. An omitted seed stayed absent and a present seed of zero stayed zero. Earlier trace digests remained valid. |
| Writer | Typed C++ metadata survived a wire round trip. An invalid configuration digest and an invalid time enum were rejected before trace staging. |
| Malformed metadata | All 16 invalid headers were rejected. The cases included duplicates at the root and inside the capability object. |
| Recovery | Strict inspection of the truncated fixture emitted no metadata. Prefix recovery emitted the validated header and returned exit 3. |

The independent trace SHA-256 is `65522e326166048ca58fd001c4c30d2581be2c357baea8f1e35d594b5a0878eb`. Its declared configuration SHA-256 is `a74725c6e624b191b57b182935490f89171eb9ea2cbed25cc8538dd306877626`, calculated from the literal configuration bytes `{"steps":4}`. The fixture checks preservation of the declared digest. The inspector reads the trace and has no configuration file input.

The [wire specification](cltr-v1.md) defines the fields and the [conformance workflow](../.github/workflows/conformance.yml) repeats the tests on the supported platforms. G0 remains open for relational attribute schemas and payload coverage.
