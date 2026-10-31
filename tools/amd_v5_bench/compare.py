#!/usr/bin/env python3
"""Paired development runs with explicit quality/resource proxy gates.

Profiles apply contest numerical thresholds to development data. PASS is not
official-equivalent; sampled memory and GPU-profiler proof remain distinct.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys

GIB = 1024 ** 3
PROFILES = {"sift": {10: 0.95, 100: 0.95}, "hidden": {10: 0.90, 100: 0.85}}


def number(value, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return None
    return value if value >= 0 and (not positive or value > 0) else None


def evaluate_pair(before, after, profile):
    checks = []
    def check(name, condition, evidence):
        checks.append({"name": name, "status": "UNKNOWN" if condition is None else "PASS" if condition else "FAIL",
                       "evidence": evidence})
    for label, run in (("before", before), ("after", after)):
        check(label + "_run", run.get("status") == "PASS" if "status" in run else None, run.get("status"))
        count = run.get("dataset", {}).get("queries")
        check(label + "_dataset_4096_queries", count == 4096 if count is not None else None, count)
    before_hashes, after_hashes = before.get("dataset", {}).get("hashes"), after.get("dataset", {}).get("hashes")
    keys = {"base.npy", "queries.npy", "ids.npy", "truth.npy"}
    valid_hashes = all(isinstance(h, dict) and keys <= set(h)
                       and all(isinstance(h[key], str) and h[key] for key in keys)
                       for h in (before_hashes, after_hashes))
    check("paired_dataset_hashes", before_hashes == after_hashes if valid_hashes else None,
          "all four prepared array hashes must be present and identical")
    ratios, deltas = [], {}
    for k in (10, 100):
        old, new = before.get(f"single_k{k}", {}), after.get(f"single_k{k}", {})
        for label, item in (("before", old), ("after", new)):
            count = item.get("queries")
            check(f"{label}_k{k}_4096_queries", count == 4096 if count is not None else None, count)
        old_recall, new_recall = number(old.get("recall")), number(new.get("recall"))
        if old_recall is not None and old_recall > 1: old_recall = None
        if new_recall is not None and new_recall > 1: new_recall = None
        floor = PROFILES[profile][k]
        check(f"k{k}_absolute_recall", new_recall + 1e-12 >= floor if new_recall is not None else None,
              {"candidate": new_recall, "minimum": floor, "profile": profile})
        delta = new_recall - old_recall if old_recall is not None and new_recall is not None else None
        deltas[str(k)] = delta
        check(f"k{k}_relative_recall", delta + 1e-12 >= -0.005 if delta is not None else None,
              {"delta": delta, "minimum": -0.005})
        old_qps, new_qps = number(old.get("qps"), True), number(new.get("qps"), True)
        ratios.append(new_qps / old_qps if old_qps is not None and new_qps is not None else None)
    geomean = math.sqrt(ratios[0] * ratios[1]) if all(r is not None for r in ratios) else None
    check("query_90pct", geomean + 1e-12 >= 0.9 if geomean is not None else None,
          {"geomean": geomean, "minimum": 0.9})
    memory = {}
    for label, run in (("before", before), ("after", after)):
        resource = run.get("resources", {})
        samples = number(resource.get("samples"), True)
        rss, hwm = number(resource.get("peak_rss_sampled_bytes"), True), number(resource.get("peak_vm_hwm_single_process_bytes"), True)
        sample_ok = bool(samples and resource.get("seekdb_pids") and not resource.get("sampling_error"))
        host = max(rss, hwm) if sample_ok and rss is not None and hwm is not None else None
        check(label + "_host_memory_proxy", host <= 96 * GIB if host is not None else None,
              {"peak_bytes": host, "limit_bytes": 96 * GIB, "sampling_error": resource.get("sampling_error")})
        selection = resource.get("gpu_device_selection", {})
        counters = resource.get("gpu_peak_sampled_bytes")
        # One assigned GPU is required. Host-wide fallback/multiple counters
        # cannot establish which device owns the scored memory peak.
        gpu_ok = (sample_ok and selection.get("mode") == "accessible_render_nodes"
                  and isinstance(counters, dict) and len(counters) == 1
                  and all(number(v, True) is not None for v in counters.values()))
        gpu = counters if gpu_ok else None
        memory[label] = gpu
        check(label + "_physical_vram_proxy", all(v <= 45 * GIB for v in gpu.values()) if gpu else None,
              {"sampled_device_peak_bytes": gpu, "limit_bytes": 45 * GIB,
               "scope": "device-wide sampled peak; development proxy, not exact profiler peak",
               "selection": selection.get("mode")})
    old_gpu, new_gpu = memory["before"], memory["after"]
    relative = (all(new_gpu[key] <= 1.25 * old_gpu[key] for key in old_gpu)
                if old_gpu and new_gpu and old_gpu.keys() == new_gpu.keys() else None)
    check("relative_vram_proxy", relative, {"before": old_gpu, "after": new_gpu, "maximum_ratio": 1.25})
    old_build, new_build = number(before.get("build_e2e_seconds"), True), number(after.get("build_e2e_seconds"), True)
    check("build_timing_present", True if old_build is not None and new_build is not None else None,
          "CREATE through completed first legal query")
    return {"status": "PASS" if all(c["status"] == "PASS" for c in checks) else "FAIL",
            "profile": profile, "checks": checks, "recall_deltas": deltas,
            "query_speedup_geomean": geomean,
            "build_speedup": old_build / new_build if old_build is not None and new_build is not None else None,
            "coverage_exclusions": ["external GPU build/search profiler", "separate CPU/lifecycle regression",
                                    "official datasets/SQL/compiler/resource environment", "exact unsampled memory peaks"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "before-bridge", "after-bridge", "source", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--datasets", nargs="+", type=Path, required=True)
    parser.add_argument("--profile", nargs="+", choices=tuple(PROFILES), required=True,
                        help="One development quality profile per dataset, in the same order")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--extra-run-args", nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args()
    if args.rounds < 1 or len(args.profile) != len(args.datasets):
        parser.error("Need positive rounds and exactly one --profile entry per dataset")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"scope": "development paired gates; sampled memory proxy, NOT official evaluator",
              "status": "RUNNING", "pairs": []}
    output = args.output / "comparison.json"
    def save():
        output.write_text(json.dumps(report, indent=2) + "\n")
    for round_number in range(1, args.rounds + 1):
        for dataset_number, (dataset, profile) in enumerate(zip(args.datasets, args.profile)):
            pair = {"round": round_number, "dataset": str(dataset.resolve()), "profile": profile, "runs": {}}
            report["pairs"].append(pair)
            order = ("before", "after") if (dataset_number + round_number - 1) % 2 == 0 else ("after", "before")
            for label in order:
                work = args.output / f"r{round_number}-d{dataset_number}-{label}"
                cmd = [sys.executable, str(Path(__file__).with_name("bench.py")), "run",
                       "--observer", str(getattr(args, label)), "--bridge", str(getattr(args, label + "_bridge")),
                       "--source", str(args.source), "--dataset", str(dataset), "--work-dir", str(work),
                       "--label", label] + args.extra_run_args
                with (args.output / f"r{round_number}-d{dataset_number}-{label}.log").open("w") as log:
                    completed = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
                result_file = work / "result.json"
                try:
                    pair["runs"][label] = json.loads(result_file.read_text())
                except (OSError, ValueError) as error:
                    pair["runs"][label] = {"status": "UNKNOWN", "error": str(error)}
                if completed.returncode or pair["runs"][label].get("status") != "PASS":
                    report.update(status="FAIL", failed_run=str(work))
                    save()
                    return 1
                save()
            pair["development_comparison"] = evaluate_pair(pair["runs"]["before"], pair["runs"]["after"], profile)
            if pair["development_comparison"]["status"] != "PASS":
                report.update(status="FAIL", failed_pair={"round": round_number, "dataset": str(dataset)})
                save()
                return 1
            save()
    report["status"] = "PASS"
    save()
    print(str(output), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
