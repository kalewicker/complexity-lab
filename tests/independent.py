"""Produce CLTR bytes without using any C++ implementation code."""
import argparse
import csv
import hashlib
import io
import json
import os
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


def header(meta=META):
    data = json.dumps(meta, separators=(",", ":")).encode()
    return struct.pack("<4sHHII", b"CLTR", 1, 0, 0x01020304, len(data)) + data


def chunk(kind, sequence, time, work, payload=b"", flags=0, version=1):
    prefix = struct.pack("<HHIQdQII", kind, version, flags, sequence, time, work,
                         len(payload), len(payload))
    return prefix + struct.pack("<I", crc32c(prefix + payload)) + payload


def run(tool, *args, code=0):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True)
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
    }
    assert result["categorical_sha256"] == "3dda44df0c60342ddd2b6456bbb350ea66a50141d86dc62ba4e887a0000fc9ba"
    assert result["events_sha256"] == "5f5e31d67be90e5eea677c262b01358df9cc2427ff86a17d975c44f4d34ddac1"
    assert result["relational"]["sha256"] == "b9d66dd6527a31254bd27f97840a345fde40c5b100507d0def7720ecd410412d"
    assert result["temporal"]["trace_sha256"] == "602af49f1f3105f4a1db42c61c69e67fdb9235f48a194863f8500bb0725f4e60"
    if args.fixture:
        output = directory / "mock"
        run(args.fixture, output)
        with (output / "planned.csv").open() as file:
            planned, = csv.DictReader(file)
        with (output / "status.csv").open() as file:
            status, = csv.DictReader(file)
        assert planned["run_id"] == status["run_id"]
        assert (status["status"], status["reason"], status["completed_work"]) == ("capped", "work", "5")
        assert planned["hard_work_cap"] == "5" and planned["target_work"] == "8"
        mock_trace = output / status["trace"]
        rows = list(csv.DictReader(io.StringIO(run(args.eval, mock_trace))))
        assert {row["run_id"] for row in rows} == {planned["run_id"]}
        assert max(int(row["work"]) for row in rows if row["work"]) == 5
        assert metric(rows, "work.logical_updates.v1", 11)["value"] == "5"
        mock_sha = hashlib.sha256(mock_trace.read_bytes()).hexdigest()
        assert mock_sha == "890dcb152bc02ec22bb4754fefbd69a6bea966239fbdc87df7d3c72110780ff1"
        result["mock"] = status | {"trace_sha256": mock_sha}
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
