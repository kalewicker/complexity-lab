"""Produce CLTR bytes without using any C++ implementation code."""
import argparse
import csv
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import hashlib
import heapq
import io
from itertools import groupby
from math import comb, factorial, prod
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from cltr_fixture import META, chunk, crc32c, header, metadata_header


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


def check_attributes(args, directory):
    schema = [
        {"owner": "entity", "name": "phase", "kind": "categorical", "required": True,
         "labels": ["idle", "active"]},
        {"owner": "relation", "name": "length", "kind": "continuous", "required": True,
         "unit": "m", "lower": 0, "upper": 10, "resolution": 0.125},
        {"owner": "entity", "name": "energy", "kind": "continuous", "required": False,
         "unit": "J", "lower": -4, "upper": 4, "resolution": 0.25},
    ]
    declared = META | {"relational_attributes": schema}
    topology = relational_payload([(30, 1), (9, 2)], [(4, 1, 0, 2, 1)], [(30, 0), (9, 1)])
    empty = relational_payload([], [], [])
    slots = (struct.pack("<BQ", 1, 1), struct.pack("<BQ", 1, 0),
             struct.pack("<Bd", 1, 1.75), bytes(9), struct.pack("<Bd", 1, -2.25))
    prefix = header(declared)
    payload = topology + b"".join(slots)
    records = chunk(4, 0, 0, 0, payload, version=2) + chunk(4, 1, 1, 1, empty, version=2)
    end = chunk(0, 2, 1, 1)
    data = prefix + records + end
    trace = directory / "attributes.cltr"
    trace.write_bytes(data)
    attrs = list(csv.DictReader(io.StringIO(run(args.inspect, "--attributes", trace))))
    expected = [
        ("0", "0", "entity", "30", "1", "present"),
        ("0", "0", "entity", "9", "0", "present"),
        ("0", "1", "relation", "4", "1.75", "present"),
        ("0", "2", "entity", "30", "", "missing"),
        ("0", "2", "entity", "9", "-2.25", "present"),
    ]
    fields = ("sequence", "column", "owner", "owner_id", "value", "status")
    assert [tuple(row[field] for field in fields) for row in attrs] == expected, attrs
    assert json.loads(run(args.inspect, "--metadata", trace)) == declared
    index = csv.DictReader(io.StringIO(run(args.inspect, "--index", trace)))
    assert [(row["type"], row["version"], row["flags"]) for row in index] == [
        ("4", "2", "0"), ("4", "2", "0"), ("0", "1", "0"),
    ]
    actual = run(args.eval, trace)
    trace.write_bytes(header() + chunk(4, 0, 0, 0, topology) + chunk(4, 1, 1, 1, empty) + end)
    assert run(args.eval, trace) == actual
    assert not list(csv.DictReader(io.StringIO(run(args.inspect, "--attributes", trace))))

    def replace_slot(index, replacement):
        modified = list(slots)
        modified[index] = replacement
        return topology + b"".join(modified)

    first_end = chunk(0, 1, 0, 0)
    malformed = [
        prefix + chunk(4, 0, 0, 0, topology),
        header() + chunk(4, 0, 0, 0, payload, version=2),
        prefix + chunk(4, 0, 0, 0, payload, version=2, flags=1),
    ]
    for body in (
        payload[:-1], payload + b"X",
        replace_slot(0, struct.pack("<BQ", 2, 1)),
        replace_slot(3, struct.pack("<BQ", 0, 1)),
        replace_slot(0, bytes(9)),
        replace_slot(0, struct.pack("<BQ", 1, 2)),
        replace_slot(0, struct.pack("<BQ", 1, 2**32)),
        replace_slot(2, struct.pack("<Bd", 1, 11.0)),
        replace_slot(2, struct.pack("<Bd", 1, float("nan"))),
        replace_slot(4, struct.pack("<Bd", 1, float("inf"))),
    ):
        malformed.append(prefix + chunk(4, 0, 0, 0, body, version=2))

    for bad_schema in (
        [], {}, schema + [schema[0]],
        [schema[0] | {"labels": ["idle", "idle"]}],
        [schema[0] | {"labels": []}],
        [schema[0] | {"labels": {"0": "idle"}}],
        [schema[0] | {"owner": "port"}],
        [schema[0] | {"required": 1}],
        [schema[1] | {"unit": ""}],
        [schema[1] | {"lower": 11}],
        [schema[1] | {"resolution": 0}],
        [schema[1] | {"resolution": "0.125"}],
    ):
        malformed.append(header(META | {"relational_attributes": bad_schema})
                         + chunk(4, 0, 0, 0, payload, version=2))

    for invalid in malformed:
        trace.write_bytes(invalid + first_end)
        rejected = run(args.inspect, "--index", "--recover-prefix", trace, code=1)
        assert not list(csv.DictReader(io.StringIO(rejected))), rejected
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "slot_count": len(expected),
        "malformed_rejected": len(malformed),
    }


def check_wire_compatibility(args, directory):
    metadata = json.dumps(META, separators=(",", ":")).encode()
    future_metadata = json.dumps(META | {"future_hint": "optional"},
                                 separators=(",", ":")).encode()
    past = struct.pack("<2I", 0, 1)
    later = struct.pack("<2I", 1, 0)
    first = chunk(1, 0, 0, 0, past)
    baseline = metadata_header(metadata) + first + chunk(1, 1, 1, 1, later) + chunk(0, 2, 1, 1)
    new_prefix = metadata_header(future_metadata, minor=1)
    add_kind = chunk(85, 1, 0, 0, b"additional kind", flags=1)
    add_version = chunk(1, 2, 0, 0, b"additional version", flags=1, version=2)
    extended = (new_prefix + first + add_kind + add_version
                + chunk(1, 3, 1, 1, later) + chunk(0, 4, 1, 1))
    trace = directory / "minor-compatibility.cltr"

    def observations(data):
        trace.write_bytes(data)
        rows = csv.DictReader(io.StringIO(run(args.eval, trace)))
        return [{key: value for key, value in row.items() if key != "sequence"} for row in rows]

    assert observations(baseline) == observations(extended)
    assert json.loads(run(args.inspect, "--metadata", trace)) == META
    index = csv.DictReader(io.StringIO(run(args.inspect, "--index", trace)))
    assert [(row["sequence"], row["type"], row["version"], row["flags"]) for row in index] == [
        ("0", "1", "1", "0"), ("1", "85", "1", "1"), ("2", "1", "2", "1"),
        ("3", "1", "1", "0"), ("4", "0", "1", "0"),
    ]

    bad_kind = chunk(85, 1, 0, 0, b"required kind")
    bad_version = chunk(1, 1, 0, 0, b"required version", version=2)
    corrupt = add_kind[:-1] + bytes([add_kind[-1] ^ 1])
    for label, data in (
        ("unknown required kind", new_prefix + first + bad_kind + chunk(0, 2, 0, 0)),
        ("unknown required version", new_prefix + first + bad_version + chunk(0, 2, 0, 0)),
        ("wrong major", metadata_header(metadata, major=2) + first + chunk(0, 1, 0, 0)),
        ("corrupt optional kind", new_prefix + first + corrupt + chunk(0, 2, 0, 0)),
    ):
        trace.write_bytes(data)
        assert not run(args.inspect, "--metadata", "--recover-prefix", trace, code=1), label

    return {
        "baseline_sha256": hashlib.sha256(baseline).hexdigest(),
        "extended_sha256": hashlib.sha256(extended).hexdigest(),
        "optional_chunks_skipped": 2,
        "incompatible_or_corrupt_rejected": 4,
    }


def registry_fixture():
    graph = relational_payload([(7, 1), (11, 2)], [(23, 1, 0, 2, 0)], [(7, 0), (11, 0)])
    return (header()
            + chunk(1, 0, 0, 0, struct.pack("<4I", 0, 0, 1, 1))
            + chunk(4, 1, 0, 0, graph)
            + chunk(2, 2, 0, 0, struct.pack("<3Q", 4, 2, 1))
            + chunk(3, 3, 0, 0, struct.pack("<6Q", 0, 4, 0, 2, 16, 20))
            + chunk(1, 4, 1, 1, struct.pack("<4I", 1, 1, 0, 0))
            + chunk(0, 5, 1, 1))


