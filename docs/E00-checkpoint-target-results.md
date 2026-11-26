# E00 checkpoint target verification

The [registered protocol](../protocols/E00-checkpoint-target.json) adds byte verification for a local checkpoint explicitly named by the caller. Tests ran on macOS 15.7.9 arm64 with Apple Clang 17.0.0, CMake 4.3.1 and Python 3.14.5.

| Check | Observed result |
|---|---|
| Matches | The 30-byte and 131,329-byte file targets both matched their independently calculated SHA-256 references. The larger input spans three 64 KiB read buffers. |
| Target failures | Same-size modified, shorter, longer, missing, directory and FIFO targets all failed with no success output. |
| Selection failures | The aggregate event chunk, absent sequence, malformed sequence and overflowing sequence all failed with no success output. |
| Trace failures | A trace truncated after the matching reference and a trace with a corrupt end marker both failed with no success output. |
| Compatibility | Existing categorical, aggregate-event, relational, detailed-event and capped mock trace hashes matched their prior receipts. |
| Build matrix | Release, ASan/UBSan and evaluator-only each passed both CTest suites. The external consumer built and ran against the evaluator-only install. The installed CLI and an OpenSSL 3.6.2 build of the inspector on macOS produced identical fixture receipts. |

| Independent input | SHA-256 |
|---|---|
| Trace | `c465293c23d85f8ec432acd49d22e995b235d9b17ca9dfdb163abdede812563d` |
| 30-byte target | `ce9f95fe6a654065c9c3d5c17ce30bde9b06cf567f1abfbd9c070c6fa404f855` |
| 131,329-byte target | `9a9be4affebde7c5b0a7b853817499d05c089ad7b9abcd60182be4371e7f80b8` |

Run `ctest --preset release` after the [README build steps](../README.md). The Python fixture generator computes its reference digests with `hashlib`. The inspector uses system CommonCrypto on macOS and OpenSSL EVP on Linux. Only the inspector links the Linux crypto dependency. [CommonCrypto digest API](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/CC_SHA256_Init.3cc.html) · [OpenSSL EVP digest API](https://docs.openssl.org/3.5/man3/EVP_DigestInit/)

The verification compares bytes against a trace reference. Checkpoint decoding, restore, and trust in the trace remain outside this slice. G0 remains open for watchdog and process-crash reconciliation, Linux conformance and the other outstanding [trace and fixture coverage](E00-relational-results.md).
