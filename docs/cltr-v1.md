# CLTR 1.0 draft wire format

This milestone implements uncompressed categorical snapshots, event counts and a small work ledger. Multi-byte integers are unsigned little-endian. Floating-point time is IEEE 754 binary64 in little-endian order. Table sizes are bytes. There is no padding.

## File prefix

| Field | Size | Value |
|---|---:|---|
| Magic | 4 | ASCII CLTR |
| Major | 2 | 1 |
| Minor | 2 | 0 |
| Byte-order marker | 4 | 0x01020304 |
| Metadata length | 4 | Number of UTF-8 JSON bytes |

Metadata immediately follows. Required fields are nonempty strings `run_id`, `encoding`, `time_unit` and booleans `exact`, `fixed_sites`. An encoding identifies the categorical alphabet and the ordering convention. `exact` indicates complete coverage under the declared encoding. A producer setting `fixed_sites` must preserve the same sites in the same order between snapshots. A sampled fixed-site observation can describe recurrence of the sample. Unknown metadata fields are ignored by this reader. Preserve the original trace for provenance.

## Chunk envelope

| Field | Size | Value |
|---|---:|---|
| Type | 2 | Payload identifier |
| Version | 2 | 1 for the types below |
| Flags | 4 | Bit 0 marks a skippable type or version. All other bits reserved |
| Sequence | 8 | Contiguous from zero, including skipped chunks and end |
| Simulated time | 8 | Finite, nonnegative and nondecreasing |
| Reference work | 8 | Nondecreasing, producer-defined |
| Raw length | 4 | Uncompressed payload bytes |
| Stored length | 4 | Equals raw length in this release |
| CRC32C | 4 | Castagnoli checksum over the preceding 40 bytes and stored payload |
| Payload | Stored length | Specified below |

An unknown major version or required chunk fails validation. Compatible minor versions use the same prefix and envelope. Unknown optional chunks are checked and skipped. The reader checks both lengths before allocation. Defaults are 64 KiB for metadata and 4 MiB per chunk.

| Type | Payload |
|---:|---|
| 0 | End marker, empty, zero flags. No trailing data. For an empty trace its time and work are zero |
| 1 | Ordered array of u32 symbols. Array length comes from the chunk length |
| 2 | Three u64 integers for event count, births and deaths over the batch. An event may account for several births or deaths |
| 3 | Six u64 integers for cumulative logical updates, reads, writes, RNG draws, current cell/state bytes and peak cell/state bytes |

A complete trace has a checked end marker. Recovery exposes only the complete validated prefix of a trace cut short after valid metadata. Checksum failures remain errors in recovery mode. On supported local filesystems the writer stages in a unique adjacent directory and publishes by creating a hard link after closing the staged file. This atomically claims the destination without replacing an existing trace. The staging path remains available if publication fails. A process crash can leave a staging directory. The prototype makes no power-loss durability guarantee.

## Tier 0 output

`clab-eval` streams CSV with trace identity, stamp, encoding, coverage flag, metric ID, optional symbol, value and status. The exit status must be checked before using the CSV. A suffix error can occur after earlier rows were written. Missing values are empty with an explicit status. Counts on sampled traces describe the supplied sample.

The entropy diagnostic is empirical Shannon entropy in bits per symbol. Byte RLE uses pairs of one-byte length and one-byte value, splits runs at 255, and scores the canonical little-endian payload only. Changed sites compare consecutive fixed-site snapshots. Recurrence uses exact symbol equality across up to 16 previous snapshots within 4 MiB of history. Lag zero means no match within the reported lookback. These diagnostics describe the observations under this encoding and budget.
