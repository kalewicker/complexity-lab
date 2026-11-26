"""Produce CLTR bytes without using any C++ implementation code."""
import argparse
import csv
from copy import deepcopy
import hashlib
import io
import json
import os
import signal
import struct
import subprocess
import tempfile
from pathlib import Path

META = {
    "run_id": 'hand-"written"',
    "encoding": "categorical-u32",
    "time_unit": "step",
    "exact": True,
    "fixed_sites": True,
}


def crc32c(data):
    # Reflected Castagnoli polynomial, independent of zlib's IEEE CRC32.
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def metadata_header(data):
    return struct.pack("<4sHHII", b"CLTR", 1, 0, 0x01020304, len(data)) + data


def header(meta=META):
    return metadata_header(json.dumps(meta, separators=(",", ":")).encode())


def chunk(kind, sequence, time, work, payload=b"", flags=0, version=1):
    prefix = struct.pack("<HHIQdQII", kind, version, flags, sequence, time, work,
                         len(payload), len(payload))
    return prefix + struct.pack("<I", crc32c(prefix + payload)) + payload


def run(tool, *args, code=0):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                            timeout=20)
    assert result.returncode == code, (tool, args, result.returncode, result.stderr)
    return result.stdout


def metric(rows, name, sequence="", symbol=""):
    matches = [
        row for row in rows
        if (row["metric_id"], row["sequence"], row["symbol"]) ==
        (name, str(sequence), str(symbol))
    ]
    assert len(matches) == 1, matches
    return matches[0]


def relational_payload(entities, relations, ports):
    return (struct.pack("<3I", len(entities), len(relations), len(ports))
            + b"".join(struct.pack("<QI", *row) for row in entities)
            + b"".join(struct.pack("<Q4I", *row) for row in relations)
            + b"".join(struct.pack("<QI", *row) for row in ports))