def check_registry(args, directory):
    by_channel = {
        "categorical": [
            "state.symbol_count", "state.shannon_bits_per_symbol", "state.type_count",
            "state.rle_bytes_per_raw_byte", "state.longest_symbol_run", "state.changed_sites",
            "state.recurrence_lag_snapshots", "state.recurrence_lookback_snapshots",
        ],
        "relational": [
            "relational.entity_count", "relational.relation_count", "relational.port_count",
            "relational.entity_type_count", "relational.relation_type_count",
            "relational.arity_count", "relational.component_count", "relational.port_degree_count",
        ],
        "events": [
            "activity.events", "activity.births", "activity.deaths", "activity.observed_events",
            "activity.reconstructed_events",
        ],
        "ledger": [
            "work.logical_updates", "work.reads", "work.writes", "work.rng_draws",
            "resource.live_state_bytes", "resource.peak_state_bytes",
        ],
    }
    catalog = list(csv.DictReader(io.StringIO(run(args.eval, "--list-metrics"))))
    expected = [(f"{name}.v1", channel) for channel, metrics in by_channel.items() for name in metrics]
    assert [(row["metric_id"], row["channel"]) for row in catalog] == expected
    assert all(row["unit"] for row in catalog)
    assert len({row["metric_id"] for row in catalog}) == 27
    assert catalog[1]["unit"] == "bits_per_symbol"
    assert catalog[-1]["unit"] == "bytes"

    trace = directory / "registry.cltr"
    data = registry_fixture()
    trace.write_bytes(data)
    default_csv = run(args.eval, trace)
    default_digest = hashlib.sha256(default_csv.encode()).hexdigest()
    # Captured from the pre-registry G0 evaluator.
    assert default_digest == "a1129d06e5de0595b308416bebc7fc7a1ee81029b107504ac6f5dd4ab8b2001b"
    default = list(csv.DictReader(io.StringIO(default_csv)))

    def select(*ids):
        return run(args.eval, *(arg for metric_id in ids for arg in ("--metric", metric_id)), trace)

    assert select(*(row["metric_id"] for row in catalog)) == default_csv
    selected = ("resource.live_state_bytes.v1", "activity.observed_events.v1",
                "activity.events.v1", "relational.port_degree_count.v1",
                "state.recurrence_lag_snapshots.v1", "state.shannon_bits_per_symbol.v1",
                "state.shannon_bits_per_symbol.v1")
    rows = list(csv.DictReader(io.StringIO(select(*selected))))
    assert rows == [row for row in default if row["metric_id"] in selected]
    assert {row["status"] for row in rows} == {"valid", "unsupported", "insufficient_data"}
    for metric_id in dict(expected):
        per_metric = list(csv.DictReader(io.StringIO(select(metric_id))))
        assert per_metric == [row for row in default if row["metric_id"] == metric_id]

    for options in (("--metric", "unknown.v1", directory / "missing"),
                    ("--metric",), ("--list-metrics", trace)):
        assert not run(args.eval, *options, code=2)

    trace.write_bytes(data[:-1])
    run(args.eval, "--metric", "activity.events.v1", trace, code=1)
    trace.write_bytes(header() + chunk(1, 0, 0, 0, struct.pack("<I", 0))
                      + chunk(1, 1, 1, 1, struct.pack("<2I", 0, 1)) + chunk(0, 2, 1, 1))
    run(args.eval, "--metric", "work.reads.v1", trace, code=1)
    return {
        "catalog_size": len(catalog),
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "default_csv_sha256": default_digest,
        "selected_rows": len(rows),
        "single_metric_projections": len(expected),
    }


def check_dependency_plan(args):
    def plan(*ids):
        flags = [arg for metric_id in ids for arg in ("--metric", metric_id)]
        raw = run(args.eval, "--plan-metrics", *flags)
        return [(row["kind"], row["id"]) for row in csv.DictReader(io.StringIO(raw))]

    symbol_pair = ("state.type_count.v1", "state.shannon_bits_per_symbol.v1")
    symbols = plan(*symbol_pair, symbol_pair[0])
    assert symbols == [
        ("artifact", "artifact.symbol_histogram.v1"),
        ("metric", "state.shannon_bits_per_symbol.v1"),
        ("metric", "state.type_count.v1"),
    ]
    assert plan(*reversed(symbol_pair)) == symbols
    incidence = plan("relational.port_degree_count.v1", "relational.component_count.v1")
    assert incidence == [
        ("artifact", "artifact.entity_index.v1"),
        ("artifact", "artifact.relational_incidence.v1"),
        ("metric", "relational.component_count.v1"),
        ("metric", "relational.port_degree_count.v1"),
    ]
    scalar = plan("work.reads.v1")
    assert scalar == [("metric", "work.reads.v1")]
    full = plan()
    assert len(full) == len(set(full)) == 34
    catalog = csv.DictReader(io.StringIO(run(args.eval, "--list-metrics")))
    assert {id for kind, id in full if kind == "metric"} == {
        row["metric_id"] for row in catalog
    }
    assert not run(args.eval, "--plan-metrics", "--metric", "artifact.entity_index.v1", code=2)
    assert not run(args.eval, "--plan-metrics", "unexpected-path", code=2)
    return {
        "nodes": len(full),
        "shared_histogram_plan": [id for _, id in symbols],
        "incidence_plan": [id for _, id in incidence],
        "ledger_scalar_plan": [id for _, id in scalar],
    }


def check_histogram_cache(args, directory):
    trace = directory / "cache-witness.cltr"
    workspace = directory / "cache"
    entropy_id = "state.shannon_bits_per_symbol.v1"
    type_id = "state.type_count.v1"
    selected = ("--metric", entropy_id, "--metric", type_id)

    def encode_snapshot(symbols, metadata=META):
        payload = struct.pack(f"<{len(symbols)}I", *symbols)
        return header(metadata) + chunk(1, 0, 0, 0, payload) + chunk(0, 1, 0, 0)

    baseline = encode_snapshot([0, 0, 1, 1])
    trace.write_bytes(baseline)

    def cached(cache_dir=workspace, flags=selected):
        result = subprocess.run(
            [str(args.eval), "--cache-dir", str(cache_dir), *flags, str(trace)],
            text=True, capture_output=True, timeout=20,
        )
        assert result.returncode == 0, result.stderr
        marker, *fields = result.stderr.split()
        assert marker == "symbol_histogram_cache", result.stderr
        return result.stdout, {key: int(value) for field in fields
                               for key, value in [field.split("=")]}

    def counters(hit=0, miss=0, stores=0, skipped=0, failures=0):
        return dict(hits=hit, misses=miss, stores=stores, skipped=skipped, write_failures=failures)

    expected = run(args.eval, *selected, trace)
    assert cached() == (expected, counters(miss=1, stores=1))
    assert cached() == (expected, counters(hit=1))
    entropy_only = ("--metric", entropy_id)
    assert cached(flags=entropy_only) == (run(args.eval, *entropy_only, trace), counters(hit=1))

    cache_slots = workspace / "symbol-histogram-v1"
    slot, = cache_slots.glob("*.bin")
    entry = slot.read_bytes()

    def lookup_hash(data):
        value = 14695981039346656037
        for byte in data:
            value = ((value ^ byte) * 1099511628211) % (1 << 64)
        return value

    def exact_key(symbols):
        encoding = META["encoding"].encode()
        return (b"CLH1" + struct.pack("<BII", 1, len(encoding), len(symbols))
                + encoding + struct.pack(f"<{len(symbols)}I", *symbols))

    body = exact_key([0, 0, 1, 1]) + struct.pack("<IIQIQ", 2, 0, 2, 1, 2)
    assert entry == body + struct.pack("<Q", lookup_hash(body))
    assert slot.name == f"{lookup_hash(exact_key([0, 0, 1, 1])) % 64}.bin"

    for damaged in (entry[:-1], entry[:-1] + bytes([entry[-1] ^ 1]), bytes(256 * 1024 + 1)):
        slot.write_bytes(damaged)
        assert cached() == (expected, counters(miss=1, stores=1))
        assert slot.read_bytes() == entry

    for alternate in (
        encode_snapshot([0, 0, 1, 2]),
        encode_snapshot([1, 0, 1, 0]),
        encode_snapshot([0, 0, 1, 1], META | {"encoding": "other-u32"}),
        encode_snapshot([0, 0, 1, 1], META | {"exact": False}),
    ):
        trace.write_bytes(alternate)
        assert cached() == (run(args.eval, *selected, trace), counters(miss=1, stores=1))

    collision = {}
    for symbol in range(65):
        position = lookup_hash(exact_key([symbol])) % 64
        if position in collision:
            pair = collision[position], symbol
            break
        collision[position] = symbol
    else:
        raise AssertionError("pigeonhole collision missing")
    for symbol in (*pair, pair[0]):
        trace.write_bytes(encode_snapshot([symbol]))
        assert cached(directory / "collision") == (
            run(args.eval, *selected, trace), counters(miss=1, stores=1),
        )
    assert cached(directory / "collision")[1] == counters(hit=1)

    trace.write_bytes(baseline)
    shared = directory / "shared"
    with ThreadPoolExecutor(max_workers=2) as pool:
        first, second = [pool.submit(cached, shared) for _ in range(2)]
        assert first.result()[0] == second.result()[0] == expected
    assert cached(shared) == (expected, counters(hit=1))
    assert not list(shared.rglob(".write-*"))

    unavailable = directory / "cache-file"
    unavailable.write_text("ordinary file")
    assert cached(unavailable) == (expected, counters(miss=1, failures=1))
    assert unavailable.read_text() == "ordinary file"

    unrelated = directory / "unrelated"
    scalar = ("--metric", "state.symbol_count.v1")
    assert cached(unrelated, scalar) == (run(args.eval, *scalar, trace), counters())
    assert not unrelated.exists()

    oversize = directory / "oversize"
    trace.write_bytes(encode_snapshot([0] * 65536))
    assert cached(oversize) == (run(args.eval, *selected, trace), counters(skipped=1))
    assert not oversize.exists()
    trace.write_bytes(encode_snapshot(range(20000)))
    assert cached(oversize, entropy_only) == (
        run(args.eval, *entropy_only, trace), counters(miss=1, skipped=1),
    )
    assert not oversize.exists()

    # One trace crosses the slot count without creating unbounded files.
    many = header() + b"".join(chunk(1, i, i, i, struct.pack("<I", i)) for i in range(128))
    trace.write_bytes(many + chunk(0, 128, 127, 127))
    bounded = directory / "bounded"
    assert cached(bounded) == (run(args.eval, *selected, trace), counters(miss=128, stores=128))
    slots = list((bounded / "symbol-histogram-v1").glob("*.bin"))
    assert len(slots) == 64 and all(p.stat().st_size <= 256 * 1024 for p in slots)
    return {
        "trace_sha256": hashlib.sha256(baseline).hexdigest(),
        "csv_sha256": hashlib.sha256(expected.encode()).hexdigest(),
        "cache_entry_sha256": hashlib.sha256(entry).hexdigest(),
        "colliding_symbols": pair,
        "resident_slots_after_128_inputs": len(slots),
    }


