"""Audit training nominees and conditional support in the registered v1 archive."""

import argparse
from collections import Counter
from fractions import Fraction
from hashlib import sha256
from itertools import permutations
import json
from math import comb
from pathlib import Path
import struct

from cltr_fixture import META, chunk, header
from repetition_growth import PROTOCOL_PATH, supported_definition


def read_frames(directory, run_id, expected_digest, shape):
    frames, slots, width = shape
    data = (directory / f"{run_id}.cltr").read_bytes()
    if sha256(data).hexdigest() != expected_digest:
        raise ValueError(f"{run_id}: trace digest differs from receipt")
    prefix = header(META | {"run_id": run_id})
    if not data.startswith(prefix):
        raise ValueError(f"{run_id}: unexpected trace header")
    record = struct.Struct(f"<{slots * (width + 1)}I")
    offset = len(prefix)
    for frame in range(frames):
        payload = data[offset + 44:offset + 44 + record.size]
        expected = chunk(1, frame, frame, frame, payload)
        if data[offset:offset + len(expected)] != expected or len(payload) != record.size:
            raise ValueError(f"{run_id}: unexpected frame {frame}")
        labels = record.unpack(payload)
        yield tuple(labels[i:i + width + 1] for i in range(0, len(labels), width + 1))
        offset += len(expected)
    if data[offset:] != chunk(0, frames, frames - 1, frames - 1):
        raise ValueError(f"{run_id}: unexpected trace footer")


def starts(block, pattern):
    return tuple(i for i in range(len(block) - len(pattern) + 1)
                 if block[i:i + len(pattern)] == pattern)


def gamma_bits(value):
    return 2 * value.bit_length() - 1


def placement_bits(offsets, width):
    cursor = 0
    bits = gamma_bits(len(offsets) + 1)
    for offset in offsets:
        bits += gamma_bits(offset - cursor + 1)
        cursor = offset + width
    return bits


def second_placements(slots, pattern, varied, stride):
    return tuple(slot * stride + start for slot in varied for start in starts(slots[slot][:-1], pattern))


def training_nominee(frames, varied, stride):
    """Enumerate the scorer's second-entry candidates on this slot fixture."""
    # Fresh separators cannot be part of any candidate with two placements.
    first = {}
    for i, frame in enumerate(frames):
        for slot in varied:
            block = frame[slot][:-1]
            for start in range(len(block) - 1):
                for stop in range(start + 2, len(block) + 1):
                    first.setdefault(block[start:stop], (i, slot * stride + start))
    orders = []
    for pattern, first_seen in first.items():
        width = len(pattern)
        hits = [second_placements(frame, pattern, varied, stride) for frame in frames]
        count = sum(map(len, hits))
        if count < 2:
            continue
        savings = 32 * width * count - sum(placement_bits(offsets, width) for offsets in hits)
        orders.append((gamma_bits(width) + 32 * width - savings, first_seen, width, pattern))
    return min(orders)[-1] if orders else None


def exact_tail(pattern, appearance_count, trials, alphabet, excluded):
    legal = [block for block in permutations(alphabet) if block not in excluded]
    probability = Fraction(sum(bool(starts(block, pattern)) for block in legal), len(legal))
    return sum((comb(trials, i) * probability**i * (1 - probability)**(trials - i)
                for i in range(appearance_count, trials + 1)), Fraction())


