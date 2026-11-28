"""Run the conditional second-entry calibration on fresh matched fixtures."""

import argparse
from fractions import Fraction
from hashlib import sha256
import json
from pathlib import Path
import shutil
import subprocess

from audit_repetition_support import check_nominee, read_frames, validate_layout
from calibrate_motifs import rejection_ceiling, upper_bound_millionths, write_json
from repetition_growth import beats, costs, matched_traces, score_trace, supported_definition


PROTOCOL = Path(__file__).resolve().parents[1] / "protocols/E01-categorical-repetition-conditional-support.json"


def evaluate_group(directory, evaluator, slot, protocol):
    population, decision = protocol["population"], protocol["decision"]
    train, guard, test = (protocol["split"][key] for key in ("train", "guard", "test"))
    shape = (train + guard + test, population["slots"], len(population["labels"]))
    group = slot["group"]
    directory.mkdir()
    frames, results = {}, {}
    for condition, trace in matched_traces(slot["seed"], group, protocol).items():
        run_id = f"{group}-{condition}"
        (directory / f"{run_id}.cltr").write_bytes(trace)
        digest = sha256(trace).hexdigest()
        frames[condition] = tuple(read_frames(directory, run_id, digest, shape))
        results[condition] = {"trace_sha256": digest}
    a = frames["repeat"][0][0][:-1]
    b = a[::-1]
    varied = tuple(i for i, block in enumerate(frames["diversify"][0]) if block[:-1] == b)
    if sorted(a) != population["labels"] or len(varied) != population["slots"] - population["early_motif_slots"]:
        raise ValueError("invalid generated layout")
    for condition, result in results.items():
        try:
            validate_layout(frames[condition], condition, a, b, varied, population["labels"],
                            population["separator_start"], exclude_reverse=False)
            scores = score_trace(evaluator, directory / f"{group}-{condition}.cltr", protocol)
            selected, bits = supported_definition(scores)
            result.update(scores=scores, heldout_model=selected)
            if selected == "pair":
                checked = check_nominee(frames[condition], scores["pair"], () if condition == "repeat" else varied,
                                        a, b, train, guard, test)
                result.update(nominee=checked["nominee"], test_matches=checked["test_matches_by_frame"],
                              tail=checked["a_only_excluded_tail"])
                if Fraction(result["tail"]) > Fraction(*decision["tail_cutoff"]):
                    first = scores["single"]
                    eligible = first["model_bits"] and beats(costs(first), costs(first, "baseline_"))
                    selected, bits = ("single", costs(first)[0]) if eligible else ("none", 0)
            result.update(status="complete", selected=selected, definition_bits=bits)
        except (OSError, KeyError, TypeError, ValueError, subprocess.TimeoutExpired) as error:
            result.update(status="unresolved", reason=str(error))
    outcome = slot | {"conditions": results, "growth_pass": False}
    if all(result["status"] == "complete" for result in results.values()):
        early, repeat, diversify = (results[name]["definition_bits"] for name in ("early", "repeat", "diversify"))
        added, repeated = diversify - early, repeat - early
        limit = Fraction(*decision["maximum_repetition_fraction_of_growth"])
        outcome.update(repetition_change_bits=repeated, diversity_change_bits=added,
                       growth_pass=results["early"]["selected"] == decision["required_early_model"]
                       and added >= decision["minimum_growth_definition_bits"]
                       and abs(repeated) <= limit * added)
    return outcome


def summarize(outcomes, decision, n):
    unstarted = n - len(outcomes)
    if unstarted < 0:
        raise ValueError("outcomes exceed the registered roster")
    possible = unstarted + sum(row["conditions"]["early"].get("selected") == "pair"
                              or row["conditions"]["early"]["status"] != "complete" for row in outcomes)
    detected = sum(row["conditions"]["diversify"].get("selected") == "pair" for row in outcomes)
    growth = sum(row["growth_pass"] for row in outcomes)
    tail = Fraction(*decision["family_error"]) / 2
    ceiling = rejection_ceiling(n, Fraction(*decision["maximum_false_positive_rate"]), tail)
    floor = n - rejection_ceiling(n, 1 - Fraction(*decision["minimum_sensitivity"]), tail)
    if (ceiling, floor) != (decision["maximum_possible_false_positives"], decision["minimum_detected_plants"]):
        raise ValueError("registered count thresholds disagree with exact binomial bounds")
    return {"possible_false_positives": possible, "detected_plants": detected, "passing_growth_groups": growth,
            "unstarted_groups": unstarted,
            "false_positive_upper_millionths": upper_bound_millionths(n, possible, tail),
            "sensitivity_lower_millionths": 10**6 - upper_bound_millionths(n, n - detected, tail),
            "pass": possible <= ceiling and detected >= floor and growth >= decision["minimum_passing_growth_groups"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("smoke", "run"))
    parser.add_argument("--eval", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    protocol_bytes = PROTOCOL.read_bytes()
    protocol = json.loads(protocol_bytes)
    size = protocol["population"]["smoke_groups" if args.mode == "smoke" else "matched_groups"]
    slots = [{"group": f"group-{i:03}",
              "seed": sha256(f"{protocol['protocol']}\n{args.mode}\n{i}\n".encode()).hexdigest()}
             for i in range(size)]
    evaluator = args.eval.resolve(strict=True)
    output = args.out.resolve()
    output.mkdir()
    evaluator = Path(shutil.copy2(evaluator, output / "clab-eval"))
    manifest = {"protocol": protocol["protocol"], "protocol_sha256": sha256(protocol_bytes).hexdigest(),
                "mode": args.mode, "slots": slots, "evaluator_sha256": sha256(evaluator.read_bytes()).hexdigest()}
    write_json(output / "manifest.json", manifest)
    outcomes = []
    for slot in slots:
        directory = output / slot["group"]
        outcome = evaluate_group(directory, evaluator, slot, protocol)
        write_json(directory / "outcome.json", outcome)
        outcomes.append(outcome)
    report = manifest | {"outcomes": outcomes}
    if args.mode == "run":
        report["decision"] = summarize(outcomes, protocol["decision"], size)
    write_json(output / "report.json", report)


if __name__ == "__main__":
    main()