def check_categorical_codes(args, directory):
    sequences = [
        [], [2**32 - 1], [3, 9, 3, 9, 3],
        [17] * 40, [2] * 4 + [8] * 10 + [2] * 13,
        [2, 7, 15, 2, 7, 15, 2, 7], list(range(20)),
    ]
    relabeled = [[symbol ^ 0xf10ff10f for symbol in values] for values in sequences]
    data = header(META | {"fixed_sites": False})
    all_samples = sequences + relabeled
    for seq, values in enumerate(all_samples):
        data += chunk(1, seq, seq, seq, struct.pack(f"<{len(values)}I", *values))
    data += chunk(0, len(all_samples), len(all_samples) - 1, len(all_samples) - 1)
    trace = directory / "categorical-codes.cltr"
    trace.write_bytes(data)
    actual_csv = run(args.eval, "--categorical-baselines", trace)
    assert hashlib.sha256(actual_csv.encode()).hexdigest() == (
        "bfc268ab453e184ea40260d50d748ba318c0768c1c758cdc577f30c345ebfd05"
    )
    rows = list(csv.DictReader(io.StringIO(actual_csv)))

    def reference(symbols):
        def gamma(value):
            assert value > 0
            encoded = format(value, "b")
            return "0" * (len(encoded) - 1) + encoded

        def raw(values):
            return "".join(format(value, "032b") for value in values)

        header_bits = 2 + len(gamma(len(symbols) + 1))
        if not symbols:
            return [(header_bits, 0, 0)] * 4
        alphabet = sorted(set(symbols))
        period = next(step for step in range(1, len(symbols) + 1)
                      if all(value == symbols[i % step] for i, value in enumerate(symbols)))
        width = (len(alphabet) - 1).bit_length()
        indices = "".join(format(alphabet.index(symbol), f"0{width}b")
                          for symbol in symbols) if width else ""
        runs = "".join(raw([label]) + gamma(len(list(group))) for label, group in groupby(symbols))
        return [
            (header_bits, 0, len(raw(symbols))),
            (header_bits, len(gamma(len(alphabet)) + raw(alphabet)), len(indices)),
            (header_bits, 0, len(runs)),
            (header_bits, len(gamma(period) + raw(symbols[:period])), 0),
        ]

    ids = [f"categorical.{name}.v1" for name in ("raw", "uniform", "run_length", "periodic")]
    assert len(rows) == len(all_samples) * len(ids)
    for seq, symbols in enumerate(all_samples):
        bounds = rows[seq * 4:seq * 4 + 4]
        for code_id, row, lengths in zip(ids, bounds, reference(symbols)):
            assert row["code_id"] == code_id and row["status"] == "valid"
            assert row["sequence"] == row["work"] == str(seq)
            assert row["symbol_count"] == str(len(symbols))
            assert row["encoding"] == META["encoding"] and row["exact"] == "true"
            actual = tuple(int(row[key]) for key in ("header_bits", "model_bits", "data_bits"))
            assert actual == lengths and int(row["total_bits"]) == sum(lengths)

    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 2, 0, 1)) + chunk(0, 1, 0, 0))
    unsupported = list(csv.DictReader(io.StringIO(run(args.eval, "--categorical-baselines", trace))))
    assert [row["code_id"] for row in unsupported] == ids
    assert all(row["status"] == "unsupported" and not row["total_bits"] for row in unsupported)
    trace.write_bytes(data[:-1])
    run(args.eval, "--categorical-baselines", trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(actual_csv.encode()).hexdigest(),
        "independent_rows": len(rows),
        "witness_total_bits": [sum(parts) for parts in reference(sequences[2])],
    }


def check_categorical_iid(args, directory):
    samples = [
        [], [2**32 - 1], [9, 3, 3], [1, 2**32 - 1, 2**32 - 1],
        [9] * 40 + [3] * 40, [1, 2, 3] * 15, [7] * 4097, [7] * 4095 + [8],
    ]
    trace = directory / "categorical-iid.cltr"
    chunks = [
        chunk(1, i, i, i, struct.pack(f"<{len(symbols)}I", *symbols))
        for i, symbols in enumerate(samples)
    ]
    data = (header(META | {"fixed_sites": False}) + b"".join(chunks)
            + chunk(0, len(chunks), len(chunks) - 1, len(chunks) - 1))
    trace.write_bytes(data)
    csv_text = run(args.eval, "--categorical-iid", trace)
    assert hashlib.sha256(csv_text.encode()).hexdigest() == (
        "fc4255174f0e0030ac0c82f6da86bfe8ee51f61b3e96a2113668a90c122bb874"
    )
    rows = list(csv.DictReader(io.StringIO(csv_text)))
    assert len(rows) == len(samples)

    def gamma_bits(integer):
        return 2 * integer.bit_length() - 1

    widths = []
    for i, (row, symbols) in enumerate(zip(rows, samples)):
        assert row["sequence"] == row["work"] == str(i)
        assert row["symbol_count"] == str(len(symbols))
        assert row["code_id"] == "categorical.iid_enumerative.v1"
        fields = ("header_bits", "model_bits", "data_bits", "total_bits")
        if len(symbols) > 4096:
            assert row["status"] == "budget_exceeded"
            assert not any(row[field] for field in fields)
            continue
        assert row["status"] == "valid"
        histogram = Counter(symbols)
        model_bits = (gamma_bits(len(histogram)) +
                      sum(32 + gamma_bits(n) for n in histogram.values())) if histogram else 0
        permutations = factorial(len(symbols)) // prod(factorial(n) for n in histogram.values())
        rank_bits = (permutations - 1).bit_length()
        header_bits = gamma_bits(len(symbols) + 1)
        expected = (header_bits, model_bits, rank_bits, header_bits + model_bits + rank_bits)
        assert tuple(int(row[field]) for field in fields) == expected
        widths.append(rank_bits)

    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    unsupported, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-iid", trace)))
    assert unsupported["status"] == "unsupported" and not unsupported["total_bits"]
    trace.write_bytes(data[:-1])
    run(args.eval, "--categorical-iid", trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(csv_text.encode()).hexdigest(),
        "valid_rank_widths": widths,
        "budget_exceeded_rows": 1,
    }