def check_relational(args, directory):
    relations = [(40, 7, 0, 2, 0), (50, 7, 2, 3, 1)]
    entities = [(5, 2), (8, 2), (12, 3)]
    ports = [(5, 0), (8, 0), (12, 1), (5, 2), (12, 2)]
    payload = relational_payload(entities, relations, ports)
    relabeled = relational_payload(
        [(102, 3), (4, 2), (9, 2)],
        [(401, 7, 0, 2, 0), (10, 7, 2, 3, 1)],
        [(4, 0), (9, 0), (102, 1), (9, 2), (102, 2)],
    )
    chunks = [chunk(4, 0, 0, 0, payload),
              chunk(77, 1, 0, 0, b"optional", flags=1),
              chunk(4, 2, 1, 2, relabeled),
              chunk(0, 3, 1, 2)]
    prefix = header(META | {"encoding": "relational-fixture", "fixed_sites": False})
    data = prefix + b"".join(chunks)
    trace = directory / "relational.cltr"
    trace.write_bytes(data)
    rows = list(csv.DictReader(io.StringIO(run(args.eval, trace))))
    expected = {
        ("relational.entity_count.v1", ""): "3",
        ("relational.relation_count.v1", ""): "2",
        ("relational.port_count.v1", ""): "5",
        ("relational.entity_type_count.v1", "2"): "2",
        ("relational.entity_type_count.v1", "3"): "1",
        ("relational.relation_type_count.v1", "7"): "2",
        ("relational.arity_count.v1", "2"): "1",
        ("relational.arity_count.v1", "3"): "1",
        ("relational.component_count.v1", ""): "1",
        ("relational.port_degree_count.v1", "1"): "1",
        ("relational.port_degree_count.v1", "2"): "2",
    }
    for seq in (0, 2):
        found = {(row["metric_id"], row["symbol"]): row["value"]
                 for row in rows if row["sequence"] == str(seq) and row["status"] == "valid"}
        assert found == expected, found
    assert metric(rows, "state.symbol_count.v1")["status"] == "unsupported"

    offsets = [len(prefix) + sum(map(len, chunks[:i])) for i in range(len(chunks))]
    index = list(csv.DictReader(io.StringIO(run(args.inspect, "--index", trace))))
    assert len(index) == len(chunks)
    for seq, (row, offset, encoded) in enumerate(zip(index, offsets, chunks)):
        fields = ["type", "version", "flags", "sequence", "simulated_time", "work",
                  "raw_bytes", "payload_bytes", "crc32c"]
        wire = dict(zip(fields, struct.unpack("<HHIQdQIII", encoded[:44])))
        del wire["raw_bytes"]
        observed = {key: float(row[key]) if key == "simulated_time" else int(row[key])
                    for key in wire}
        assert observed == wire and observed["sequence"] == seq, row
        assert int(row["header_offset"]) == offset, row

    trace.write_bytes(data[:offsets[2] + 46])
    partial = run(args.inspect, "--index", "--recover-prefix", trace, code=3)
    assert list(csv.DictReader(io.StringIO(partial))) == index[:2]
    run(args.inspect, "--index", trace, code=1)

    malformed = [
        relational_payload([(0, 2), *entities[1:]], relations, ports),
        relational_payload([entities[0], entities[0], entities[2]], relations, ports),
        relational_payload(entities, [(0, 7, 0, 2, 0), relations[1]], ports),
        relational_payload(entities, [relations[0], (40, 7, 2, 3, 1)], ports),
        relational_payload(entities, relations, [(999, 0), *ports[1:]]),
        relational_payload(entities, [(40, 7, 1, 2, 0), relations[1]], ports),
        relational_payload(entities, [relations[0], (50, 7, 2, 4, 1)], ports),
        relational_payload(entities, [(40, 7, 0, 2, 2), relations[1]], ports),
        relational_payload(entities, [relations[0], (50, 7, 2, 2, 1)], ports),
        struct.pack("<3I", 0xFFFFFFFF, 0, 0),
        payload + b"unexpected",
    ]
    for invalid_payload in malformed:
        trace.write_bytes(prefix + chunk(4, 0, 0, 0, invalid_payload) + chunk(0, 1, 0, 0))
        rejected = run(args.inspect, "--index", "--recover-prefix", trace, code=1)
        assert not list(csv.DictReader(io.StringIO(rejected))), rejected

    return {
        "sha256": hashlib.sha256(data).hexdigest(),
        "verified_header_offsets": offsets,
        "valid_snapshot_diagnostics": len(expected),
        "malformed_rejected": len(malformed),
    }


def detailed_record(work, event_type, flags=0, rule=0, participants=(), created=(),
                    deleted=(), delta=b""):
    ids = (*participants, *created, *deleted)
    return (struct.pack("<QIIQ4I", work, event_type, flags, rule, len(participants),
                        len(created), len(deleted), len(delta))
            + struct.pack(f"<{len(ids)}Q", *ids) + delta)


def checkpoint_payload(content, format_id):
    digest = hashlib.sha256(content).digest()
    return struct.pack("<Q32sI", len(content), digest, len(format_id)) + format_id


