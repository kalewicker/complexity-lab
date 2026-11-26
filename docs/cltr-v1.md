# CLTR 1.0 draft wire format

This milestone implements uncompressed categorical and relational snapshots, event records, a small work ledger and checkpoint references. Multi-byte integers are unsigned little-endian. Floating-point time is IEEE 754 binary64 in little-endian order. Table sizes are bytes. There is no padding.

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
| 4 | Typed relational snapshot |
| 5 | Detailed event batch |
| 6 | Checkpoint reference |

Readers that lack support for an emitted required chunk reject it. Existing chunk layouts are unchanged.

### Type 4 payload

Three u32 counts give the number of entities, relations and ports. The counts are followed by the three row arrays, in that order. The payload length must equal `12 + 12*entities + 24*relations + 12*ports`.

| Row | Fields in wire order |
|---|---|
| Entity | u64 ID, u32 type |
| Relation | u64 ID, u32 type, u32 first-port index, u32 port count, u32 flags |
| Port | u64 entity ID, u32 role |

IDs are nonzero and unique within their respective entity or relation table. Every port references an entity in the snapshot. Relation port ranges partition the port table in row order, starting at index zero. Zero-port relations and repeated endpoints are allowed. Relation flag bit 0 marks ordered ports. All other bits must be zero. Role values and type codes derive their meaning from the producer's encoding. The wire order, roles and IDs are preserved when reading. The format carries no attributes in this payload version.

### Type 5 payload

A u32 event count is followed by that many event records without padding. Each record starts with the following header.

| Field | Wire type |
|---|---|
| Reference work | u64 |
| Event type | u32 |
| Provenance flags | u32 |
| Rule ID | u64 |
| Participant count | u32 |
| Created entity count | u32 |
| Deleted entity count | u32 |
| State delta length in bytes | u32 |

The body contains the participant, created and deleted arrays, each of u64 entity IDs, followed by the opaque state-delta bytes. IDs are nonzero and unique within each list. Creation and deletion lists are disjoint. Participants can also appear in either list. Flag bit 0 marks reconstructed events. Clear flags mark direct observations. Other bits must be zero. A zero rule ID indicates that the producer supplied no rule ID.

Records have nondecreasing work within each batch and each event's work is at most its chunk's work. The chunk stamp describes the batch emission. A batch may describe historical work, so consumers cannot infer cross-batch ordering from emission alone. The encoding defines the event and delta semantics. The reader validates wire bounds and local lists. Snapshot membership, lifecycle history, and causal dependence need additional evidence. An empty batch explicitly reports zero events for its scope.

### Type 6 payload

A nonzero u64 checkpoint byte length is followed by 32 bytes of SHA-256 digest, a u32 format identifier length and that many ASCII bytes. The identifier is 1–128 characters from letters, digits, period, underscore and hyphen. It names a producer-defined checkpoint format. The chunk stamp identifies the referenced boundary. The reference carries no storage path. Readers can validate its layout, while access to the target bytes is required to verify the length and digest or restore a producer's state.

## Validation and index

A complete trace has a checked end marker. Recovery exposes only the complete validated prefix of a trace cut short after valid metadata. Checksum failures remain errors in recovery mode. On supported local filesystems the writer stages in a unique adjacent directory and publishes by creating a hard link after closing the staged file. This atomically claims the destination without replacing an existing trace. The staging path remains available if publication fails. A process crash can leave a staging directory. The prototype makes no power-loss durability guarantee.

`clab-inspect --index TRACE.cltr` emits a CSV offset index derived from a full sequential validation. Offsets point to chunk headers and are counted from byte zero. Entries include checked envelope fields, cover skipped optional chunks and include the end marker only after EOF is verified. The CSV does not authenticate the trace or provide direct seeking. Keep it paired with the exact input bytes. Exit 0 indicates a complete index. With `--recover-prefix`, exit 3 indicates a checked prefix of an incomplete trace. An error can leave earlier rows on stdout. Discard the output on any other nonzero exit.

`clab-inspect --checkpoints TRACE.cltr` lists checkpoint sequence, work, format, byte length and the lowercase hexadecimal SHA-256 digest. It uses the same exit statuses as the index. A complete scan verifies the trace envelope and reference encoding.

`clab-inspect --verify-checkpoint SEQUENCE CHECKPOINT_FILE TRACE.cltr` first validates the complete trace and selects the reference by chunk sequence. It then checks the explicitly supplied regular file. Verification streams through a 64 KiB buffer and reports success only when the byte count and SHA-256 both match. Argument errors exit 2. Invalid traces, missing references and target mismatches exit 1. A successful match establishes the identity of the bytes read against the reference. Checkpoint trust, decoding and state restore belong to the producer's workflow.

## Tier 0 output

`clab-eval` streams CSV with trace identity, stamp, encoding, coverage flag, metric ID, optional symbol, value and status. The exit status must be checked before using the CSV. A suffix error can occur after earlier rows were written. Missing values are empty with an explicit status. Counts on sampled traces describe the supplied sample.

The entropy diagnostic is empirical Shannon entropy in bits per symbol. Byte RLE uses pairs of one-byte length and one-byte value, splits runs at 255, and scores the canonical little-endian payload only. The longest symbol run is the largest contiguous block of equal symbols in the categorical payload order. An empty snapshot has longest run zero. This diagnostic follows the serialized order. Changed sites compare consecutive fixed-site snapshots. Recurrence uses exact symbol equality across up to 16 previous snapshots within 4 MiB of history. Lag zero means no match within the reported lookback. These diagnostics describe the observations under this encoding and budget.

For type 4, the evaluator reports entity, relation and port counts plus entity-type, relation-type and relation-arity histograms. The histogram key occupies the CSV `symbol` column. A hyperedge contributes one relation and as many port incidences as it has ports. Repeated endpoints count as distinct incidences. Identifier values and port order do not enter these diagnostics. The port-degree histogram counts each entity's incident ports, with a zero-degree bin for isolated entities. If a degree exceeds the CSV histogram key range, the evaluator reports `budget_exceeded` for that histogram. Component count unions the entities that share a relation, ignoring roles and direction. Every isolated entity adds one component. Repeated endpoints, zero-port relations and empty snapshots add no extra components. An empty graph has a valid component count of zero and no degree bins.

Type 5 produces event, birth and death counts per batch, plus separate observed and reconstructed event counts. The provenance split is unsupported for type 2 aggregate batches. To count activity over a window, provide disjoint batches or supply overlap information in the producer's encoding. The evaluator emits rows per chunk and does not infer overlap. Type 6 contributes no diagnostic value.