def check_baseline_comparison(args, directory):
    sparse = {2, 7, 15, 29, 32, 58, 70, 79, 83, 99, 104, 119}
    samples = [
        [], list(range(12)), [int(bit) for bit in "01101000110110101100100111010010"],
        [1] * 80 + [2] * 60, [3, 9, 3, 9, 3], [9 if i in sparse else 3 for i in range(128)],
        [7] * 4097, [1, 2, 1],
    ]
    expected = [
        [4, 4, 4, 4, 4], [394, 449, 406, 401, 442], [1038, 113, 725, 1015, 129],
        [4498, 225, 106, 4513, 244], [168, 80, 173, 75, 85], [4114, 213, 905, 4063, 160],
    ]
    expected_winners = [0, 0, 1, 2, 3, 4]
    code_ids = [f"categorical.{name}.v1" for name in
                ("raw", "uniform", "run_length", "periodic", "iid_enumerative")]
    chunks = [chunk(1, i, i, i, struct.pack(f"<{len(sample)}I", *sample))
              for i, sample in enumerate(samples)]
    data = (header(META | {"fixed_sites": False}) + b"".join(chunks)
            + chunk(0, len(chunks), len(chunks) - 1, len(chunks) - 1))
    trace = directory / "baseline-comparison.cltr"
    trace.write_bytes(data)
    report = run(args.eval, "--categorical-compare", trace)
    rows = list(csv.DictReader(io.StringIO(report)))
    assert len(rows) == 5 * len(samples)

    # Construct all expected lengths independently from the specified bit layouts.
    def gamma(n):
        return 2 * n.bit_length() - 1

    for i, sample in enumerate(samples):
        n = len(sample)
        costs = rows[i * 5:i * 5 + 5]
        assert [row["code_id"] for row in costs] == code_ids
        assert all(row["sequence"] == row["work"] == str(i) for row in costs)
        assert all(row["symbol_count"] == str(n) and row["family_bits"] == "3" for row in costs)
        selected = [tag for tag, row in enumerate(costs) if row["selected"] == "true"]
        header_bits = 3 + gamma(n + 1)
        model = [0] * 5
        payload = [0] * 5
        if n:
            counts = Counter(sample)
            k = len(counts)
            period = next(step for step in range(1, n + 1)
                          if all(label == sample[j % step] for j, label in enumerate(sample)))
            model[1] = gamma(k) + 32 * k
            model[3] = gamma(period) + 32 * period
            model[4] = gamma(k) + sum(32 + gamma(count) for count in counts.values())
            payload[0] = 32 * n
            payload[1] = n * (k - 1).bit_length()
            payload[2] = sum(32 + gamma(len(list(group))) for _, group in groupby(sample))
            arrangements = factorial(n) // prod(factorial(count) for count in counts.values())
            payload[4] = (arrangements - 1).bit_length()
        fields = ("header_bits", "model_bits", "data_bits", "total_bits")
        for tag, row in enumerate(costs):
            if n > 4096 and tag == 4:
                assert row["status"] == "budget_exceeded" and not any(row[field] for field in fields)
                continue
            parts = (header_bits, model[tag], payload[tag])
            assert row["status"] == "valid"
            assert tuple(int(row[field]) for field in fields) == (*parts, sum(parts))
        if n > 4096:
            assert selected == [] and {row["comparison_status"] for row in costs} == {"budget_exceeded"}
        else:
            totals = [int(row["total_bits"]) for row in costs]
            winner = totals.index(min(totals))
            assert selected == [winner] and {row["comparison_status"] for row in costs} == {"valid"}
            if i < len(expected):
                assert totals == expected[i] and winner == expected_winners[i]

    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    unsupported = list(csv.DictReader(io.StringIO(run(args.eval, "--categorical-compare", trace))))
    assert [row["code_id"] for row in unsupported] == code_ids
    assert all(row["comparison_status"] == row["status"] == "unsupported"
               and not row["total_bits"] and row["selected"] == "false" for row in unsupported)
    trace.write_bytes(data[:-1])
    run(args.eval, "--categorical-compare", trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(report.encode()).hexdigest(),
        "candidate_rows": len(rows),
        "wins_by_fixture": expected_winners,
        "over_budget_comparisons": 1,
    }


def independent_frozen_huffman(fit, observed):
    counts = Counter(fit)
    serial = len(counts) + 1
    queue = [(count, i, (label,)) for i, (label, count) in enumerate(counts.items())]
    queue.append((1, len(counts), (None,)))
    heapq.heapify(queue)
    depths = dict.fromkeys([*counts, None], 0)
    while len(queue) > 1:
        left_weight, _, left = heapq.heappop(queue)
        right_weight, _, right = heapq.heappop(queue)
        for label in left + right:
            depths[label] += 1
        heapq.heappush(queue, (left_weight + right_weight, serial, left + right))
        serial += 1

    def gamma(n):
        return 2 * n.bit_length() - 1

    unseen = sum(label not in counts for label in observed)
    model = gamma(len(counts)) + sum(32 + gamma(n) for n in counts.values())
    header_bits = gamma(len(observed) + 1)
    payload = sum(depths[label] if label in counts else 32 + depths[None] for label in observed)
    raw = header_bits + 32 * len(observed)
    gain = raw - (header_bits + payload)
    return model, header_bits, payload, header_bits + payload, raw, unseen, gain, gain - model

def check_categorical_heldout(args, directory):
    train, guard, test = 4, 2, 4
    base = [9, 3, 9, 9]
    mixed = [9, 3, 100, 9]
    samples = [
        base + [1, 2] + mixed[:-1],
        base + [1, 2] + mixed,
        base + [100, 100] + mixed + [0, 0, 0],
        base + [1, 2] + [100] * 4,
        base + [1, 2] + [9] * 4,
        [90, 2, 8, 8, 81, 91, 2, 90, 2, 100],
        [1, 80, 9, 9, 81, 91, 80, 1, 80, 101],
    ]
    trace = directory / "categorical-heldout.cltr"

    def write_samples(observations):
        chunks = [chunk(1, i, i, i, struct.pack(f"<{len(sample)}I", *sample))
                  for i, sample in enumerate(observations)]
        n = len(chunks)
        data = (header(META | {"fixed_sites": False}) + b"".join(chunks)
                + chunk(0, n, n-1, n-1))
        trace.write_bytes(data)
        return data

    data = write_samples(samples)
    output = run(args.eval, "--categorical-heldout", train, guard, test, trace)
    rows = list(csv.DictReader(io.StringIO(output)))
    assert len(rows) == len(samples)
    fields = ("model_bits", "test_header_bits", "test_data_bits", "test_bits", "raw_bits",
              "unseen_symbols", "conditional_gain_bits", "charged_gain_bits")

    checks = []
    for i, (sample, row) in enumerate(zip(samples, rows)):
        assert row["sequence"] == row["work"] == str(i)
        assert row["symbol_count"] == str(len(sample))
        assert row["code_id"] == "categorical.frozen_huffman.v1"
        assert [row[key] for key in ("train_symbols", "guard_symbols", "test_symbols")] == [
            str(train), str(guard), str(test),
        ]
        if len(sample) < train + guard + test:
            assert row["status"] == "insufficient_data" and not any(row[key] for key in fields)
            continue
        assert row["status"] == "valid"
        actual = tuple(int(row[key]) for key in fields)
        assert actual == independent_frozen_huffman(sample[:train], sample[train+guard:train+guard+test])
        checks.append(actual)

    assert checks[0] == checks[1] == (71, 5, 38, 43, 133, 1, 90, 19)
    assert checks[2] == (71, 5, 136, 141, 133, 4, -8, -79)
    assert checks[3] == (71, 5, 4, 9, 133, 0, 124, 53)
    assert checks[4] == checks[5]

    write_samples([[3] * 4098])
    capped, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-heldout", 4097, 0, 1, trace)))
    assert capped["status"] == "budget_exceeded" and not any(capped[key] for key in fields)
    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    unsupported, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-heldout", train, guard, test, trace)))
    assert unsupported["status"] == "unsupported" and not unsupported["symbol_count"]
    assert not any(unsupported[key] for key in fields)
    for split in ((0, 2, 4), (4, -1, 4), (4, 2, 0), ("bad", 2, 4), (2**64 - 1, 1, 1)):
        assert not run(args.eval, "--categorical-heldout", *split, trace, code=2)
    trace.write_bytes(data[:-1])
    run(args.eval, "--categorical-heldout", train, guard, test, trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "valid_rows": len(checks),
        "short_rows": 1,
        "mixed_gains": checks[0][-2:],
        "shifted_gains": checks[2][-2:],
    }


