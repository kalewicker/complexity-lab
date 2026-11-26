"""Produce CLTR bytes without using any C++ implementation code."""
import argparse
import csv
import hashlib
import io
import json
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
    }
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
        result["mock"] = status | {"trace_sha256": hashlib.sha256(mock_trace.read_bytes()).hexdigest()}
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