def audit(archive):
    parent = json.loads(PROTOCOL_PATH.read_bytes())
    manifest_data = (archive / "manifest.json").read_bytes()
    report_data = (archive / "report.json").read_bytes()
    manifest = json.loads(manifest_data)
    report = json.loads(report_data)
    if (manifest["mode"] != "run" or manifest["protocol_sha256"] != sha256(PROTOCOL_PATH.read_bytes()).hexdigest()
            or manifest["evaluator_sha256"] != sha256((archive / "clab-eval").read_bytes()).hexdigest()
            or any(report[key] != value for key, value in manifest.items())):
        raise ValueError("archive does not match the registered v1 run")
    population = parent["population"]
    alphabet = population["labels"]
    width, slots = len(alphabet), population["slots"]
    train, guard, test = (parent["split"][key] for key in ("train", "guard", "test"))
    shape = (train + guard + test, slots, width)
    roster = [f"pair-{i:03}" for i in range(population["pairs"])]
    if any([entry["pair_id"] for entry in entries] != roster
           for entries in (manifest["slots"], report["outcomes"])):
        raise ValueError("archive roster differs from registration")
    rows = []
    for slot, row in zip(manifest["slots"], report["outcomes"]):
        name = row["pair_id"]
        seed = sha256(f"{parent['protocol']}\nrun\n{int(name[5:])}\n".encode()).hexdigest()
        if row["seed"] != seed or slot["seed"] != seed:
            raise ValueError(f"{name}: seed does not match registration")
        conditions = row["conditions"]
        if set(conditions) != set(population["conditions"]):
            raise ValueError(f"{name}: incomplete condition receipts")
        frames = {label: tuple(read_frames(archive / name, f"{name}-{label}", receipt["trace_sha256"], shape))
                  for label, receipt in conditions.items()}
        a = frames["repeat"][0][0][:-1]
        b = a[::-1]
        varied = tuple(i for i, slot in enumerate(frames["diversify"][0]) if slot[:-1] == b)
        if sorted(a) != alphabet or len(varied) != slots - population["early_motif_slots"]:
            raise ValueError(f"{name}: anchor or varied layout differs from registration")
        for label, series in frames.items():
            validate_layout(series, label, a, b, varied, alphabet, population["separator_start"])
            varied_slots = () if label == "repeat" else varied
            receipt = conditions[label]
            if supported_definition(receipt["scores"]) != (receipt["selected"], receipt["definition_bits"]):
                raise ValueError(f"{name}: archived decision differs from recorded costs")
            result = {"pair": name, "condition": label, "v1_selected": receipt["selected"]}
            if receipt["selected"] == "pair":
                result.update(check_nominee(series, receipt["scores"]["pair"], varied_slots, a, b, train, guard,
                                            test))
            rows.append(result)
    return {"archive_manifest_sha256": sha256(manifest_data).hexdigest(),
            "archive_report_sha256": sha256(report_data).hexdigest(), "rows": rows}


def validate_layout(series, condition, a, b, varied, alphabet, separator_start, *, exclude_reverse=True):
    excluded = (a, b) if exclude_reverse else (a,)
    for i, frame in enumerate(series):
        for j, slot in enumerate(frame):
            symbols = slot[:-1]
            if slot[-1] != separator_start + i * len(frame) + j:
                raise ValueError("unexpected separator")
            if condition == "early" and j in varied:
                if sorted(symbols) != alphabet or symbols in excluded:
                    raise ValueError("unexpected early nuisance order")
            elif symbols != (b if condition == "diversify" and j in varied else a):
                raise ValueError("unexpected motif placement")


def check_nominee(frames, score, varied, a, b, train, guard, test):
    stride = len(a) + 1
    pattern = training_nominee(frames[:train], varied, stride)
    if pattern is None:
        raise ValueError("archived pair has no training nominee")
    held_out = frames[train + guard:]
    hits = [second_placements(frame, pattern, varied, stride) for frame in held_out]
    observed = sum(map(len, hits))
    second_model = gamma_bits(len(pattern)) + 32 * len(pattern)
    first_model = gamma_bits(len(a)) + 32 * len(a)
    gain = 32 * len(pattern) * observed - sum(placement_bits(hit, len(pattern)) for hit in hits)
    expected = {"model_bits": first_model + second_model, "first_model_bits": first_model,
                "first_occurrences": test * (len(frames[0]) - len(varied)), "second_occurrences": observed,
                "conditional_gain_vs_first_bits": gain, "charged_gain_vs_first_bits": gain - second_model}
    if any(value != int(score[key]) for key, value in expected.items()):
        raise ValueError("independent nominee or placement costs differ from archived scorer")
    trials = test * len(varied)
    return {"nominee": pattern, "test_matches_by_frame": list(map(len, hits)), "test_slots": trials,
            "charged_gain_bits": gain - second_model,
            "v1_background_tail": str(exact_tail(pattern, observed, trials, a, {a, b})),
            "a_only_excluded_tail": str(exact_tail(pattern, observed, trials, a, {a}))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.archive), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