def check_categorical_motif_heldout(args, directory):
    pattern = [1, 2, 3, 4, 5, 6]
    training = [label for i in range(100, 124) for label in pattern + [i]]
    guard = [800, 801, 802]
    matched = pattern * 24
    reordered = pattern[::-1] * 24
    assert Counter(matched) == Counter(reordered)
    split = len(training), len(guard), len(matched)
    base = training + guard + matched
    samples = [
        base[:-1], base, training + [900, 901, 902] + matched + [8000],
        training + guard + reordered, [0xFFFFFFFF - symbol for symbol in base],
        ([1, 2] * 84) + guard + matched, list(range(168)) + guard + matched,
    ]
    trace = directory / "categorical-motif-heldout.cltr"

    def write_samples(observations):
        chunks = [chunk(1, i, i, i, struct.pack(f"<{len(sample)}I", *sample))
                  for i, sample in enumerate(observations)]
        data = (header(META | {"fixed_sites": False}) + b"".join(chunks)
                + chunk(0, len(chunks), len(chunks) - 1, len(chunks) - 1))
        trace.write_bytes(data)
        return data

    mode = "--categorical-motif-heldout"
    data = write_samples(samples)
    output = run(args.eval, mode, *split, trace)
    rows = list(csv.DictReader(io.StringIO(output)))
    assert len(rows) == len(samples)
    assert [row["status"] for row in rows] == [
        "insufficient_data", "valid", "valid", "valid", "valid", "no_training_motif", "no_training_motif",
    ]
    motif_fields = ("model_bits", "test_header_bits", "placement_bits", "residual_bits", "test_bits", "occurrences")
    baseline_fields = ("baseline_model_bits", "baseline_test_bits")
    gain_fields = ("conditional_gain_bits", "charged_gain_bits")
    fields = (*motif_fields, *baseline_fields, *gain_fields)
    for i, (sample, row) in enumerate(zip(samples, rows)):
        assert None not in row and None not in row.values()
        assert row["sequence"] == row["work"] == str(i) and row["symbol_count"] == str(len(sample))
        assert row["code_id"] == "categorical.frozen_motif.v1"
        assert row["baseline_id"] == "categorical.frozen_huffman.v1"
        assert tuple(int(row[key]) for key in ("train_symbols", "guard_symbols", "test_symbols")) == split
        if row["status"] == "insufficient_data":
            assert not any(row[key] for key in fields)
            continue
        baseline = independent_frozen_huffman(sample[:split[0]], sample[split[0]+split[1]:][:split[2]])
        assert tuple(int(row[key]) for key in baseline_fields) == (baseline[0], baseline[3])
        if row["status"] == "no_training_motif":
            assert not any(row[key] for key in (*motif_fields, *gain_fields))
            continue
        # gamma(6) plus six u32s pays for the motif once. gamma(145) is the test header.
        # The matched test starts at offset zero with 24 adjacent matches. All gaps cost a bit.
        # The reversed period has no match and pays gamma(1) and 144 raw words.
        bits = (197, 15, 1, 4608, 4624, 0) if i == 3 else (197, 15, 33, 0, 48, 24)
        conditional = baseline[3] - bits[4]
        charged = conditional + baseline[0] - bits[0]
        assert tuple(int(row[key]) for key in fields) == (*bits, baseline[0], baseline[3], conditional, charged)

    assert all(rows[1][key] == rows[2][key] == rows[4][key] for key in fields)
    assert all(rows[1][key] == rows[3][key] for key in ("model_bits", *baseline_fields))
    assert all(int(rows[1][key]) > 0 > int(rows[3][key]) for key in gain_fields)
    write_samples([[3] * 4098])
    for capped_split in ((4097, 0, 1), (1, 0, 4097)):
        capped, = csv.DictReader(io.StringIO(run(args.eval, mode, *capped_split, trace)))
        assert capped["status"] == "budget_exceeded" and not any(capped[key] for key in fields)
    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    unsupported, = csv.DictReader(io.StringIO(run(args.eval, mode, *split, trace)))
    assert unsupported["status"] == "unsupported" and not unsupported["symbol_count"]
    assert not any(unsupported[key] for key in fields)
    assert not run(args.eval, mode, split[0], -1, split[2], trace, code=2)
    trace.write_bytes(data[:-1])
    run(args.eval, mode, *split, trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "matched": {key: int(rows[1][key]) for key in fields},
        "reordered": {key: int(rows[3][key]) for key in fields},
    }


def write_temporal_steps(trace, snapshots, times=None, fixed=True):
    if times is None:
        times = list(range(len(snapshots)))
    chunks = []
    for i, (frame, time) in enumerate(zip(snapshots, times)):
        if i in (1, 3):
            seq = len(chunks)
            chunks.append(chunk(2, seq, time, seq, struct.pack("<3Q", 1, 0, 0)))
        seq = len(chunks)
        chunks.append(chunk(1, seq, time, seq, struct.pack(f"<{len(frame)}I", *frame)))
    data = (header(META | {"fixed_sites": fixed}) + b"".join(chunks)
            + chunk(0, len(chunks), times[-1], len(chunks) - 1))
    trace.write_bytes(data)
    return data

def check_categorical_temporal(args, directory):
    trace = directory / "categorical-temporal.cltr"
    frames = [[9, 3, 0], [9, 9, 0], [700, 701, 0], [9, 3, 0], [100, 9, 0], [800, 801, 0]]

    def evaluate(*sizes):
        row, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-temporal",
                                               *(sizes or (2, 1, 2, 2)), trace)))
        return row

    data = write_temporal_steps(trace, frames)
    output = run(args.eval, "--categorical-temporal", 2, 1, 2, 2, trace)
    row, = csv.DictReader(io.StringIO(output))
    assert row["status"] == "valid" and row["reason"] == ""
    assert row["code_id"] == "categorical.frozen_huffman.v1" and row["exact"] == "true"
    assert row["time_unit"] == "step"
    assert [row[name] for name in ("train_seen", "guard_seen", "test_seen")] == ["2", "1", "2"]
    assert [row[f"{anchor}_{field}"] for anchor in ("train_end", "test_start", "test_end")
            for field in ("sequence", "work", "time")] == ["2", "2", "1", "5", "5", "3", "6", "6", "4"]
    fields = ("model_bits", "test_header_bits", "test_data_bits", "test_bits", "raw_bits",
              "unseen_symbols", "conditional_gain_bits", "charged_gain_bits")
    # Frozen training [9,3,9,9] assigns 9=1, 3=00, escape=01.
    # Each two-symbol frame pays three header bits. The two test payloads cost 3 and 35 bits.
    expected = (71, 6, 38, 44, 134, 1, 90, 19)
    assert tuple(int(row[name]) for name in fields) == expected, row

    mutated = [frame[:2] + [900 + i] for i, frame in enumerate(frames)]
    mutated[2] = [100, 100, 902]
    mutated[-1] = [100, 100, 905]
    write_temporal_steps(trace, mutated)
    assert evaluate() == row
    mutated[3] = [100, 100, 903]
    write_temporal_steps(trace, mutated)
    shifted = evaluate()
    # First test now costs 2 escapes at 34 bits each. Last test stays at 35 data bits.
    assert tuple(int(shifted[name]) for name in fields) == (71, 6, 103, 109, 134, 3, 25, -46)

    def missing(expected_status, reason, *sizes):
        actual = evaluate(*sizes)
        assert (actual["status"], actual["reason"]) == (expected_status, reason), actual
        assert not any(actual[name] for name in fields)
        return actual

    write_temporal_steps(trace, frames[:4])
    short = missing("insufficient_data", "too_few_snapshots")
    assert short["test_seen"] == "1"
    write_temporal_steps(trace, frames, [0, 1, 1, 3, 4, 5])
    missing("insufficient_data", "nonincreasing_time")
    write_temporal_steps(trace, frames)
    missing("insufficient_data", "too_few_sites", 2, 1, 2, 4)
    write_temporal_steps(trace, frames, fixed=False)
    missing("unsupported", "fixed_sites_required")
    write_temporal_steps(trace, [[0] * 4097] * 3)
    missing("budget_exceeded", "code_limit", 1, 1, 1, 4097)
    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    missing("unsupported", "no_categorical_snapshots")
    write_temporal_steps(trace, frames[:-1] + [[0]])
    assert not run(args.eval, "--categorical-temporal", 2, 1, 2, 2, trace, code=1)
    trace.write_bytes(data[:-1])
    assert not run(args.eval, "--categorical-temporal", 2, 1, 2, 2, trace, code=1)
    assert not run(args.eval, "--categorical-temporal", 2, 0, 2, 2, trace, code=2)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "baseline_bits": list(expected),
        "shifted_charged_gain": int(shifted["charged_gain_bits"]),
        "test_boundaries": [int(row["test_start_sequence"]), int(row["test_end_sequence"])],
    }


