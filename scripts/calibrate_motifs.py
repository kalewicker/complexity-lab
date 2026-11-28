"""Register, run, and audit fixed synthetic temporal motif panels."""
import argparse
import csv
from dataclasses import dataclass
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

@dataclass(frozen=True)
class CalibrationProtocol:
    data: dict
    digest: str
    command: str
    shared_sites: bool

    @property
    def split(self):
        return tuple(self.data["split"][key] for key in ("train", "guard", "test", "sites"))

    @property
    def strata(self):
        return self.data["population"]["strata"]

    @property
    def tail_error(self):
        return Fraction(*self.data["decision"]["family_error"]) / len(self.strata)


def load_protocol(filename, command, shared_sites=False):
    path = Path(__file__).resolve().parents[1] / "protocols" / filename
    data = path.read_bytes()
    return CalibrationProtocol(json.loads(data), sha256(data).hexdigest(), command, shared_sites)


FRAME_PROTOCOL = load_protocol("E01-categorical-motif-calibration.json", "--categorical-motif-permutation")
SITE_PROTOCOL = load_protocol("E01-categorical-motif-site-calibration.json",
                              "--categorical-motif-site-permutation", shared_sites=True)
DICTIONARY_PROTOCOL = load_protocol("E01-categorical-motif-dictionary-site-calibration.json",
                                    "--categorical-motif-dictionary-site-permutation", shared_sites=True)
PROTOCOLS = {
    "independent-frame": FRAME_PROTOCOL,
    "shared-site": SITE_PROTOCOL,
    "dictionary-shared-site": DICTIONARY_PROTOCOL,
}


def binomial_cdf(n, k, rate):
    a, b = rate.numerator, rate.denominator
    return Fraction(sum(comb(n, i) * a**i * (b - a)**(n - i)
                        for i in range(k + 1)), b**n)


def rejection_ceiling(n, maximum_rate, tail_error):
    low, high = -1, n
    while low + 1 < high:
        mid = (low + high) // 2
        if binomial_cdf(n, mid, maximum_rate) <= tail_error:
            low = mid
        else:
            high = mid
    return low


def upper_bound_millionths(n, count, tail_error=FRAME_PROTOCOL.tail_error):
    # Round the one-sided exact upper bound outward on a millionth grid.
    low, high = 0, 10**6
    while low + 1 < high:
        mid = (low + high) // 2
        if binomial_cdf(n, count, Fraction(mid, 10**6)) <= tail_error:
            high = mid
        else:
            low = mid
    return high


def plan(protocol=FRAME_PROTOCOL):
    decision = protocol.data["decision"]
    n = protocol.data["population"]["worlds_per_stratum"]
    ceiling = rejection_ceiling(n, Fraction(*decision["maximum_false_positive_rate"]), protocol.tail_error)
    floor = n - rejection_ceiling(n, 1 - Fraction(*decision["minimum_sensitivity"]), protocol.tail_error)
    return {
        "protocol": protocol.data["protocol"],
        "protocol_sha256": protocol.digest,
        "worlds_per_stratum": n,
        "draws_per_world": decision["draws"],
        "minimum_rank": str(Fraction(1, decision["draws"] + 1)),
        "rank_cutoff": str(Fraction(*decision["rank_cutoff"])),
        "tail_error_per_bound": str(protocol.tail_error),
        "maximum_possible_false_positives": ceiling,
        "minimum_detected_plants": floor,
        "planning_null_pass_probability": round(float(binomial_cdf(
            n, ceiling, Fraction(*decision["planning_false_positive_rate"]))), 8),
        "planning_plant_pass_probability": round(float(binomial_cdf(
            n, n - floor, 1 - Fraction(*decision["planning_sensitivity"]))), 8),
    }


def roster(mode, protocol=FRAME_PROTOCOL):
    n = (protocol.data["population"]["worlds_per_stratum"] if mode == "run"
         else protocol.data["execution"]["smoke_worlds_per_stratum"])
    for stratum in protocol.strata:
        for i in range(n):
            slot = f"{stratum}-{i:03}"
            namespace = f'{protocol.data["protocol"]}\n{mode}\n{slot}\n'.encode()
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


def shared_site_order(rng, sites, persistent, planted):
    if not planted:
        order = list(range(sites))
        rng.shuffle(order)
        return order
    offset = rng.below(sites - persistent + 1)
    nuisance_sites = list(range(persistent, sites))
    rng.shuffle(nuisance_sites)
    return nuisance_sites[:offset] + list(range(persistent)) + nuisance_sites[offset:]


