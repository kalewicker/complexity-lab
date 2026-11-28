"""Run the registered matched repetition and motif diversity comparison."""

import argparse
import csv
from hashlib import sha256
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess

from calibrate_motifs import FrameRandom, write_json
from cltr_fixture import META, chunk, header


PROTOCOL_PATH = Path(__file__).resolve().parents[1] / "protocols/E01-categorical-repetition-growth.json"
SCORERS = {
    "single": "--categorical-motif-temporal",
    "pair": "--categorical-motif-dictionary-temporal",
}


def matched_traces(seed, pair_id, protocol):
    population = protocol["population"]
    slots = population["slots"]
    motif_a = population["labels"].copy()
    rng = FrameRandom(seed)
    rng.shuffle(motif_a)
    motif_b = motif_a[::-1]
    positions = list(range(slots))
    rng.shuffle(positions)
    a_positions = set(positions[:population["early_motif_slots"]])
    excluded = (motif_a, motif_b) if population.get("exclude_reverse_in_background", True) else (motif_a,)
    conditions = population["conditions"]
    traces = {condition: bytearray(header(META | {"run_id": f"{pair_id}-{condition}"}))
              for condition in conditions}
    train, guard, test, sites = (protocol["split"][key] for key in ("train", "guard", "test", "sites"))
    frames = train + guard + test
    for frame in range(frames):
        labels = {condition: [] for condition in conditions}
        for slot in range(slots):
            early = motif_a
            diverse = motif_a
            if slot not in a_positions:
                diverse = motif_b
                early = motif_a.copy()
                while early in excluded:
                    rng.shuffle(early)
            separator = population["separator_start"] + frame * slots + slot
            for condition, motif in zip(conditions, (early, motif_a, diverse)):
                labels[condition].extend([*motif, separator])
        for condition in conditions:
            payload = struct.pack(f"<{sites}I", *labels[condition])
            traces[condition].extend(chunk(1, frame, frame, frame, payload))
    for trace in traces.values():
        trace.extend(chunk(0, frames, frames - 1, frames - 1))
    return traces


def score_trace(evaluator, trace, protocol):
    split = [str(protocol["split"][key]) for key in ("train", "guard", "test", "sites")]
    scores = {}
    for name, flag in SCORERS.items():
        result = subprocess.run([evaluator, flag, *split, trace], capture_output=True, text=True,
                                timeout=protocol["execution"]["timeout_seconds_per_score"])
        trace.with_suffix(f".{name}.csv").write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            raise ValueError(f"{trace.stem} {name} exited {result.returncode}: {result.stderr.strip()}")
        row, = csv.DictReader(io.StringIO(result.stdout))
        if row["status"] != "valid" or row["run_id"] != trace.stem:
            raise ValueError(f"{trace.stem} {name}: {row['status']} {row['reason']}")
        scores[name] = row
    return scores


def costs(row, prefix=""):
    return int(row[f"{prefix}model_bits"]), int(row[f"{prefix}test_bits"])


def beats(candidate, comparator):
    return candidate[1] < comparator[1] and sum(candidate) < sum(comparator)


def supported_definition(scores):
    single, pair = scores["single"], scores["pair"]
    baseline = costs(single, "baseline_")
    if baseline != costs(pair, "baseline_"):
        raise ValueError("Huffman costs differ between temporal scorers")
    single_cost = costs(single) if single["model_bits"] else None
    if pair["model_bits"]:
        pair_cost = costs(pair)
        comparators = [baseline, costs(pair, "first_")]
        if single_cost is not None:
            comparators.append(single_cost)
        if all(beats(pair_cost, other) for other in comparators):
            return "pair", pair_cost[0]
    if single_cost is not None and beats(single_cost, baseline):
        return "single", single_cost[0]
    return "none", 0


def evaluate_pair(evaluator, directory, slot, protocol):
    pair_id = slot["pair_id"]
    directory.mkdir()
    conditions = {}
    try:
        for condition, data in matched_traces(slot["seed"], pair_id, protocol).items():
            trace = directory / f"{pair_id}-{condition}.cltr"
            trace.write_bytes(data)
            scores = score_trace(evaluator, trace, protocol)
            selected, bits = supported_definition(scores)
            conditions[condition] = {"selected": selected, "definition_bits": bits, "scores": scores,
                                     "trace_sha256": sha256(data).hexdigest()}
    except (OSError, subprocess.TimeoutExpired, KeyError, ValueError) as error:
        return slot | {"status": "unresolved", "reason": str(error), "conditions": conditions}
    early, repeat, diversify = (conditions[name]["definition_bits"]
                                for name in ("early", "repeat", "diversify"))
    repetition = repeat - early
    growth = diversify - early
    decision = protocol["decision"]
    numerator, denominator = decision["maximum_repetition_fraction_of_growth"]
    passed = (conditions["early"]["selected"] == decision["required_early_model"]
              and growth >= decision["minimum_growth_definition_bits"]
              and abs(repetition) * denominator <= growth * numerator)
    return slot | {"status": "pass" if passed else "fail", "repetition_change_bits": repetition,
                   "diversity_change_bits": growth, "conditions": conditions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("smoke", "run"))
    parser.add_argument("--eval", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    protocol_bytes = PROTOCOL_PATH.read_bytes()
    protocol = json.loads(protocol_bytes)
    size = protocol["population"]["smoke_pairs" if args.mode == "smoke" else "pairs"]
    slots = [
        {"pair_id": f"pair-{i:03}",
         "seed": sha256(f"{protocol['protocol']}\n{args.mode}\n{i}\n".encode()).hexdigest()}
        for i in range(size)
    ]
    evaluator = args.eval.resolve(strict=True)
    output = args.out.resolve()
    output.mkdir()
    evaluator = Path(shutil.copy2(evaluator, output / "clab-eval"))
    manifest = {"protocol": protocol["protocol"], "protocol_sha256": sha256(protocol_bytes).hexdigest(),
                "mode": args.mode, "slots": slots, "evaluator_sha256": sha256(evaluator.read_bytes()).hexdigest()}
    write_json(output / "manifest.json", manifest)
    outcomes = []
    for slot in slots:
        directory = output / slot["pair_id"]
        outcome = evaluate_pair(evaluator, directory, slot, protocol)
        write_json(directory / "outcome.json", outcome)
        outcomes.append(outcome)
    count = sum(outcome["status"] == "pass" for outcome in outcomes)
    report = manifest | {"outcomes": outcomes, "passing_pairs": count}
    if args.mode == "run":
        report["pass"] = count >= protocol["decision"]["minimum_passing_pairs"]
    write_json(output / "report.json", report)
    print(f"{count}/{size} pairs passed")


if __name__ == "__main__":
    main()