def check_categorical_motif_temporal(args, directory):
    trace = directory / "categorical-motif-temporal.cltr"
    mode = "--categorical-motif-temporal"
    split = (2, 1, 2, 84)
    pattern = [1, 2, 3, 4, 5, 6]
    training = [[label for i in range(start, start + 12) for label in pattern + [i]]
                for start in (100, 112)]
    matched = pattern * 14
    reversed_test = pattern[::-1] * 14
    assert Counter(matched) == Counter(reversed_test)
    frames = [*training, [7] * 84, matched, matched, [8] * 84]
    frames = [frame + [1000 + i] for i, frame in enumerate(frames)]
    data = write_temporal_steps(trace, frames)
    output = run(args.eval, mode, *split, trace)
    row, = csv.DictReader(io.StringIO(output))
    motif_fields = ("model_bits", "test_header_bits", "placement_bits", "residual_bits", "test_bits", "occurrences")
    baseline_fields = ("baseline_model_bits", "baseline_test_bits")
    gain_fields = ("conditional_gain_bits", "charged_gain_bits")
    fields = (*motif_fields, *baseline_fields, *gain_fields)
    assert row["status"] == "valid" and row["reason"] == ""
    assert row["code_id"] == "categorical.frozen_motif.v1" and row["exact"] == "true"
    assert row["baseline_id"] == "categorical.frozen_huffman.v1"
    assert tuple(int(row[key]) for key in ("train_seen", "guard_seen", "test_seen")) == split[:3]
    assert [row[f"{anchor}_{field}"] for anchor in ("train_end", "test_start", "test_end")
            for field in ("sequence", "work", "time")] == ["2", "2", "1", "5", "5", "3", "6", "6", "4"]

    def costs(actual, tests, expected_motif):
        assert None not in actual and None not in actual.values()
        fit = training[0] + training[1]
        references = [independent_frozen_huffman(fit, test) for test in tests]
        baseline_model = references[0][0]
        baseline_test = sum(ref[3] for ref in references)
        conditional = baseline_test - expected_motif[4]
        charged = conditional + baseline_model - expected_motif[0]
        assert tuple(int(actual[key]) for key in fields) == (
            *expected_motif, baseline_model, baseline_test, conditional, charged,
        )

    # Each 84-symbol frame pays gamma(85)=13 for length; 14 matches cost gamma(15)+14=21.
    costs(row, [matched, matched], (197, 26, 42, 0, 68, 28))

    def evaluate(sizes=split):
        actual, = csv.DictReader(io.StringIO(run(args.eval, mode, *sizes, trace)))
        return actual

    mutated = [frame[:84] + [2000 + i] for i, frame in enumerate(frames)]
    mutated[2] = [6] * 85
    mutated[-1] = [5] * 85
    write_temporal_steps(trace, mutated)
    assert evaluate() == row
    mutated[3] = reversed_test + [1003]
    write_temporal_steps(trace, mutated)
    shifted = evaluate()
    # One frame now pays gamma(1) for no matches plus 84 literal u32s.
    costs(shifted, [reversed_test, matched], (197, 26, 22, 2688, 2736, 14))
    assert all(int(row[key]) > 0 > int(shifted[key]) for key in gain_fields)

    write_temporal_steps(trace, [[9] * 85, [9] * 85, *frames[2:]])
    absent = evaluate()
    assert (absent["status"], absent["reason"]) == ("valid", "no_training_motif")
    assert all(absent[key] for key in baseline_fields)
    assert not any(absent[key] for key in (*motif_fields, *gain_fields))

    def missing(status, reason, sizes=split):
        actual = evaluate(sizes)
        assert None not in actual and None not in actual.values()
        assert (actual["status"], actual["reason"]) == (status, reason)
        assert not any(actual[key] for key in fields)

    write_temporal_steps(trace, frames[:4])
    missing("insufficient_data", "too_few_snapshots")
    write_temporal_steps(trace, frames, [0, 1, 1, 3, 4, 5])
    missing("insufficient_data", "nonincreasing_time")
    write_temporal_steps(trace, frames, fixed=False)
    missing("unsupported", "fixed_sites_required")
    write_temporal_steps(trace, [[0] * 2049] * 4)
    missing("budget_exceeded", "code_limit", (1, 1, 2, 2049))
    write_temporal_steps(trace, frames[:-1] + [[0]])
    assert not run(args.eval, mode, *split, trace, code=1)
    trace.write_bytes(data[:-1])
    assert not run(args.eval, mode, *split, trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "matched": {key: int(row[key]) for key in fields},
        "reordered_first_test": {key: int(shifted[key]) for key in fields},
    }


def splitmix64_words(seed):
    mask = (1 << 64) - 1
    state = seed
    while True:
        state = (state + 0x9E3779B97F4A7C15) & mask
        mixed = ((state ^ (state >> 30)) * 0xBF58476D1CE4E5B9) & mask
        mixed = ((mixed ^ (mixed >> 27)) * 0x94D049BB133111EB) & mask
        yield mixed ^ (mixed >> 31)


def shuffle_positions(labels, random_words):
    shuffled = list(labels)
    for remaining in range(len(shuffled), 1, -1):
        threshold = (1 << 64) % remaining
        value = next(word for word in random_words if word >= threshold)
        selected = value % remaining
        shuffled[remaining - 1], shuffled[selected] = shuffled[selected], shuffled[remaining - 1]
    return shuffled


def check_categorical_motif_permutation(args, directory):
    trace = directory / "categorical-motif-permutation.cltr"
    train, guard, test, sites = split = (10, 1, 2, 6)
    count, seed = 19, 0xFEE10A0C12345678
    mode = "--categorical-motif-permutation"
    training = [[1, 2, *range(start, start + 4)] for start in range(100, 140, 4)]
    tests = [[1, 2, 200, 201, 202, 203], [1, 2, 204, 205, 206, 207]]
    windows = [*training, [8] * sites, *tests, [9] * sites]
    frames = [frame + [1000 + i] for i, frame in enumerate(windows)]
    data = write_temporal_steps(trace, frames)
    output = run(args.eval, mode, *split, count, seed, trace)
    row, = csv.DictReader(io.StringIO(output))
    observed, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-motif-temporal", *split, trace)))
    assert all(row[key] == value for key, value in observed.items())
    assert row["permutation_id"] == "categorical.frame_permutation.v1" and int(row["seed"]) == seed
    assert row["status"] == "valid" and int(row["statistic_bits"]) == int(observed["charged_gain_bits"]) > 0

    random_words = splitmix64_words(seed)

    def permute(frame):
        labels = shuffle_positions(frame[:sites], random_words)
        assert Counter(labels) == Counter(frame[:sites])
        return labels + frame[sites:]

    scores = []
    scored_frames = {*range(train), *range(train + guard, train + guard + test)}
    for _ in range(count):
        surrogate = [permute(frame) if i in scored_frames else frame for i, frame in enumerate(frames)]
        write_temporal_steps(trace, surrogate)
        replay, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-motif-temporal", *split, trace)))
        assert replay["status"] == "valid"
        raw = replay["charged_gain_bits"]
        scores.append(raw if raw else "absent")
    assert row["draw_gains_bits"].split() == scores
    assert 0 < scores.count("absent") < count
    extreme = sum(int(score) >= int(row["statistic_bits"]) for score in scores if score != "absent")
    assert tuple(int(row[key]) for key in (
        "draws_requested", "draws_complete", "draws_no_training_motif", "draws_budget_exceeded",
        "extreme_draws", "p_numerator", "p_denominator",
    )) == (count, count, scores.count("absent"), 0, extreme, extreme + 1, count + 1)

    changed = [frame[:sites] + [4000 + i] for i, frame in enumerate(frames)]
    changed[train] = [21] * (sites + 1)
    changed[-1] = [22] * (sites + 1)
    write_temporal_steps(trace, changed)
    assert run(args.eval, mode, *split, count, seed, trace) == output
    tied_frames = [[7] * (sites + 1)] * len(frames)
    write_temporal_steps(trace, tied_frames)
    tied, = csv.DictReader(io.StringIO(run(args.eval, mode, train, guard, test, 1, count, 0, trace)))
    assert tied["reason"] == "no_training_motif" and tied["seed"] == "0"
    assert tied["draw_gains_bits"].split() == ["absent"] * count and tied["statistic_bits"] == "0"
    assert int(tied["p_numerator"]) == int(tied["p_denominator"]) == count + 1
    negative, = csv.DictReader(io.StringIO(run(args.eval, mode, *split, count, 0, trace)))
    assert int(negative["charged_gain_bits"]) < 0 and negative["statistic_bits"] == "0"
    assert negative["draw_gains_bits"].split() == [negative["charged_gain_bits"]] * count
    assert negative["p_numerator"] == negative["p_denominator"] == str(count + 1)
    write_temporal_steps(trace, frames[:3])
    short, = csv.DictReader(io.StringIO(run(args.eval, mode, *split, count, seed, trace)))
    assert short["status"] == "insufficient_data" and short["draws_complete"] == "0"
    assert not any(short[key] for key in ("statistic_bits", "p_numerator", "p_denominator", "draw_gains_bits"))
    for invalid_count, invalid_seed in ((0, seed), (200, seed), (19, -1), (19, 2**64)):
        assert not run(args.eval, mode, *split, invalid_count, invalid_seed, trace, code=2)
    trace.write_bytes(data[:-1])
    assert not run(args.eval, mode, *split, count, seed, trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "statistic_bits": int(row["statistic_bits"]),
        "draw_gains_bits": scores,
        "absent_draws": scores.count("absent"),
        "rank": [extreme + 1, count + 1],
    }