def check_temporal(args, directory):
    encoded_events = (
        detailed_record(2, 7, rule=19, participants=(1, 2), created=(2,), delta=b"\x00\xff"),
        detailed_record(4, 8, flags=1, participants=(1, 3), created=(3,), deleted=(1,)),
    )
    events = struct.pack("<I", len(encoded_events)) + b"".join(encoded_events)
    # No file with these bytes is needed by the decoder.
    checkpoint = b"independent checkpoint content"
    digest = hashlib.sha256(checkpoint)
    fmt = b"fixture.checkpoint-v1"
    ref = checkpoint_payload(checkpoint, fmt)
    prefix = header(META | {"encoding": "temporal-fixture", "fixed_sites": False})
    event_chunk = chunk(5, 0, 4, 4, events)
    ref_chunk = chunk(6, 1, 4, 4, ref)
    data = prefix + event_chunk + ref_chunk + chunk(0, 2, 4, 4)
    trace = directory / "temporal.cltr"
    trace.write_bytes(data)

    rows = list(csv.DictReader(io.StringIO(run(args.eval, trace))))
    counts = {"events": 2, "births": 2, "deaths": 1,
              "observed_events": 1, "reconstructed_events": 1}
    for metric_name, value in counts.items():
        row = metric(rows, f"activity.{metric_name}.v1", 0)
        assert (row["value"], row["status"]) == (str(value), "valid")
    assert not any(row["sequence"] == "1" for row in rows)
    refs = list(csv.DictReader(io.StringIO(run(args.inspect, "--checkpoints", trace))))
    assert refs == [{"sequence": "1", "work": "4", "format": fmt.decode(),
                     "size_bytes": str(len(checkpoint)), "sha256": digest.hexdigest()}]
    trace.write_bytes(prefix + event_chunk + ref_chunk[:-1])
    index = run(args.inspect, "--recover-prefix", "--index", trace, code=3)
    assert [row["type"] for row in csv.DictReader(io.StringIO(index))] == ["5"]
    assert not list(csv.DictReader(io.StringIO(
        run(args.inspect, "--recover-prefix", "--checkpoints", trace, code=3))))

    invalid_batches = [
        b"",
        struct.pack("<I", 0xffffffff),
        struct.pack("<I", 2) + encoded_events[0] + encoded_events[1][:-1],
        struct.pack("<I", 2) + b"".join(reversed(encoded_events)),
        struct.pack("<I", 1) + detailed_record(5, 1),
        struct.pack("<I", 1) + detailed_record(2, 1, flags=2),
        struct.pack("<I", 1) + detailed_record(2, 1, participants=(0,)),
        struct.pack("<I", 1) + detailed_record(2, 1, participants=(9, 9)),
        struct.pack("<I", 1) + detailed_record(2, 1, created=(9,), deleted=(9,)),
        struct.pack("<I", 1) + struct.pack("<QIIQ4I", 0, 0, 0, 0, 0, 0, 0, 0xffffffff),
        events + b"trailing",
    ]
    invalid_refs = [b"", ref[:-1], ref + b"trailing", bytes(8) + ref[8:],
                    ref[:40] + struct.pack("<I", 0),
                    ref[:40] + struct.pack("<I", 129) + b"x" * 129,
                    ref[:44] + b"/" + fmt[1:]]
    for kind, payloads in ((5, invalid_batches), (6, invalid_refs)):
        for malformed in payloads:
            trace.write_bytes(prefix + chunk(kind, 0, 4, 4, malformed) + chunk(0, 1, 4, 4))
            rejected = run(args.inspect, "--recover-prefix", "--index", trace, code=1)
            assert not list(csv.DictReader(io.StringIO(rejected))), rejected
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "checkpoint_sha256": digest.hexdigest(),
        "counts": counts,
        "invalid_event_batches_rejected": len(invalid_batches),
        "invalid_checkpoint_refs_rejected": len(invalid_refs),
    }