def separated_block_order(rng, sites, first_width, second_width):
    starts = [
        (first, second)
        for first in range(sites - first_width + 1)
        for second in range(sites - second_width + 1)
        if first + first_width < second or second + second_width < first
    ]
    first, second = starts[rng.below(len(starts))]
    persistent = first_width + second_width
    nuisance = list(range(persistent, sites))
    rng.shuffle(nuisance)
    order = [None] * sites
    order[first:first + first_width] = range(first_width)
    order[second:second + second_width] = range(first_width, persistent)
    remaining = iter(nuisance)
    return [next(remaining) if index is None else index for index in order]


def world_trace(slot, protocol=FRAME_PROTOCOL):
    train, guard, test, sites = protocol.split
    population = protocol.data["population"]
    blocks = population.get("motifs")
    motif = [label for block in blocks for label in block] if blocks else population["motif"]
    width = sites - len(motif)
    rng = FrameRandom(slot["input_seed"])
    planted = slot["stratum"] == protocol.strata[1]
    order = None
    if blocks and planted:
        order = separated_block_order(rng, sites, *map(len, blocks))
    elif protocol.shared_sites:
        order = shared_site_order(rng, sites, len(motif), planted)
    data = bytearray(header(META | {"run_id": slot["id"]}))
    for i in range(train + guard + test):
        start = population["nuisance_label_start"] + width * i
        nuisance = list(range(start, start + width))
        if order is not None:
            rng.shuffle(nuisance)
            canonical = motif + nuisance
            labels = [canonical[site] for site in order]
        elif planted and (i < train or i >= train + guard):
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


