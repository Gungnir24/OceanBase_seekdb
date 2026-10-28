#!/usr/bin/env python3
"""Fallback row loader: stream INSERT batches through parallel mysql clients.

`LOAD DATA INFILE` is preferred (see amd_cagra_bench.py); this path exists for
servers where server-side INFILE is disabled. It is slower but only used to
prepare the shared data directory once.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import threading

import numpy as np

CHUNK_ROWS = 200


def insert_worker(args, start: int, end: int, base, errors: list[str]) -> None:
    proc = subprocess.Popen(
        [args.mysql, "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
         f"-P{args.port}", "-uroot", "-A", "-N", "-B"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
    )
    try:
        for chunk_start in range(start, end, CHUNK_ROWS):
            chunk_end = min(end, chunk_start + CHUNK_ROWS)
            rows = []
            for row_id in range(chunk_start, chunk_end):
                vec = base[row_id]
                rows.append(f"({row_id},'[" + ",".join(f"{value:.9g}" for value in vec) + "]')")
            proc.stdin.write(f"insert into {args.table} (id, v) values " + ",".join(rows) + ";\n")
        proc.stdin.close()
        stderr = proc.stderr.read()
        if proc.wait() != 0:
            errors.append(stderr[:400])
    except BrokenPipeError:
        errors.append(proc.stderr.read()[:400])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--mysql", default="mysql")
    parser.add_argument("--table", default="bench.t")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()

    dataset = pathlib.Path(args.dataset)
    meta = json.loads((dataset / "meta.json").read_text())
    n, dim = meta["n"], meta["dim"]
    base = np.memmap(dataset / "base.f32", dtype=np.float32, mode="r", shape=(n, dim))

    workers = max(1, args.workers)
    per = (n + workers - 1) // workers
    errors: list[str] = []
    threads = []
    for index in range(workers):
        start = index * per
        end = min(n, start + per)
        if start >= end:
            continue
        thread = threading.Thread(target=insert_worker, args=(args, start, end, base, errors))
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()
    if errors:
        print("load errors:\n" + "\n".join(errors), file=sys.stderr)
        return 1
    print(f"loaded {n} rows with {workers} workers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
