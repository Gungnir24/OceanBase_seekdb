#!/usr/bin/env python3
"""Alternate baseline/candidate per prepared dataset using bench.py.

Dataset 0 runs baseline first, dataset 1 candidate first. Results and checks are
development evidence, not official scoring or GPU-build-path verification.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--before-bridge", type=Path, required=True)
    parser.add_argument("--after-bridge", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--datasets", nargs="+", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--extra-run-args", nargs=argparse.REMAINDER, default=[],
                        help="Pass shared run flags last, e.g. --import-mode insert")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("rounds must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"scope": "development paired runs, NOT official evaluator", "status": "RUNNING", "pairs": []}
    output = args.output / "comparison.json"
    for number in range(1, args.rounds + 1):
        for dataset_number, dataset in enumerate(args.datasets):
            pair = {"round": number, "dataset": str(dataset.resolve()), "runs": {}}
            report["pairs"].append(pair)
            order = ("before", "after") if dataset_number % 2 == 0 else ("after", "before")
            for label in order:
                work = args.output / f"r{number}-d{dataset_number}-{label}"
                cmd = [sys.executable, str(Path(__file__).with_name("bench.py")), "run",
                       "--observer", str(getattr(args, label)),
                       "--bridge", str(getattr(args, label + "_bridge")),
                       "--source", str(args.source), "--dataset", str(dataset),
                       "--work-dir", str(work), "--label", label] + args.extra_run_args
                with (args.output / f"r{number}-d{dataset_number}-{label}.log").open("w") as log:
                    completed = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
                if (work / "result.json").exists():
                    pair["runs"][label] = json.loads((work / "result.json").read_text())
                if completed.returncode:
                    report.update(status="FAIL", failed_run=str(work))
                    output.write_text(json.dumps(report, indent=2) + "\n")
                    return completed.returncode
                output.write_text(json.dumps(report, indent=2) + "\n")
                print(f"round {number}, dataset {dataset_number}, {label}: PASS", flush=True)
            before, after = pair["runs"]["before"], pair["runs"]["after"]
            assert before["dataset"]["hashes"] == after["dataset"]["hashes"]
            ratios = [after[f"single_k{k}"]["qps"] / before[f"single_k{k}"]["qps"] for k in (10, 100)]
            deltas = {str(k): after[f"single_k{k}"]["recall"] - before[f"single_k{k}"]["recall"] for k in (10, 100)}
            pair["development_comparison"] = {
                "build_speedup": before["build_e2e_seconds"] / after["build_e2e_seconds"],
                "query_speedup_geomean": math.sqrt(ratios[0] * ratios[1]),
                "recall_deltas": deltas,
                "relative_recall_check": all(delta >= -0.005 - 1e-12 for delta in deltas.values()),
                "query_90pct_check": math.sqrt(ratios[0] * ratios[1]) >= 0.9,
                "official_absolute_recall_check": "not applied to development datasets"}
            output.write_text(json.dumps(report, indent=2) + "\n")
    report["status"] = "PASS"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(str(output), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
