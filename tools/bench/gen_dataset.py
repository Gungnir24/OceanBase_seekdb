#!/usr/bin/env python3
"""Deterministic FP32 dataset and exact ground truth for the build benchmark.

Writes base.f32 (n x dim, row major), query.f32 (nq x dim) and gt.i32
(nq x topk, exact L2 neighbours) plus meta.json. The base matrix is memory
mapped so that 1M x 768 does not need to fit in RAM twice.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import time

import numpy as np

CHUNK = 65536


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--n", type=int, default=100_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--nq", type=int, default=100)
    parser.add_argument("--topk", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261028)
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    base = np.memmap(out / "base.f32", dtype=np.float32, mode="w+", shape=(args.n, args.dim))
    for start in range(0, args.n, CHUNK):
        end = min(args.n, start + CHUNK)
        block = rng.standard_normal((end - start, args.dim), dtype=np.float32)
        norms = np.linalg.norm(block, axis=1, keepdims=True)
        block /= np.maximum(norms, 1e-12)
        base[start:end] = block
    base.flush()

    picks = rng.choice(args.n, size=args.nq, replace=False)
    queries = np.empty((args.nq, args.dim), dtype=np.float32)
    for index, row in enumerate(picks):
        noise = rng.standard_normal(args.dim, dtype=np.float32) * 0.05
        vector = np.asarray(base[row], dtype=np.float32) + noise
        queries[index] = vector / max(float(np.linalg.norm(vector)), 1e-12)
    queries.tofile(out / "query.f32")

    topk = min(args.topk, args.n)
    gt = np.empty((args.nq, topk), dtype=np.int32)
    distances = np.empty(args.n, dtype=np.float32)
    started = time.time()
    for index, query in enumerate(queries):
        for start in range(0, args.n, CHUNK):
            end = min(args.n, start + CHUNK)
            diff = np.asarray(base[start:end]) - query
            distances[start:end] = np.einsum("ij,ij->i", diff, diff)
        candidates = np.argpartition(distances, topk - 1)[:topk]
        gt[index] = candidates[np.argsort(distances[candidates])]
    gt.tofile(out / "gt.i32")

    meta = {
        "n": args.n,
        "dim": args.dim,
        "nq": args.nq,
        "topk": topk,
        "seed": args.seed,
        "metric": "l2",
        "gt_seconds": round(time.time() - started, 3),
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(json.dumps(meta))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