def check_checkpoint_targets(args, directory):
    small = b"independent checkpoint content"
    large = bytes(range(256)) * 513 + b"\x00"
    format_id = b"fixture.checkpoint-v1"
    end = chunk(0, 3, 2, 2)
    prefix = (header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 0, 0, 0))
              + chunk(6, 1, 1, 1, checkpoint_payload(small, format_id))
              + chunk(6, 2, 2, 2, checkpoint_payload(large, format_id)))
    trace = directory / "checkpoint-targets.cltr"
    trace.write_bytes(prefix + end)
    target = directory / "checkpoint target.bin"

    def verify(sequence, path=target):
        return subprocess.run(
            [str(args.inspect), "--verify-checkpoint", str(sequence), str(path), str(trace)],
            capture_output=True, text=True, timeout=15,
        )

    matches = []
    for sequence, content in ((1, small), (2, large)):
        target.write_bytes(content)
        result = verify(sequence)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == ["status=verified", f"sequence={sequence}"]
        matches.append({"sequence": sequence, "size_bytes": len(content),
                        "sha256": hashlib.sha256(content).hexdigest()})

    rejections = []

    def reject(label, message, sequence=1, path=target, code=1):
        result = verify(sequence, path)
        assert result.returncode == code and not result.stdout, result
        assert message in result.stderr, result.stderr
        rejections.append(label)

    for label, content, message in (
        ("same-size changed bytes", b"X" + small[1:], "SHA-256 mismatch"),
        ("shorter target", small[:-1], "size mismatch"),
        ("longer target", small + b"X", "size mismatch"),
    ):
        target.write_bytes(content)
        reject(label, message)
    target.write_bytes(small)
    reject("absent target", "could not open", path=directory / "missing")
    reject("directory target", "must be a regular file", path=directory)
    pipe = directory / "pipe"
    os.mkfifo(pipe)
    reject("FIFO target", "must be a regular file", path=pipe)
    reject("aggregate event sequence", "no checkpoint reference", sequence=0)
    reject("absent sequence", "no checkpoint reference", sequence=900)
    reject("invalid sequence", "invalid checkpoint sequence", sequence="1x", code=2)
    reject("overflowing sequence", "invalid checkpoint sequence", sequence=2**64, code=2)

    trace.write_bytes(prefix + end[:-1])
    reject("truncated trace after matching reference", "before a complete end chunk")
    trace.write_bytes(prefix + end[:-1] + bytes([end[-1] ^ 1]))
    reject("corrupt trace after matching reference", "checksum mismatch")
    return {
        "trace_sha256": hashlib.sha256(prefix + end).hexdigest(),
        "matches": matches,
        "rejections": rejections,
    }


def read_fixture_prefix(data, run_id, last_work, complete):
    magic, major, minor, marker, metadata_bytes = struct.unpack_from("<4sHHII", data)
    assert (magic, major, minor, marker) == (b"CLTR", 1, 0, 0x01020304)
    offset = 16 + metadata_bytes
    assert json.loads(data[16:offset])["run_id"] == run_id
    sequence = 0
    last_cells = None
    processed_end = False
    while offset < len(data):
        raw = data[offset:offset + 44]
        kind, version, flags, seq, time, work, raw_size, size, crc = struct.unpack("<HHIQdQIII", raw)
        offset += 44
        body = data[offset:offset + size]
        offset += size
        assert len(body) == raw_size == size
        assert crc32c(raw[:40] + body) == crc
        assert (version, flags, seq) == (1, 0, sequence)
        if kind == 0:
            assert complete and sequence == 2 * (last_work + 1)
            assert size == 0 and work == time == last_work and offset == len(data)
            processed_end = True
            break
        assert work == time == sequence // 2 <= last_work
        if sequence % 2 == 0:
            assert kind == 1
            cells = struct.unpack("<9I", body)
            assert all(cell in (0, 1) for cell in cells)
            if last_cells is not None:
                assert cells == tuple(last_cells[(i - 1) % 9] ^ last_cells[(i + 1) % 9]
                                      for i in range(9))
            last_cells = cells
        else:
            assert kind == 3
            assert struct.unpack("<6Q", body) == (work, 18 * work, 9 * work, 9, 72, 72)
        sequence += 1
    assert sequence == 2 * (last_work + 1)
    assert processed_end == complete