def check_categorical_motif_site_permutation(args, directory):
    mode = "--categorical-motif-site-permutation"
    trace = directory / "categorical-motif-site-permutation.cltr"
    split = (6, 1, 3, 6)
    count, seed = 19, 42
    stationary = [[9, 17, 40, 1, 11, 83]] * 10
    data = write_temporal_steps(trace, stationary)
    output = run(args.eval, mode, *split, count, seed, trace)
    shared, = csv.DictReader(io.StringIO(output))
    independent, = csv.DictReader(io.StringIO(
        run(args.eval, "--categorical-motif-permutation", *split, count, seed, trace)))
    assert shared["permutation_id"] == "categorical.shared_site_permutation.v1"
    assert shared["status"] == "valid" and shared["draws_complete"] == str(count)
    assert int(shared["charged_gain_bits"]) > 0
    assert shared["draw_gains_bits"].split() == [shared["charged_gain_bits"]] * count
    assert shared["p_numerator"] == shared["p_denominator"] == str(count + 1)
    assert shared["statistic_bits"] == independent["statistic_bits"]
    assert independent["p_numerator"] == "1" and independent["p_denominator"] == str(count + 1)

    train, guard, test, sites = split
    frames = [[1, 2, 100+i*4, 101+i*4, 102+i*4, 103+i*4, 222] for i in range(11)]
    variable_data = write_temporal_steps(trace, frames)
    variable_output = run(args.eval, mode, *split, count, seed, trace)
    row, = csv.DictReader(io.StringIO(variable_output))
    observed, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-motif-temporal", *split, trace)))
    assert all(row[key] == value for key, value in observed.items())
    scored_indices = [*range(train), *range(train + guard, train + guard + test)]

    def histories(source):
        return Counter(tuple(source[i][site] for i in scored_indices) for site in range(sites))

    scores = []
    words = splitmix64_words(seed)
    for _ in range(count):
        order = shuffle_positions(range(sites), words)
        surrogate = [frame[:] for frame in frames]
        for i in scored_indices:
            surrogate[i][:sites] = [frames[i][p] for p in order]
            assert Counter(surrogate[i][:sites]) == Counter(frames[i][:sites])
        assert histories(surrogate) == histories(frames)
        write_temporal_steps(trace, surrogate)
        replay, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-motif-temporal", *split, trace)))
        assert replay["status"] == "valid"
        scores.append(replay["charged_gain_bits"] or "absent")
    assert 0 < scores.count("absent") < count
    assert row["draw_gains_bits"].split() == scores and row["draws_complete"] == str(count)
    observed_statistic = int(row["statistic_bits"])
    extreme = sum((0 if score == "absent" else max(0, int(score))) >= observed_statistic for score in scores)
    assert tuple(map(int, (row["extreme_draws"], row["p_numerator"], row["p_denominator"]))) == (
        extreme, extreme + 1, count + 1,
    )
    changed = [frame[:sites] + [333] for frame in frames]
    changed[train] = [123] * (sites + 1)
    changed[-1] = [145] * (sites + 1)
    write_temporal_steps(trace, changed)
    assert run(args.eval, mode, *split, count, seed, trace) == variable_output
    trace.write_bytes(variable_data[:-1])
    assert not run(args.eval, mode, *split, count, seed, trace, code=1)
    return {
        "stationary_trace_sha256": hashlib.sha256(data).hexdigest(),
        "stationary_csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "stationary_charged_gain_bits": int(shared["charged_gain_bits"]),
        "stationary_independent_rank": [1, count + 1],
        "stationary_shared_rank": [count + 1, count + 1],
        "variable_trace_sha256": hashlib.sha256(variable_data).hexdigest(),
        "variable_csv_sha256": hashlib.sha256(variable_output.encode()).hexdigest(),
        "variable_shared_rank": [extreme + 1, count + 1],
        "variable_draw_gains_bits": scores,
    }


def check_motif_calibration_runner(args, directory):
    from calibrate_motifs import roster, upper_bound_millionths

    script = Path(__file__).resolve().parents[1] / "scripts/calibrate_motifs.py"

    def invoke(*arguments, code=0):
        return run(sys.executable, script, *arguments, code=code)

    design = json.loads(invoke("plan"))
    n = design["worlds_per_stratum"]
    assert (n, design["draws_per_world"], design["minimum_rank"], design["tail_error_per_bound"]) == (
        350, 99, "1/100", "1/40",
    )
    null_ceiling = design["maximum_possible_false_positives"]
    plant_miss_ceiling = n - design["minimum_detected_plants"]
    assert (null_ceiling, plant_miss_ceiling) == (23, 55)
    # Exact independent checks at both sides of each acceptance boundary.
    for ceiling, denominator in ((null_ceiling, 10), (plant_miss_ceiling, 5)):
        weight = sum(comb(n, k) * (denominator - 1)**(n - k) for k in range(ceiling + 1))
        next_weight = comb(n, ceiling + 1) * (denominator - 1)**(n - ceiling - 1)
        assert weight * 40 <= denominator**n < (weight + next_weight) * 40
        grid = 10**6
        upper = upper_bound_millionths(n, ceiling)
        for point, below in ((upper - 1, False), (upper, True)):
            numerator = sum(comb(n, k) * point**k * (grid - point)**(n - k)
                            for k in range(ceiling + 1))
            assert (numerator * 40 <= grid**n) == below

    archive = directory / "calibration-smoke"
    smoke = json.loads(invoke("smoke", "--eval", args.eval, "--out", archive))
    assert smoke == json.loads(invoke("audit", archive))
    failed_overwrite = invoke("smoke", "--eval", args.eval, "--out", archive, code=1)
    assert not failed_overwrite and smoke == json.loads(invoke("audit", archive))
    rows = smoke["outcomes"]
    assert [row["rank"] for row in rows] == [[68, 100], [100, 100], [1, 100], [1, 100]]
    assert "bounds" not in smoke

    manifest = json.loads((archive / "manifest.json").read_text())
    assert manifest["evaluator_sha256"] == hashlib.sha256(Path(args.eval).read_bytes()).hexdigest()
    reserved = list(roster("run"))
    assert len(reserved) == 700 and len({slot["input_seed"] for slot in reserved + rows}) == 704
    assert len({slot["permutation_seed"] for slot in reserved + rows}) == 704
    trace_digests = []
    for row in rows:
        trace = (archive / row["id"] / "trace.cltr").read_bytes()
        trace_digests.append(hashlib.sha256(trace).hexdigest())
        size, = struct.unpack_from("<I", trace, 12)
        offset = 16 + size
        for frame in range(10):
            labels = struct.unpack_from("<12I", trace, offset + 44)
            assert sorted(labels) == [1, 2, 3, 4, *range(100 + frame * 8, 108 + frame * 8)]
            if row["stratum"] == "planted_order" and frame != 6:
                start = labels.index(1)
                assert labels[start:start+4] == (1, 2, 3, 4)
            offset += 44 + 12 * 4

    (archive / rows[0]["id"]).rename(directory / "held-world")
    missing = archive / rows[1]["id"] / "receipt.json"
    missing.rename(missing.with_suffix(".saved"))
    incorrect = archive / rows[2]["id"] / "receipt.json"
    receipt = json.loads(incorrect.read_text())
    field_rows = list(csv.DictReader(io.StringIO(receipt["stdout"])))
    field_rows[0]["p_numerator"] = "100"
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=list(field_rows[0]))
    writer.writeheader()
    writer.writerows(field_rows)
    receipt["stdout"] = buffer.getvalue()
    incorrect.write_text(json.dumps(receipt))
    changed = json.loads(invoke("audit", archive))["outcomes"]
    assert [row["status"] for row in changed] == ["unstarted", "unresolved", "unresolved", "rejected"]
    assert [row["reason"] for row in changed[1:3]] == [
        "missing_receipt", "rank does not reconcile with recorded costs",
    ]

    empty_panel = directory / "calibration-empty"
    empty_panel.mkdir()
    (empty_panel / "clab-eval").hardlink_to(archive / "clab-eval")
    (empty_panel / "manifest.json").write_text(json.dumps(manifest | {"mode": "run", "slots": reserved}))
    empty = json.loads(invoke("audit", empty_panel))
    assert all(stratum["unstarted"] == n for stratum in empty["strata"].values())
    assert empty["bounds"] == {
        "possible_false_positives": n, "detected_plants": 0,
        "false_positive_upper_millionths": 10**6, "sensitivity_lower_millionths": 0,
        "false_positive_pass": False, "sensitivity_pass": False,
    }
    return {
        "protocol_sha256": design["protocol_sha256"],
        "smoke_trace_sha256": trace_digests,
        "smoke_ranks": [row["rank"] for row in rows],
        "acceptance_counts": [null_ceiling, n - plant_miss_ceiling],
        "unstarted_full_panel": 2 * n,
    }