def launch(evaluator, directory, slot, protocol):
    directory.mkdir()
    trace = directory / "trace.cltr"
    data = world_trace(slot, protocol)
    trace.write_bytes(data)
    draws = protocol.data["decision"]["draws"]
    command = [str(evaluator), protocol.command, *map(str, (*protocol.split, draws, slot["permutation_seed"])),
               str(trace)]
    receipt = {"trace_sha256": sha256(data).hexdigest()}
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=protocol.data["execution"]["timeout_seconds_per_world"])
        receipt.update(status="exited", returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
    except subprocess.TimeoutExpired as error:
        receipt.update(status="timeout", stdout=(error.stdout or b"").decode("utf-8", "replace"),
                       stderr=(error.stderr or b"").decode("utf-8", "replace"))
    except OSError as error:
        receipt.update(status="launch_failed", error=str(error))
    write_json(directory / "receipt.json", receipt)


def checked_rank(csv_text, slot, protocol):
    row, = csv.DictReader(io.StringIO(csv_text))
    if None in row or None in row.values() or row["run_id"] != slot["id"]:
        raise ValueError("row does not identify the planned world")
    split_fields = ("train_snapshots", "guard_snapshots", "test_snapshots", "sites")
    split = protocol.split
    draw_count = protocol.data["decision"]["draws"]
    if (tuple(int(row[key]) for key in split_fields) != split
            or tuple(int(row[key]) for key in ("train_seen", "guard_seen", "test_seen")) != split[:3]
            or row["encoding"] != META["encoding"] or row["exact"] != "true"
            or row["permutation_id"] != protocol.data["permutation_id"]
            or int(row["seed"]) != slot["permutation_seed"]
            or row["status"] != "valid" or int(row["draws_requested"]) != draw_count
            or int(row["draws_complete"]) != draw_count or int(row["draws_budget_exceeded"]) != 0):
        raise ValueError("evaluator did not complete the planned permutation split")
    first_entry = protocol.data["decision"].get("comparator") == "first_entry"
    gain = row["charged_gain_vs_first_bits" if first_entry else "charged_gain_bits"]
    absence = "no_training_dictionary" if first_entry else "no_training_motif"
    if row["reason"] != ("" if gain else absence):
        raise ValueError("observed motif status is inconsistent")
    if gain:
        comparator = "first" if first_entry else "baseline"
        baseline = int(row[f"{comparator}_model_bits"]) + int(row[f"{comparator}_test_bits"])
        coded = int(row["model_bits"]) + int(row["test_bits"])
        if baseline - coded != int(gain):
            raise ValueError("observed gain does not reconcile with recorded costs")
    observed = max(0, int(gain)) if gain else 0
    draws = row["draw_gains_bits"].split()
    if len(draws) != draw_count or draws.count("absent") != int(row[f"draws_{absence}"]):
        raise ValueError("draw accounting is inconsistent")
    extreme = sum((0 if token == "absent" else max(0, int(token))) >= observed for token in draws)
    numerator, denominator = int(row["p_numerator"]), int(row["p_denominator"])
    if (int(row["statistic_bits"]), int(row["extreme_draws"]), numerator, denominator) != (
            observed, extreme, extreme + 1, draw_count + 1):
        raise ValueError("rank does not reconcile with recorded costs")
    return numerator, denominator


def outcome(output, slot, protocol):
    directory = output / slot["id"]
    if not directory.exists():
        return {"status": "unstarted"}
    receipt_path = directory / "receipt.json"
    if not receipt_path.is_file():
        return {"status": "unresolved", "reason": "missing_receipt"}
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        data = (directory / "trace.cltr").read_bytes()
        if sha256(data).hexdigest() != receipt["trace_sha256"] or data != world_trace(slot, protocol):
            return {"status": "unresolved", "reason": "trace_mismatch"}
        if receipt["status"] != "exited":
            return {"status": "unresolved", "reason": receipt["status"]}
        if receipt["returncode"] != 0:
            return {"status": "unresolved", "reason": "evaluator_exit"}
        rank = checked_rank(receipt["stdout"], slot, protocol)
    except (OSError, KeyError, TypeError, ValueError) as error:
        return {"status": "unresolved", "reason": str(error)}
    cutoff = Fraction(*protocol.data["decision"]["rank_cutoff"])
    return {"status": "rejected" if Fraction(*rank) <= cutoff else "retained", "rank": rank}


def audit(output, protocol=None):
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    mode = manifest["mode"]
    if protocol is None:
        registered_id = manifest["design"]["protocol"]
        protocol = next((item for item in PROTOCOLS.values() if item.data["protocol"] == registered_id), None)
        if protocol is None:
            raise ValueError("unknown protocol in manifest")
    design = plan(protocol)
    slots = list(roster(mode, protocol))
    if (mode not in ("run", "smoke") or manifest["design"] != design or manifest["slots"] != slots):
        raise ValueError("manifest differs from the registered roster")
    if sha256((output / "clab-eval").read_bytes()).hexdigest() != manifest["evaluator_sha256"]:
        raise ValueError("evaluator snapshot differs from its registered digest")

    outcomes = [slot | outcome(output, slot, protocol) for slot in slots]
    strata = {}
    for stratum in protocol.strata:
        statuses = [entry["status"] for entry in outcomes if entry["stratum"] == stratum]
        strata[stratum] = {status: statuses.count(status)
                           for status in ("rejected", "retained", "unresolved", "unstarted")}

    report = {"mode": mode, "design": design, "strata": strata, "outcomes": outcomes}
    if mode == "smoke":
        return report
    n = design["worlds_per_stratum"]
    null_stratum, plant_stratum = protocol.strata
    null = strata[null_stratum]
    possible = null["rejected"] + null["unresolved"] + null["unstarted"]
    detected = strata[plant_stratum]["rejected"]
    report["bounds"] = {
        "possible_false_positives": possible,
        "detected_plants": detected,
        "false_positive_upper_millionths": upper_bound_millionths(n, possible, protocol.tail_error),
        "sensitivity_lower_millionths": 10**6 - upper_bound_millionths(n, n - detected, protocol.tail_error),
        "false_positive_pass": possible <= design["maximum_possible_false_positives"],
        "sensitivity_pass": detected >= design["minimum_detected_plants"],
    }
    return report


def run_panel(mode, evaluator, output, protocol=FRAME_PROTOCOL):
    evaluator = evaluator.resolve(strict=True)
    output = output.resolve()
    design = plan(protocol)
    if Fraction(design["minimum_rank"]) > Fraction(design["rank_cutoff"]):
        raise ValueError("the registered rank cutoff is unreachable")
    slots = list(roster(mode, protocol))
    output.mkdir()
    evaluator = shutil.copy2(evaluator, output / "clab-eval")
    evaluator_digest = sha256(evaluator.read_bytes()).hexdigest()
    write_json(output / "manifest.json", {
        "mode": mode, "design": design, "slots": slots, "evaluator_sha256": evaluator_digest,
    })
    for slot in slots:
        launch(evaluator, output / slot["id"], slot, protocol)
    report = audit(output, protocol)
    write_json(output / "report.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", choices=tuple(PROTOCOLS),
                        help="select a panel (defaults to independent-frame, or the archived protocol for audit)")
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("plan")
    for mode in ("smoke", "run"):
        command = commands.add_parser(mode)
        command.add_argument("--eval", type=Path, required=True)
        command.add_argument("--out", type=Path, required=True)
    check = commands.add_parser("audit")
    check.add_argument("out", type=Path)
    args = parser.parse_args()
    protocol = PROTOCOLS[args.protocol] if args.protocol else None
    try:
        if args.mode == "plan":
            result = plan(protocol or FRAME_PROTOCOL)
        elif args.mode == "audit":
            result = audit(args.out, protocol)
        else:
            result = run_panel(args.mode, args.eval, args.out, protocol or FRAME_PROTOCOL)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