def check_worker_panel(args, directory):
    panel = (
        ("work", 0, "capped", "work", 5, "complete"),
        ("completed", 0, "completed", "", 8, "complete"),
        ("wall-cap", 0, "capped", "wall", 2, "incomplete"),
        ("crash", 1, "failed", "signal", 3, "incomplete"),
        ("memory-cap", 0, "capped", "memory", None, "missing"),
    )
    # Frozen in the macOS receipt in docs/E00-worker-results.md.
    expected_digests = {
        "work": "890dcb152bc02ec22bb4754fefbd69a6bea966239fbdc87df7d3c72110780ff1",
        "completed": "98ad5e901b0e96ef29279dc7cc1073b94af79bd85a3b290d204e66f1149426d4",
        "wall-cap": "f81de7a7abd230b6846ae7d9d7297323a07cca041916bada4ec87845e3cf7c0c",
        "crash": "0ab5bdc3857c879cb044606b004a5c52195f897752aba0260ec8c191829bf92f",
    }
    results = {}
    for scenario, exit_code, status, reason, last_work, trace_status in panel:
        output = directory / scenario
        argv = (output,) if scenario == "work" else ("--scenario", scenario, output)
        run(args.fixture, *argv, code=exit_code)
        with (output / "planned.csv").open() as file:
            planned, = csv.DictReader(file)
        with (output / "status.csv").open() as file:
            receipt, = csv.DictReader(file)
        assert receipt["run_id"] == planned["run_id"]
        assert (receipt["status"], receipt["reason"], receipt["trace_status"]) == (
            status, reason, trace_status,
        ), receipt
        assert receipt["completed_work"] == ("" if last_work is None else str(last_work)), receipt
        assert planned["target_work"] == "8"
        assert planned["hard_work_cap"] == ("5" if scenario == "work" else "8")
        assert planned["cell_buffer_cap"] == ("71" if scenario == "memory-cap" else "128")
        assert planned["wall_cap_ms"] == ("1500" if scenario == "wall-cap" else "5000")
        assert not list(output.glob("*.csv.tmp")), output

        signal_case = scenario in {"wall-cap", "crash"}
        if signal_case:
            assert receipt["worker_signal"] == str(signal.SIGKILL.value)
            assert receipt["worker_exit"] == ""
            assert (int(receipt["elapsed_ms"]) >= int(planned["wall_cap_ms"])) == (reason == "wall")
        else:
            assert receipt["worker_exit"] == ("2" if scenario == "memory-cap" else "0"), receipt
            assert receipt["worker_signal"] == ""

        row = {
            "status": status, "reason": reason, "completed_work": last_work,
            "trace_status": trace_status, "elapsed_ms": int(receipt["elapsed_ms"]),
            "worker_exit": receipt["worker_exit"], "worker_signal": receipt["worker_signal"],
        }
        if trace_status == "missing":
            assert receipt["trace"] == ""
            assert not list(output.rglob("*.cltr")) and not list(output.rglob("*.partial"))
        else:
            trace = output / receipt["trace"]
            complete = trace_status == "complete"
            data = trace.read_bytes()
            row["trace_sha256"] = hashlib.sha256(data).hexdigest()
            assert row["trace_sha256"] == expected_digests[scenario], row
            read_fixture_prefix(data, planned["run_id"], last_work, complete)
            index = list(csv.DictReader(io.StringIO(run(
                args.inspect, "--index", "--recover-prefix", trace, code=0 if complete else 3,
            ))))
            assert len(index) == 2 * (last_work + 1) + complete
            assert max(int(record["work"]) for record in index) == last_work
            if complete:
                assert receipt["trace"] == "trace.cltr" and not list(output.rglob("*.partial"))
            else:
                assert trace.name == "trace.partial" and not (output / "trace.cltr").exists()
                assert not run(args.inspect, trace, code=1)
        results[scenario] = row

    rows = list(csv.DictReader(io.StringIO(run(args.eval, directory / "work" / "trace.cltr"))))
    assert {row["run_id"] for row in rows} == {"mock-work-cap"}
    assert max(int(row["work"]) for row in rows if row["work"]) == 5
    assert metric(rows, "work.logical_updates.v1", 11)["value"] == "5"
    return results