def check_site_calibration_runner(args, directory):
    from calibrate_motifs import SITE_PROTOCOL, roster

    script = Path(__file__).resolve().parents[1] / "scripts/calibrate_motifs.py"

    def invoke(*arguments, code=0):
        return run(sys.executable, script, *arguments, code=code)

    design = json.loads(invoke("--protocol", "shared-site", "plan"))
    frame_design = json.loads(invoke("plan"))
    assert design["protocol"] == "E01-categorical-motif-site-calibration-v1"
    assert design["protocol_sha256"] != frame_design["protocol_sha256"]
    decision_fields = ("draws_per_world", "minimum_rank", "rank_cutoff", "tail_error_per_bound",
                       "worlds_per_stratum", "maximum_possible_false_positives", "minimum_detected_plants")
    assert tuple(design[key] for key in decision_fields) == tuple(frame_design[key] for key in decision_fields)

    archive = directory / "site-calibration-smoke"
    smoke = json.loads(invoke("--protocol", "shared-site", "smoke", "--eval", args.eval, "--out", archive))
    assert smoke == json.loads(invoke("audit", archive))
    assert not invoke("--protocol", "independent-frame", "audit", archive, code=1)
    rows = smoke["outcomes"]
    assert [row["rank"] for row in rows] == [[2, 100], [58, 100], [2, 100], [2, 100]]
    assert [row["status"] for row in rows] == ["rejected", "retained", "rejected", "rejected"]
    assert "bounds" not in smoke

    trace_digests = []
    for row in rows:
        world = archive / row["id"]
        receipt = json.loads((world / "receipt.json").read_text())
        scored, = csv.DictReader(io.StringIO(receipt["stdout"]))
        assert scored["permutation_id"] == "categorical.shared_site_permutation.v1"
        assert scored["draws_complete"] == "99" and scored["draws_budget_exceeded"] == "0"
        trace = (world / "trace.cltr").read_bytes()
        trace_digests.append(hashlib.sha256(trace).hexdigest())
        size, = struct.unpack_from("<I", trace, 12)
        offset = 16 + size
        frames = []
        for frame in range(10):
            labels = struct.unpack_from("<12I", trace, offset + 44)
            assert sorted(labels) == [1, 2, 3, 4, *range(100 + frame * 8, 108 + frame * 8)]
            frames.append(labels)
            offset += 44 + 12 * 4
        sites = {label: frames[0].index(label) for label in (1, 2, 3, 4)}
        assert all(frame[index] == label for frame in frames for label, index in sites.items())
        histories = list(zip(*frames))
        assert sum(len(set(history)) == 1 for history in histories) == 4
        assert all(len(set(history)) in (1, 10) for history in histories)
        if row["stratum"] == "planted_order":
            assert tuple(sites.values()) == tuple(range(sites[1], sites[1] + 4))

    reserved = list(roster("run", SITE_PROTOCOL))
    frame_slots = [*roster("run"), *roster("smoke")]
    all_slots = reserved + rows + frame_slots
    assert len(reserved) == 700
    assert len({slot["input_seed"] for slot in all_slots}) == len(all_slots)
    assert len({slot["permutation_seed"] for slot in all_slots}) == len(all_slots)

    manifest = json.loads((archive / "manifest.json").read_text())
    empty_panel = directory / "site-calibration-empty"
    empty_panel.mkdir()
    (empty_panel / "clab-eval").hardlink_to(archive / "clab-eval")
    (empty_panel / "manifest.json").write_text(json.dumps(manifest | {"mode": "run", "slots": reserved}))
    empty = json.loads(invoke("audit", empty_panel))
    assert all(stratum["unstarted"] == 350 for stratum in empty["strata"].values())
    assert empty["bounds"]["possible_false_positives"] == 350 and empty["bounds"]["detected_plants"] == 0
    assert not empty["bounds"]["false_positive_pass"] and not empty["bounds"]["sensitivity_pass"]
    return {
        "protocol_sha256": design["protocol_sha256"],
        "smoke_trace_sha256": trace_digests,
        "smoke_ranks": [row["rank"] for row in rows],
        "unstarted_full_panel": len(reserved),
    }


def check_categorical_motif(args, directory):
    def gamma(value):
        binary = f"{value:b}"
        return "0" * (len(binary) - 1) + binary

    def reference(sequence):
        n = len(sequence)
        candidates = {sequence[i:i + width] for width in range(2, 7) for i in range(n - width + 1)}
        best = None
        for pattern in candidates:
            width = len(pattern)
            hits = [i for i in range(n - width + 1) if sequence[i:i + width] == pattern]
            starts = []
            for hit in hits:
                if not starts or hit >= starts[-1] + width:
                    starts.append(hit)
            if len(starts) < 2:
                continue
            ends = [0] + [start + width for start in starts]
            skipped = [sequence[end:start] for end, start in zip(ends, starts)] + [sequence[ends[-1]:]]
            parts = (
                "101" + gamma(n + 1),
                gamma(width) + "".join(f"{symbol:032b}" for symbol in pattern),
                gamma(len(starts)) + "".join(gamma(len(gap) + 1) for gap in skipped[:-1]),
                "".join(f"{symbol:032b}" for gap in skipped for symbol in gap),
            )
            costs = tuple(map(len, parts))
            order = (sum(costs), starts[0], width)
            if best is None or order < best[0]:
                best = (order, starts, costs)
        return best

    separated = tuple(symbol for i in range(100, 124) for symbol in (1, 2, 3, 4, 5, 6, i))
    relabeled = tuple(0xFFFFFFFF - symbol for symbol in separated)
    samples = [
        separated, relabeled, (7, 6, 5, 4, 3) * 16, (1, 2, 3, 4), (),
        (1, 2, 1, 2), (9, 9, 1, 9, 1, 9), (7,) * 4097, (7,) * 13,
    ]
    trace = directory / "categorical-motif.cltr"
    chunks = [chunk(1, i, i, i, struct.pack(f"<{len(sample)}I", *sample)) for i, sample in enumerate(samples)]
    data = (header(META | {"fixed_sites": False}) + b"".join(chunks)
            + chunk(0, len(chunks), len(chunks) - 1, len(chunks) - 1))
    trace.write_bytes(data)
    output = run(args.eval, "--categorical-motif", trace)
    rows = list(csv.DictReader(io.StringIO(output)))
    assert len(rows) == len(samples)
    baselines = {row["sequence"]: row for row in
                 csv.DictReader(io.StringIO(run(args.eval, "--categorical-compare", trace)))
                 if row["selected"] == "true"}
    bit_fields = ("header_bits", "model_bits", "placement_bits", "residual_bits", "total_bits", "gain_bits")
    motif_fields = ("motif_width", "occurrences", *bit_fields)
    for i, (sample, row) in enumerate(zip(samples, rows)):
        assert None not in row and None not in row.values()
        assert row["sequence"] == row["work"] == str(i) and row["symbol_count"] == str(len(sample))
        assert row["motif_id"] == "categorical.single_motif.v1" and row["exact"] == "true"
        if len(sample) > 4096:
            assert row["status"] == "budget_exceeded" and row["selected"] == "false"
            assert not any(row[key] for key in ("baseline_id", "baseline_bits", *motif_fields))
            continue
        baseline = baselines[str(i)]
        assert (row["baseline_id"], row["baseline_bits"]) == (baseline["code_id"], baseline["total_bits"])
        expected = reference(sample)
        if expected is None:
            assert row["status"] == "no_motif" and row["selected"] == "false"
            assert not any(row[key] for key in motif_fields)
            continue
        (total, _, width), starts, costs = expected
        gain = int(row["baseline_bits"]) - total
        assert row["status"] == "valid" and row["selected"] == str(gain > 0).lower()
        assert tuple(int(row[key]) for key in motif_fields) == (width, len(starts), *costs, total, gain)

    assert [row["selected"] for row in rows] == ["true", "true", *(["false"] * 7)]
    assert tuple(int(rows[0][key]) for key in bit_fields[:-1]) == (18, 197, 79, 768, 1062)
    assert all(rows[0][key] == rows[1][key] for key in ("baseline_id", "baseline_bits", *motif_fields))
    assert rows[2]["baseline_id"] == "categorical.periodic.v1" and int(rows[2]["gain_bits"]) < 0
    trace.write_bytes(header() + chunk(2, 0, 0, 0, struct.pack("<3Q", 1, 0, 0)) + chunk(0, 1, 0, 0))
    unsupported, = csv.DictReader(io.StringIO(run(args.eval, "--categorical-motif", trace)))
    assert unsupported["status"] == "unsupported" and unsupported["selected"] == "false"
    assert not any(unsupported[key] for key in ("sequence", "symbol_count", "baseline_id", *motif_fields))
    trace.write_bytes(data[:-1])
    run(args.eval, "--categorical-motif", trace, code=1)
    return {
        "trace_sha256": hashlib.sha256(data).hexdigest(),
        "csv_sha256": hashlib.sha256(output.encode()).hexdigest(),
        "snapshot_rows": len(rows),
        "separated_motif": {key: int(rows[0][key]) for key in ("baseline_bits", *motif_fields)},
        "periodic_control": {key: int(rows[2][key]) for key in ("baseline_bits", "total_bits", "gain_bits")},
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
        "attributes": check_attributes(args, directory),
        "wire_compatibility": check_wire_compatibility(args, directory),
        "registry": check_registry(args, directory),
        "dependency_plan": check_dependency_plan(args),
        "histogram_cache": check_histogram_cache(args, directory),
        "categorical_codes": check_categorical_codes(args, directory),
        "categorical_iid": check_categorical_iid(args, directory),
        "baseline_comparison": check_baseline_comparison(args, directory),
        "categorical_heldout": check_categorical_heldout(args, directory),
        "categorical_temporal": check_categorical_temporal(args, directory),
        "categorical_motif": check_categorical_motif(args, directory),
        "categorical_motif_heldout": check_categorical_motif_heldout(args, directory),
        "categorical_motif_temporal": check_categorical_motif_temporal(args, directory),
        "categorical_motif_permutation": check_categorical_motif_permutation(args, directory),
        "categorical_motif_site_permutation": check_categorical_motif_site_permutation(args, directory),
        "motif_calibration_runner": check_motif_calibration_runner(args, directory),
        "site_calibration_runner": check_site_calibration_runner(args, directory),
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
