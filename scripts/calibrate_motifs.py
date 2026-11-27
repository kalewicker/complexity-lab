"""Register, run, and audit the fixed synthetic temporal motif panel."""
import argparse
import csv
from fractions import Fraction
from hashlib import sha256
import io
import json
from math import comb
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
from cltr_fixture import META, chunk, header

PROTOCOL_PATH = Path(__file__).resolve().parents[1] / "protocols/E01-categorical-motif-calibration.json"
PROTOCOL_BYTES = PROTOCOL_PATH.read_bytes()
PROTOCOL = json.loads(PROTOCOL_BYTES)
DECISION = PROTOCOL["decision"]
STRATA = PROTOCOL["population"]["strata"]
NULL, PLANT = STRATA
SPLIT = tuple(PROTOCOL["split"][key] for key in ("train", "guard", "test", "sites"))
DRAW_COUNT = DECISION["draws"]
RANK_CUTOFF = Fraction(*DECISION["rank_cutoff"])
TAIL_ERROR = Fraction(*DECISION["family_error"]) / len(STRATA)
MODE = "--categorical-motif-permutation"


def binomial_cdf(n, k, rate):
    a, b = rate.numerator, rate.denominator
    return Fraction(sum(comb(n, i) * a**i * (b - a)**(n - i)
                        for i in range(k + 1)), b**n)


def rejection_ceiling(n, maximum_rate):
    low, high = -1, n
    while low + 1 < high:
        mid = (low + high) // 2
        if binomial_cdf(n, mid, maximum_rate) <= TAIL_ERROR:
            low = mid
        else:
            high = mid
    return low


def upper_bound_millionths(n, count):
    # Round the one-sided exact upper bound outward on a millionth grid.
    low, high = 0, 10**6
    while low + 1 < high:
        mid = (low + high) // 2
        if binomial_cdf(n, count, Fraction(mid, 10**6)) <= TAIL_ERROR:
            high = mid
        else:
            low = mid
    return high


def plan():
    n = PROTOCOL["population"]["worlds_per_stratum"]
    ceiling = rejection_ceiling(n, Fraction(*DECISION["maximum_false_positive_rate"]))
    floor = n - rejection_ceiling(n, 1 - Fraction(*DECISION["minimum_sensitivity"]))
    return {
        "protocol": PROTOCOL["protocol"],
        "protocol_sha256": sha256(PROTOCOL_BYTES).hexdigest(),
        "worlds_per_stratum": n,
        "draws_per_world": DRAW_COUNT,
        "minimum_rank": str(Fraction(1, DRAW_COUNT + 1)),
        "rank_cutoff": str(RANK_CUTOFF),
        "tail_error_per_bound": str(TAIL_ERROR),
        "maximum_possible_false_positives": ceiling,
        "minimum_detected_plants": floor,
        "planning_null_pass_probability": round(float(binomial_cdf(
            n, ceiling, Fraction(*DECISION["planning_false_positive_rate"]))), 8),
        "planning_plant_pass_probability": round(float(binomial_cdf(
            n, n - floor, 1 - Fraction(*DECISION["planning_sensitivity"]))), 8),
    }


def roster(mode):
    n = (PROTOCOL["population"]["worlds_per_stratum"] if mode == "run"
         else PROTOCOL["execution"]["smoke_worlds_per_stratum"])
    for stratum in STRATA:
        for i in range(n):
            slot = f"{stratum}-{i:03}"
            namespace = f'{PROTOCOL["protocol"]}\n{mode}\n{slot}\n'.encode()
            yield {
                "id": slot,
                "stratum": stratum,
                "input_seed": sha256(namespace + b"input").hexdigest(),
                "permutation_seed": int.from_bytes(sha256(namespace + b"scorer").digest()[:8], "big"),
            }