def check_tier0_panel(args, directory):
    receipts = {}

    def evaluate_trace(name, records, fixed_sites=False, decorate=False):
        # Optional records share the observation stamp without changing its work.
        chunks = []
        for work, (kind, payload) in enumerate(records):
            if decorate:
                chunks.append(chunk(99, len(chunks), work, work, b"frame", flags=1))
            chunks.append(chunk(kind, len(chunks), work, work, payload))
        chunks.append(chunk(0, len(chunks), len(records) - 1, len(records) - 1))
        data = (header(META | {"run_id": name, "fixed_sites": fixed_sites})
                + b"".join(chunks))
        trace = directory / f"{name}.cltr"
        trace.write_bytes(data)
        receipts[name] = hashlib.sha256(data).hexdigest()
        return list(csv.DictReader(io.StringIO(run(args.eval, trace))))

    def cat(symbols):
        return 1, struct.pack(f"<{len(symbols)}I", *symbols)

    def graph(entity_count, endpoints):
        # Each tuple defines one relation, including duplicate and zero-port cases.
        relation_rows = []
        port_rows = []
        for index, incident in enumerate(endpoints, 1):
            relation_rows.append((index, 1, len(port_rows), len(incident), 0))
            port_rows.extend((entity, 0) for entity in incident)
        entities = [(index, 1) for index in range(1, entity_count + 1)]
        return 4, relational_payload(entities, relation_rows, port_rows)

    def assert_value(rows, name, sequence, expected):
        row = metric(rows, name, sequence)
        assert (row["value"], row["status"]) == (str(expected), "valid"), row

    rows = evaluate_trace("categorical-edges", [cat([0] * 8), cat([]), cat(list(range(16)))])
    for seq, longest in enumerate((8, 0, 1)):
        assert_value(rows, "state.longest_symbol_run.v1", seq, longest)
    for seq, entropy in ((0, 0), (2, 4)):
        assert_value(rows, "state.shannon_bits_per_symbol.v1", seq, entropy)
    empty = metric(rows, "state.shannon_bits_per_symbol.v1", 1)
    assert (empty["value"], empty["status"]) == ("", "insufficient_data")

    def assert_incidence(rows, sequence, component_count, degrees):
        assert_value(rows, "relational.component_count.v1", sequence, component_count)
        counts = {
            int(row["symbol"]): int(row["value"]) for row in rows
            if row["metric_id"] == "relational.port_degree_count.v1"
            and row["sequence"] == str(sequence) and row["status"] == "valid"
        }
        assert counts == degrees, counts

    copies = (1, 4, 16, 64)
    tiled = [graph(3 * n, [(3 * i + 1, 3 * i + 2, 3 * i + 3) for i in range(n)])
             for n in copies]
    rows = evaluate_trace("repeated-relation", tiled)
    for seq, n in enumerate(copies):
        assert_incidence(rows, seq, n, {1: 3 * n})

    sizes = (1, 2, 4, 8)
    rows = evaluate_trace("growing-path", [graph(n, [(i, i + 1) for i in range(1, n)])
                                           for n in sizes])
    for seq, degrees in enumerate(({0: 1}, {1: 2}, {1: 2, 2: 2}, {1: 2, 2: 6})):
        assert_incidence(rows, seq, 1, degrees)

    modular = graph(5, [(1, 1, 2), (3, 4), ()])
    rows = evaluate_trace("frozen-modules", [graph(0, []), modular, modular])
    assert_incidence(rows, 0, 0, {})
    for seq in (1, 2):
        assert_incidence(rows, seq, 3, {0: 1, 1: 3, 2: 1})
        assert_value(rows, "relational.port_count.v1", seq, 5)

    phases = [[0, 0, 1], [1, 0, 0], [0, 1, 0]]
    oscillator = [cat(phases[i % 3]) for i in range(7)]
    plain = evaluate_trace("period-three", oscillator, fixed_sites=True)
    framed = evaluate_trace("period-three-framed", oscillator, fixed_sites=True, decorate=True)
    for seq, lag in enumerate((0, 0, 3, 3, 3, 3), 1):
        assert_value(plain, "state.recurrence_lag_snapshots.v1", seq, lag)
        assert_value(plain, "state.changed_sites.v1", seq, 2)
    first = metric(plain, "state.recurrence_lag_snapshots.v1", 0)
    assert (first["value"], first["status"]) == ("", "insufficient_data")

    def by_observation(rows):
        return [{key: value for key, value in row.items() if key not in {"run_id", "sequence"}}
                for row in rows]

    assert by_observation(plain) == by_observation(framed)
    return receipts


