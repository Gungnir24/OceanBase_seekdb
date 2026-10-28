#!/usr/bin/env python3
"""Single-query latency / batch throughput / recall measurement.

Every query is issued as its own SQL statement over one mysql session, which is
what the grader's "single query" regression check looks like. Latency is the
wall time of each statement; QPS is nq / total wall time.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import subprocess
import time

import numpy as np

WARMUP = 5


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--mysql", default="mysql")
    parser.add_argument("--topk", type=int, default=10)
    parser.add_argument("--nq", type=int, default=100)
    parser.add_argument("--table", default="bench.t")
    args = parser.parse_args()

    dataset = pathlib.Path(args.dataset)
    meta = json.loads((dataset / "meta.json").read_text())
    dim = meta["dim"]
    gt = np.fromfile(dataset / "gt.i32", dtype=np.int32).reshape(meta["nq"], meta["topk"])
    queries = np.fromfile(dataset / "query.f32", dtype=np.float32).reshape(meta["nq"], dim)
    limit = max(args.topk, 100)
    limit = min(limit, meta["topk"])

    proc = subprocess.Popen(
        # --unbuffered is required: without it the batch client block-buffers its
        # result rows and the reader below never sees a complete response.
        [args.mysql, "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
         f"-P{args.port}", "-uroot", "-A", "-N", "-B", "--unbuffered"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )

    SENTINEL = "__seekdb_done__"

    def ask(query, k: int) -> tuple[list[int], float]:
        literal = "[" + ",".join(f"{value:.9g}" for value in query) + "]"
        # The batch client keeps reading after a statement, so each query is
        # terminated by a sentinel result set instead of waiting for a blank
        # line (which batch mode never emits for a piped session).
        sql = (f"use bench; select id from {args.table} order by l2_distance(v, '{literal}') "
               f"approximate limit {k}; select '{SENTINEL}';\n")
        started = time.perf_counter()
        proc.stdin.write(sql)
        proc.stdin.flush()
        ids = []
        while True:
            line = proc.stdout.readline()
            if line == "":
                raise RuntimeError(proc.stderr.read()[:500])
            token = line.strip()
            if token == SENTINEL:
                break
            if token.isdigit() or (token.startswith("-") and token[1:].isdigit()):
                ids.append(int(token))
        return ids, time.perf_counter() - started

    for index in range(min(WARMUP, args.nq)):
        ask(queries[index], limit)

    hits10 = hits100 = 0
    latencies = []
    started = time.perf_counter()
    for index in range(args.nq):
        ids, seconds = ask(queries[index], limit)
        latencies.append(seconds * 1000.0)
        predicted = ids[:limit]
        truth = gt[index][:meta["topk"]]
        hits10 += len(set(predicted[:10]) & set(int(v) for v in truth[:10]))
        hits100 += len(set(predicted[:100]) & set(int(v) for v in truth[:100]))
    total = time.perf_counter() - started

    proc.stdin.close()
    proc.wait(timeout=60)

    ordered = sorted(latencies)
    p50 = ordered[len(ordered) // 2]
    p95 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))]
    result = {
        "nq": args.nq,
        "limit": limit,
        "recall_at_10": round(hits10 / (10.0 * args.nq), 4),
        "recall_at_100": round(hits100 / (min(100, limit) * float(args.nq)), 4),
        "qps": round(args.nq / total, 2),
        "p50_ms": round(p50, 3),
        "p95_ms": round(p95, 3),
        "mean_ms": round(statistics.fmean(latencies), 3),
    }
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
