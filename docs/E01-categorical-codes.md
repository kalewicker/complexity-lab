# Exact categorical baseline codes

`clab-eval --categorical-baselines TRACE.cltr` reports bit lengths for four reference codes on categorical snapshots. Every length comes from the actual emitted bitstream. The CSV counts the bits used for the family and length header, the model, the encoded observations, and their sum. The C++ API in `clab/categorical_code.hpp` exposes the bytes, the bit counts, and a decoder.

## Version 1 code layouts

Every code starts with its two-bit family tag and an Elias gamma code for `n + 1`, where `n` is the observation length. For a positive integer, gamma writes its binary representation preceded by one zero for each remaining bit beyond the first. Fixed-width words and bit fields are written most significant bit first. The three header bits alone encode an empty observation for each family.

| Tag | Code | Model bits | Data bits |
|---|---|---|---|
| `00` | `categorical.raw.v1` | None | All symbols as 32-bit words |
| `01` | `categorical.uniform.v1` | Gamma-coded alphabet size followed by the distinct labels as ascending 32-bit words | Zero-based alphabet indices at width `ceil(log2(k))` for alphabet size `k`. A singleton alphabet uses zero bits per symbol |
| `10` | `categorical.run_length.v1` | None | A 32-bit label and gamma-coded positive run length for each maximal equal-symbol run |
| `11` | `categorical.periodic.v1` | Gamma-coded period and that many leading labels as 32-bit words | None. The decoder repeats the template to length `n` |

The encoder chooses the shortest finite period, allowing a partial final repetition. For example, `3,9,3,9,3` has period two. Uniform alphabet coding charges the full label table and positions. This reference code uses fixed-width indices.

The byte API stores zero padding at the right edge of its last byte. Supply the unpadded total bit count to decode. Padding contributes no bits to the report. The decoder requires exact byte length and rejects nonzero padding, truncated data, trailing bits, and invalid indices, runs or model counts. Default API limits are 1,048,576 symbols and 8 MiB of encoded bytes. The baseline CSV uses empty lengths with `unsupported` when the trace contains no categorical snapshots, and `budget_exceeded` when a code exceeds a limit. The mode reports the supplied observation's coding length. CLTR wire layout and the default diagnostic CSV are unchanged.

## Verification

The [registered panel](../protocols/E01-categorical-codes.json) passed Release on macOS arm64. C++ checked exact bytes for the short witness, round trips and relabel invariance over 128 seeded sequences plus edge cases, and rejection of malformed and over-budget inputs. The independent Python oracle checked 56 CLI rows using its own bit-string encoder and brute-force period search. It also checked the channel-absent status and rejection of a truncated trace.

| Code for `[3, 9, 3, 9, 3]` | Header | Model | Data | Total bits |
|---|---:|---:|---:|---:|
| Raw | 7 | 0 | 160 | 167 |
| Uniform alphabet | 7 | 67 | 5 | 79 |
| Run length | 7 | 0 | 165 | 172 |
| Periodic | 7 | 67 | 0 | 74 |

| Independent artifact | SHA-256 |
|---|---|
| CLTR fixture | `588600fa9be00e558d044481b6c4e74d6672d4fe66e4e298a6c4ed951a4b55d2` |
| Baseline CSV | `bfc268ab453e184ea40260d50d748ba318c0768c1c758cdc577f30c345ebfd05` |

Run `scripts/check-conformance.sh` from the project root to repeat the built and installed checks. This step establishes exact engineering codelengths. Nonuniform IID coding, model comparison, and scientific calibration remain open.