def check_provenance(args, directory):
    config = b'{"steps":4}'
    provenance = {
        "config_sha256": hashlib.sha256(config).hexdigest(),
        "seed": 2**64 - 1,
        "producer": "external-é.v2",
        "backend": "portable-cpu",
        "precision": "u32-categorical",
        "capabilities": {
            "native": True, "relational": False, "temporal": True,
            "stable_entity_ids": False, "implicit_relations": True, "interventions": False,
            "time_domain": "continuous",
        },
    }
    expected = META | {"provenance": provenance}
    supplied = expected | {"future_extension": [{"kind": 1}, {"kind": 2}]}
    events = chunk(2, 0, 0.5, 1, struct.pack("<3Q", 1, 0, 0))
    end = chunk(0, 1, 0.5, 1)
    raw = header(supplied) + events + end
    trace = directory / "provenance.cltr"
    trace.write_bytes(raw)
    normalized = json.loads(run(args.inspect, "--metadata", trace))
    assert normalized == expected, normalized
    rows = list(csv.DictReader(io.StringIO(run(args.eval, trace))))
    assert metric(rows, "activity.events.v1", 0)["value"] == "1"

    for seed in (0, None):
        version = deepcopy(expected)
        if seed is None:
            del version["provenance"]["seed"]
        else:
            version["provenance"]["seed"] = seed
        trace.write_bytes(header(version) + events + end)
        assert json.loads(run(args.inspect, "--metadata", trace)) == version
    trace.write_bytes(header() + events + end)
    assert json.loads(run(args.inspect, "--metadata", trace)) == META

    trace.write_bytes(raw[:-1])
    assert not run(args.inspect, "--metadata", trace, code=1)
    recovered = run(args.inspect, "--metadata", "--recover-prefix", trace, code=3)
    assert json.loads(recovered) == expected

    malformed = [header(META | {"provenance": False})]

    def invalid_field(key, value):
        version = deepcopy(expected)
        version["provenance"][key] = value
        malformed.append(header(version))

    missing_field = deepcopy(expected)
    del missing_field["provenance"]["precision"]
    malformed.append(header(missing_field))
    for config_hash in ("a" * 63, "A" * 64, "g" * 64):
        invalid_field("config_sha256", config_hash)
    for seed in (-1, 2**64, 1.0, "0", True, None):
        invalid_field("seed", seed)
    invalid_field("producer", "")
    invalid_field("capabilities", provenance["capabilities"] | {"time_domain": "event"})
    invalid_field("capabilities", provenance["capabilities"] | {"stable_entity_ids": 1})

    serialized = json.dumps(expected, separators=(",", ":")).encode()
    malformed.extend([
        metadata_header(b'{"run_id":"duplicate",' + serialized[1:]),
        metadata_header(serialized.replace(b'"native":true', b'"native":false,"native":true')),
    ])
    for prefix in malformed:
        trace.write_bytes(prefix + events + end)
        assert not run(args.inspect, "--metadata", trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(raw).hexdigest(),
        "config_sha256": provenance["config_sha256"],
        "exact_seed_decimal": str(provenance["seed"]),
        "malformed_metadata_rejected": len(malformed),
    }