class FrameRandom:
    def __init__(self, seed):
        self.key = bytes.fromhex(seed)
        self.counter = 0

    def below(self, bound):
        limit = (1 << 64) // bound * bound
        while True:
            digest = sha256(self.key + self.counter.to_bytes(8, "big")).digest()
            self.counter += 1
            word = int.from_bytes(digest[:8], "big")
            if word < limit:
                return word % bound

    def shuffle(self, labels):
        for i in range(len(labels) - 1, 0, -1):
            j = self.below(i + 1)
            labels[i], labels[j] = labels[j], labels[i]


def world_trace(slot):
    train, guard, test, sites = SPLIT
    population = PROTOCOL["population"]
    motif = population["motif"]
    width = sites - len(motif)
    rng = FrameRandom(slot["input_seed"])
    data = bytearray(header(META | {"run_id": slot["id"]}))
    for i in range(train + guard + test):
        start = population["nuisance_label_start"] + width * i
        nuisance = list(range(start, start + width))
        if slot["stratum"] == PLANT and (i < train or i >= train + guard):
            rng.shuffle(nuisance)
            offset = rng.below(width + 1)
            labels = nuisance[:offset] + motif + nuisance[offset:]
        else:
            labels = motif + nuisance
            rng.shuffle(labels)
        data.extend(chunk(1, i, i, i, struct.pack(f"<{sites}I", *labels)))
    last = train + guard + test - 1
    data.extend(chunk(0, last + 1, last, last))
    return bytes(data)


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def launch(evaluator, directory, slot):
    directory.mkdir()
    trace = directory / "trace.cltr"
    data = world_trace(slot)
    trace.write_bytes(data)
    command = [str(evaluator), MODE, *map(str, (*SPLIT, DRAW_COUNT, slot["permutation_seed"])), str(trace)]
    receipt = {"trace_sha256": sha256(data).hexdigest()}
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=PROTOCOL["execution"]["timeout_seconds_per_world"])
        receipt.update(status="exited", returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
    except subprocess.TimeoutExpired as error:
        receipt.update(status="timeout", stdout=(error.stdout or b"").decode("utf-8", "replace"),
                       stderr=(error.stderr or b"").decode("utf-8", "replace"))
    except OSError as error:
        receipt.update(status="launch_failed", error=str(error))
    write_json(directory / "receipt.json", receipt)


def checked_rank(csv_text, slot):
    row, = csv.DictReader(io.StringIO(csv_text))
    if None in row or None in row.values() or row["run_id"] != slot["id"]:
        raise ValueError("row does not identify the planned world")
    split_fields = ("train_snapshots", "guard_snapshots", "test_snapshots", "sites")
    if (tuple(int(row[key]) for key in split_fields) != SPLIT
            or tuple(int(row[key]) for key in ("train_seen", "guard_seen", "test_seen")) != SPLIT[:3]
            or row["encoding"] != META["encoding"] or row["exact"] != "true"
            or row["permutation_id"] != PROTOCOL["permutation_id"]
            or int(row["seed"]) != slot["permutation_seed"]
            or row["status"] != "valid" or int(row["draws_requested"]) != DRAW_COUNT
            or int(row["draws_complete"]) != DRAW_COUNT or int(row["draws_budget_exceeded"]) != 0):
        raise ValueError("evaluator did not complete the planned permutation split")
    gain = row["charged_gain_bits"]
    if row["reason"] != ("" if gain else "no_training_motif"):
        raise ValueError("observed motif status is inconsistent")
    if gain:
        baseline = int(row["baseline_model_bits"]) + int(row["baseline_test_bits"])
        coded = int(row["model_bits"]) + int(row["test_bits"])
        if baseline - coded != int(gain):
            raise ValueError("observed gain does not reconcile with recorded costs")
    observed = max(0, int(gain)) if gain else 0
    draws = row["draw_gains_bits"].split()
    if len(draws) != DRAW_COUNT or draws.count("absent") != int(row["draws_no_training_motif"]):
        raise ValueError("draw accounting is inconsistent")
    extreme = sum((0 if token == "absent" else max(0, int(token))) >= observed for token in draws)
    numerator, denominator = int(row["p_numerator"]), int(row["p_denominator"])
    if (int(row["statistic_bits"]), int(row["extreme_draws"]), numerator, denominator) != (
            observed, extreme, extreme + 1, DRAW_COUNT + 1):
        raise ValueError("rank does not reconcile with recorded costs")
    return numerator, denominator


def outcome(output, slot):
    directory = output / slot["id"]
    if not directory.exists():
        return {"status": "unstarted"}
    receipt_path = directory / "receipt.json"
    if not receipt_path.is_file():
        return {"status": "unresolved", "reason": "missing_receipt"}
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        data = (directory / "trace.cltr").read_bytes()
        if sha256(data).hexdigest() != receipt["trace_sha256"] or data != world_trace(slot):
            return {"status": "unresolved", "reason": "trace_mismatch"}
        if receipt["status"] != "exited":
            return {"status": "unresolved", "reason": receipt["status"]}
        if receipt["returncode"] != 0:
            return {"status": "unresolved", "reason": "evaluator_exit"}
        rank = checked_rank(receipt["stdout"], slot)
    except (OSError, KeyError, TypeError, ValueError) as error:
        return {"status": "unresolved", "reason": str(error)}
    return {"status": "rejected" if Fraction(*rank) <= RANK_CUTOFF else "retained", "rank": rank}


def audit(output):
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    mode = manifest["mode"]
    design = plan()
    slots = list(roster(mode))
    if (mode not in ("run", "smoke") or manifest["design"] != design or manifest["slots"] != slots):
        raise ValueError("manifest differs from the registered roster")
    if sha256((output / "clab-eval").read_bytes()).hexdigest() != manifest["evaluator_sha256"]:
        raise ValueError("evaluator snapshot differs from its registered digest")

    outcomes = [slot | outcome(output, slot) for slot in slots]
    strata = {}
    for stratum in STRATA:
        statuses = [entry["status"] for entry in outcomes if entry["stratum"] == stratum]
        strata[stratum] = {status: statuses.count(status)
                           for status in ("rejected", "retained", "unresolved", "unstarted")}

    report = {"mode": mode, "design": design, "strata": strata, "outcomes": outcomes}
    if mode == "smoke":
        return report
    n = design["worlds_per_stratum"]
    null = strata[NULL]
    possible = null["rejected"] + null["unresolved"] + null["unstarted"]
    detected = strata[PLANT]["rejected"]
    report["bounds"] = {
        "possible_false_positives": possible,
        "detected_plants": detected,
        "false_positive_upper_millionths": upper_bound_millionths(n, possible),
        "sensitivity_lower_millionths": 10**6 - upper_bound_millionths(n, n - detected),
        "false_positive_pass": possible <= design["maximum_possible_false_positives"],
        "sensitivity_pass": detected >= design["minimum_detected_plants"],
    }
    return report


def run_panel(mode, evaluator, output):
    evaluator = evaluator.resolve(strict=True)
    output = output.resolve()
    design = plan()
    if Fraction(design["minimum_rank"]) > RANK_CUTOFF:
        raise ValueError("the registered rank cutoff is unreachable")
    slots = list(roster(mode))
    output.mkdir()
    evaluator = shutil.copy2(evaluator, output / "clab-eval")
    evaluator_digest = sha256(evaluator.read_bytes()).hexdigest()
    write_json(output / "manifest.json", {
        "mode": mode, "design": design, "slots": slots, "evaluator_sha256": evaluator_digest,
    })
    for slot in slots:
        launch(evaluator, output / slot["id"], slot)
    report = audit(output)
    write_json(output / "report.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("plan")
    for mode in ("smoke", "run"):
        command = commands.add_parser(mode)
        command.add_argument("--eval", type=Path, required=True)
        command.add_argument("--out", type=Path, required=True)
    check = commands.add_parser("audit")
    check.add_argument("out", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "plan":
            result = plan()
        elif args.mode == "audit":
            result = audit(args.out)
        else:
            result = run_panel(args.mode, args.eval, args.out)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