def check(args, directory):
    assert crc32c(b"123456789") == 0xE3069283
    first = struct.pack("<4I", 0, 0, 1, 1)
    second = struct.pack("<4I", 0, 1, 0, 1)
    trace = directory / "categorical.cltr"
    trace.write_bytes(header() + chunk(1, 0, 0, 0, first)
                      + chunk(99, 1, 0, 0, b"ignored", flags=1)
                      + chunk(1, 2, 1, 1, second) + chunk(1, 3, 2, 2, first)
                      + chunk(0, 4, 2, 2))
    rows = list(csv.DictReader(io.StringIO(run(args.eval, trace))))
    expected = (
        ("state.symbol_count.v1", 0, "", "4"),
        ("state.type_count.v1", 0, 0, "2"),
        ("state.type_count.v1", 0, 1, "2"),
        ("state.shannon_bits_per_symbol.v1", 0, "", "1"),
        ("state.rle_bytes_per_raw_byte.v1", 0, "", "0.625"),
        ("state.changed_sites.v1", 2, "", "2"),
        ("state.recurrence_lag_snapshots.v1", 2, "", "0"),
        ("state.recurrence_lag_snapshots.v1", 3, "", "2"),
    )
    observed = []
    for name, sequence, symbol, value in expected:
        row = metric(rows, name, sequence, symbol)
        assert row["status"] == "valid" and row["value"] == value, row
        assert row["run_id"] == META["run_id"] and row["exact"] == "true", row
        observed.append([name, sequence, str(symbol), value, row["value"]])
    assert metric(rows, "state.changed_sites.v1", 0)["status"] == "insufficient_data"
    assert run(args.inspect, trace).splitlines() == ["status=complete", "records=3", "chunks=5"]

    events = directory / "events.cltr"
    events.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 7, 2, 3))
                       + chunk(0, 1, 0, 0))
    rows = list(csv.DictReader(io.StringIO(run(args.eval, events))))
    for name, value in (("events", "7"), ("births", "2"), ("deaths", "3")):
        row = metric(rows, f"activity.{name}.v1", 0)
        assert (row["status"], row["value"]) == ("valid", value)
    assert metric(rows, "state.symbol_count.v1")["status"] == "unsupported"
    assert metric(rows, "state.symbol_count.v1")["value"] == ""
    assert metric(rows, "activity.observed_events.v1", 0)["status"] == "unsupported"
    assert metric(rows, "activity.reconstructed_events.v1", 0)["status"] == "unsupported"

    broken = directory / "broken.cltr"
    data = trace.read_bytes()
    broken.write_bytes(data[:-6])
    assert run(args.inspect, "--recover-prefix", broken, code=3).splitlines() == [
        "status=incomplete", "records=3", "chunks=4",
    ]
    run(args.eval, broken, code=1)
    malformed = (
        data + b"trailing",
        data[:-1] + bytes([data[-1] ^ 1]),
        b"bad!" + data[4:],
        header({}) + chunk(0, 0, 0, 0),
        header() + chunk(99, 0, 0, 0),
        header() + chunk(1, 0, 0, 0, b"odd"),
        header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 0, 1, 0)),
        header() + chunk(1, 1, 0, 0, first),
        header() + chunk(1, 0, float("nan"), 0, first),
        header() + struct.pack("<HHIQdQIII", 1, 1, 0, 0, 0., 0, 4194308, 4194308, 0),
        header() + chunk(3, 0, 0, 0, struct.pack("<6Q", 0, 0, 0, 0, 9, 8)),
        header() + chunk(0, 0, 0, 0, version=2),
    )
    for malformed_trace in malformed:
        broken.write_bytes(malformed_trace)
        run(args.inspect, "--recover-prefix", broken, code=1)

    result = {
        "categorical_sha256": hashlib.sha256(data).hexdigest(),
        "events_sha256": hashlib.sha256(events.read_bytes()).hexdigest(),
        "categorical_checks": observed,
        "event_checks": {"events": 7, "births": 2, "deaths": 3},
        "malformed_rejected": len(malformed),
        "relational": check_relational(args, directory),
        "temporal": check_temporal(args, directory),
        "checkpoint_targets": check_checkpoint_targets(args, directory),
        "tier0_panel": check_tier0_panel(args, directory),
        "provenance": check_provenance(args, directory),
    }
    assert result["categorical_sha256"] == "3dda44df0c60342ddd2b6456bbb350ea66a50141d86dc62ba4e887a0000fc9ba"
    assert result["events_sha256"] == "5f5e31d67be90e5eea677c262b01358df9cc2427ff86a17d975c44f4d34ddac1"
    assert result["relational"]["sha256"] == "b9d66dd6527a31254bd27f97840a345fde40c5b100507d0def7720ecd410412d"
    assert result["temporal"]["trace_sha256"] == "602af49f1f3105f4a1db42c61c69e67fdb9235f48a194863f8500bb0725f4e60"
    if args.fixture:
        result["worker_panel"] = check_worker_panel(args, directory)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--eval", type=Path, required=True)
    parser.add_argument("--inspect", type=Path, required=True)
    parser.add_argument("--fixture", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        print(json.dumps(check(args, Path(directory)), indent=2))


if __name__ == "__main__":
    main()
