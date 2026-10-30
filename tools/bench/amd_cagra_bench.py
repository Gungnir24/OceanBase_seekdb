#!/usr/bin/env python3
"""Cold-start CAGRA/HNSW vector-index build benchmark for the hipVS contest.

The official grader only accepts numbers produced by its own harness, so this
driver reproduces the *rules* documented in the task statement so that a
candidate can be compared with the frozen baseline on the same machine:

  * the raw vector table is imported before the timer starts;
  * no index, no CAGRA graph and no reusable cache exist at t0;
  * t0 -> t2 covers CREATE VECTOR INDEX, the GPU build, and the first query;
  * every round starts from the same prepared data directory;
  * each configuration runs --rounds times and the median is reported.

Usage (on the AMD/ROCm box, after building seekdb and the bridge):

    ./tools/bench/amd_cagra_bench.py \
        --observer  /work/base/build_release/src/observer/seekdb \
        --label     baseline \
        --bridge    /work/bridge/libseekdb_cuvs_bridge.so \
        --evidence  /work/bench/baseline \
        --base-dir  /work/bench/run-baseline \
        --port 2981 --render /dev/dri/renderD135 \
        --n 100000 --dim 128 --topk 10 --rounds 3

Run it once per build (baseline, candidate, lib=vsag for the CPU column) and
then merge the results with make_report.py.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time

OBSERVER_PARAMS = [
    # Mirrors the parameters shipped with the demo image, which are known to
    # bootstrap on the contest container.
    "--parameter", "_memory_budget=16G",
    "--parameter", "datafile_size=8G",
    "--parameter", "datafile_maxsize=16G",
    "--parameter", "log_disk_size=4G",
    "--log-level", "INFO",
]


def log(msg: str) -> None:
    print(f"[bench] {msg}", flush=True)


def die(msg: str) -> None:
    print(f"[bench][ERROR] {msg}", file=sys.stderr, flush=True)
    raise SystemExit(2)


def port_is_open(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.2)
        return sock.connect_ex(("127.0.0.1", port)) == 0


class Mysql:
    """Thin wrapper around the stock mysql client."""

    def __init__(self, port: int, client: str, timeout: int = 3600) -> None:
        self.port = port
        self.client = client
        self.timeout = timeout

    def _cmd(self, extra: list[str]) -> list[str]:
        return [
            self.client, "--connect-timeout=5", "--protocol=TCP",
            "-h127.0.0.1", f"-P{self.port}", "-uroot", "-A", "-N", "-B",
            *extra,
        ]

    def run(self, sql: str, timeout: int | None = None, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(
            self._cmd(["-e", sql]), capture_output=True, text=True,
            timeout=timeout or self.timeout, check=check,
        )

    def value(self, sql: str) -> str:
        return self.run(sql).stdout.strip()


class Observer:
    def __init__(self, args, run_dir: pathlib.Path, trace: pathlib.Path, log_path: pathlib.Path) -> None:
        self.args = args
        self.run_dir = run_dir
        self.trace = trace
        self.log_path = log_path
        self.proc: subprocess.Popen | None = None

    def __enter__(self) -> "Observer":
        self.start()
        return self

    def __exit__(self, *_exc) -> None:
        self.stop()

    def start(self) -> None:
        env = dict(os.environ)
        env.pop("OB_VSAG_USE_CUVS", None)
        lib_paths = []
        if self.args.bridge:
            lib_paths.append(str(pathlib.Path(self.args.bridge).parent))
        lib_paths += ["/opt/hipvs/lib", "/opt/rocm/lib"]
        env["LD_LIBRARY_PATH"] = ":".join(lib_paths + [env.get("LD_LIBRARY_PATH", "")]).strip(":")
        if self.args.trace:
            env["OB_VSAG_TRACE"] = "1"
            env["OB_VSAG_TRACE_FILE"] = str(self.trace)
            # The accelerated bridge reports cuvs_build_us / cuvs_serve markers
            # only when timing is requested; the shipped one ignores this.
            env["SEEKDB_CUVS_TIMING"] = "1"
        # The GPU-served build path is what this benchmark is about: keep the CPU
        # index as bookkeeping only and let CAGRA serve the queries. Override with
        # --set-env SEEKDB_CUVS_GPU_ONLY=0 / SEEKDB_CUVS_ADOPT=0 to compare with
        # the pure CPU path.
        env.setdefault("SEEKDB_CUVS_GPU_ONLY", "1")
        env.setdefault("SEEKDB_CUVS_ADOPT", "1")
        for item in self.args.set_env:
            name, _, val = item.partition("=")
            env[name] = val
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._handle = self.log_path.open("wb")
        self.proc = subprocess.Popen(
            [
                self.args.observer, "--nodaemon", "--port", str(self.args.port),
                "--base-dir", str(self.run_dir),
                *OBSERVER_PARAMS,
            ],
            env=env, stdout=self._handle, stderr=subprocess.STDOUT,
        )
        mysql = Mysql(self.args.port, self.args.mysql, timeout=30)
        deadline = time.time() + self.args.start_timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                die(f"observer exited early, see {self.log_path}")
            try:
                if mysql.run("select 1", timeout=10, check=False).returncode == 0:
                    return
            except subprocess.TimeoutExpired:
                pass
            time.sleep(1)
        self.stop()
        die(f"observer did not become SQL-ready, see {self.log_path}")

    def stop(self) -> None:
        if self.proc is None:
            return
        proc = self.proc
        self.proc = None
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=30)
        self._handle.close()
        deadline = time.time() + 60
        while time.time() < deadline and port_is_open(self.args.port):
            time.sleep(0.5)


def generate_dataset(out: pathlib.Path, n: int, dim: int, nq: int, topk: int, seed: int) -> None:
    out.mkdir(parents=True, exist_ok=True)
    meta = out / "meta.json"
    if meta.exists():
        data = json.loads(meta.read_text())
        if (data["n"], data["dim"], data["nq"], data["topk"], data["seed"]) == (n, dim, nq, topk, seed):
            log(f"reusing dataset {out} (n={n} dim={dim} nq={nq})")
            return
        shutil.rmtree(out)
        out.mkdir(parents=True, exist_ok=True)
    log(f"generating synthetic dataset n={n} dim={dim} nq={nq} topk={topk}")
    subprocess.run(
        [sys.executable, str(pathlib.Path(__file__).with_name("gen_dataset.py")),
         "--out", str(out), "--n", str(n), "--dim", str(dim),
         "--nq", str(nq), "--topk", str(topk), "--seed", str(seed)],
        check=True,
    )


def write_shards(dataset: pathlib.Path, shards: int, shard_dir: pathlib.Path, dim: int) -> list[pathlib.Path]:
    import numpy as np

    shard_dir.mkdir(parents=True, exist_ok=True)
    meta = json.loads((dataset / "meta.json").read_text())
    n = meta["n"]
    base = np.memmap(dataset / "base.f32", dtype=np.float32, mode="r", shape=(n, dim))
    paths = []
    per = (n + shards - 1) // shards
    for idx in range(shards):
        start = idx * per
        end = min(n, start + per)
        if start >= end:
            break
        path = shard_dir / f"shard-{idx:02d}.tsv"
        if path.exists() and path.stat().st_size > 0:
            paths.append(path)
            continue
        with path.open("w", buffering=1 << 20) as handle:
            for row_id in range(start, end):
                vec = base[row_id]
                handle.write(f"{row_id}\t[")
                handle.write(",".join(f"{value:.9g}" for value in vec))
                handle.write("]\n")
        paths.append(path)
    return paths


def prepare_template(args, dataset: pathlib.Path, template: pathlib.Path) -> None:
    marker = template / ".prepared"
    if marker.exists():
        log(f"reusing prepared data directory {template}")
        return
    if template.exists():
        shutil.rmtree(template)
    template.mkdir(parents=True, exist_ok=True)

    mysql = Mysql(args.port, args.mysql)
    trace = template / "prepare-trace.log"
    with Observer(args, template, trace, template / "prepare-observer.log"):
        mysql.run("create database if not exists bench")
        mysql.run("use bench; create table t(id bigint primary key, v vector(%d))" % args.dim)
        if args.load_mode == "insert":
            log("loading rows with parallel INSERT batches")
            subprocess.run(
                [sys.executable, str(pathlib.Path(__file__).with_name("load_vectors.py")),
                 "--dataset", str(dataset), "--port", str(args.port),
                 "--mysql", args.mysql, "--table", "bench.t",
                 "--workers", str(args.load_workers)],
                check=True,
            )
        else:
            shards = write_shards(dataset, args.load_workers, template / "shards", args.dim)
            log(f"loading {len(shards)} LOAD DATA shards")
            procs = []
            for shard in shards:
                sql = (
                    "use bench; load data infile '%s' into table t "
                    "fields terminated by '\\t' lines terminated by '\\n' (id, v)" % shard
                )
                procs.append(subprocess.Popen(
                    [args.mysql, "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
                     f"-P{args.port}", "-uroot", "-A", "-N", "-B", "-e", sql],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                ))
            for proc in procs:
                _, err = proc.communicate()
                if proc.returncode != 0:
                    die(f"LOAD DATA failed: {err.decode(errors='replace')[:500]}")
            for shard in shards:
                shard.unlink()
            shutil.rmtree(template / "shards", ignore_errors=True)
        rows = mysql.value("select count(*) from bench.t")
        log(f"loaded {rows} rows")
        if int(rows) != args.n:
            die(f"row count mismatch: expected {args.n}, got {rows}")
        mysql.run("alter system flush all", check=False)
    marker.write_text(json.dumps({"n": args.n, "dim": args.dim}))


def trace_counts(trace: pathlib.Path, offset: int) -> dict[str, int]:
    counts: dict[str, int] = {}
    if not trace.exists():
        return counts
    with trace.open("r", errors="replace") as handle:
        for lineno, line in enumerate(handle):
            if lineno < offset:
                continue
            token = line.split(maxsplit=1)[0] if line.strip() else ""
            if token.startswith("cuvs_"):
                counts[token] = counts.get(token, 0) + 1
    return counts


def trace_lines(trace: pathlib.Path) -> int:
    if not trace.exists():
        return 0
    with trace.open("rb") as handle:
        return sum(1 for _ in handle)


def run_round(args, dataset: pathlib.Path, template: pathlib.Path, index: int) -> dict:
    run_dir = args.base_dir / f"round{index}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    log(f"round {index}: copying prepared data directory")
    started = time.time()
    shutil.copytree(
        template, run_dir, symlinks=True,
        # A stopped observer leaves its unix socket and pid file behind; copying
        # them across raises ENXIO, they are recreated on start anyway.
        ignore=shutil.ignore_patterns("*.sock", "*.pid"),
    )
    copy_seconds = time.time() - started

    trace = args.evidence / f"round{index}-trace.log"
    if trace.exists():
        trace.unlink()
    result: dict = {"round": index, "dataset_copy_seconds": round(copy_seconds, 3)}
    mysql = Mysql(args.port, args.mysql)
    with Observer(args, run_dir, trace, args.evidence / f"round{index}-observer.log"):
        offset = trace_lines(trace)
        import numpy as np
        with (dataset / "query.f32").open("rb") as handle:
            first_query = handle.read(4 * args.dim)
        query_vec = np.frombuffer(first_query, dtype=np.float32)
        literal = "[" + ",".join(f"{value:.9g}" for value in query_vec) + "]"

        ddl = (
            "use bench; create vector index idx on t(v) with "
            f"(distance=l2, type=hnsw, lib={args.lib}, m=16, ef_construction=200, ef_search=200)"
        )
        t0 = time.time()
        mysql.run(ddl, timeout=args.build_timeout)
        t1 = time.time()

        # The grader counts the first query inside the build window, so it is
        # issued here even though it also pays any lazy build/transfer cost.
        served = False
        first_query_seconds = 0.0
        deadline = time.time() + args.first_query_timeout
        while True:
            q0 = time.time()
            out = mysql.run(
                "use bench; select id from t order by l2_distance(v, '%s') approximate limit %d"
                % (literal, args.topk), timeout=args.build_timeout,
            )
            first_query_seconds = time.time() - q0
            served = any(line.strip() for line in out.stdout.splitlines())
            counts = trace_counts(trace, offset)
            # A build is "done" once the query returns rows. When the bridge
            # reports trace markers we additionally require the CAGRA search to
            # have served the call, so a silent CPU fallback cannot end the
            # window early. The shipped baseline bridge emits no markers at all,
            # so there we simply trust the returned rows.
            done = served
            if args.trace and args.lib == "cuvs" and counts:
                done = served and counts.get("cuvs_serve", 0) > 0
            if done or time.time() > deadline:
                break
            time.sleep(1)
        t2 = time.time()
        result.update({
            "ddl_seconds": round(t1 - t0, 3),
            "first_query_seconds": round(first_query_seconds, 3),
            "build_seconds": round(t2 - t0, 3),
            "first_query_returned": bool(served),
            "trace": trace_counts(trace, offset),
        })
        if (args.lib == "cuvs" and args.trace and result["trace"]
                and result["trace"].get("cuvs_serve", 0) == 0):
            result["warning"] = "CAGRA search did not serve the first query"

        if not args.skip_query_bench:
            bench = subprocess.run(
                [sys.executable, str(pathlib.Path(__file__).with_name("query_bench.py")),
                 "--dataset", str(dataset), "--port", str(args.port), "--mysql", args.mysql,
                 "--topk", str(args.topk), "--nq", str(args.nq)],
                capture_output=True, text=True, check=True,
            )
            result["query"] = json.loads(bench.stdout)
    if run_dir.exists():
        shutil.rmtree(run_dir)
    return result


def median(values: list[float]) -> float:
    ordered = sorted(values)
    count = len(ordered)
    if count == 0:
        return 0.0
    mid = count // 2
    if count % 2:
        return round(ordered[mid], 3)
    return round((ordered[mid - 1] + ordered[mid]) / 2.0, 3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--observer", required=True)
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--lib", default="cuvs", choices=["cuvs", "vsag"])
    parser.add_argument("--bridge", default="")
    parser.add_argument("--render", default="")
    parser.add_argument("--mysql", default="mysql")
    parser.add_argument("--base-dir", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--port", type=int, default=2981)
    parser.add_argument("--n", type=int, default=100_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--topk", type=int, default=10)
    parser.add_argument("--nq", type=int, default=100)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20261028)
    parser.add_argument("--load-workers", type=int, default=4)
    parser.add_argument("--load-mode", default="load-data", choices=["load-data", "insert"])
    parser.add_argument("--trace", type=int, default=1)
    parser.add_argument("--skip-query-bench", action="store_true")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--reuse-dataset", default="")
    parser.add_argument("--set-env", action="append", default=[],
                        help="NAME=VALUE passed to the observer")
    parser.add_argument("--start-timeout", type=int, default=600)
    parser.add_argument("--build-timeout", type=int, default=86400)
    parser.add_argument("--first-query-timeout", type=int, default=300)
    args = parser.parse_args()

    args.base_dir = pathlib.Path(args.base_dir)
    args.evidence = pathlib.Path(args.evidence)
    args.base_dir.mkdir(parents=True, exist_ok=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    try:  # the observer wants a large fd limit
        import resource
        _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, 65535), hard))
    except Exception:  # noqa: BLE001 - best effort only
        pass

    if not pathlib.Path(args.observer).is_file():
        die(f"observer not found: {args.observer}")
    if args.lib == "cuvs" and not args.bridge:
        die("--bridge is required for lib=cuvs")
    if args.bridge and not pathlib.Path(args.bridge).is_file():
        die(f"bridge not found: {args.bridge}")
    if port_is_open(args.port):
        die(f"port {args.port} is already in use")

    dataset = pathlib.Path(args.reuse_dataset) if args.reuse_dataset else args.evidence / "dataset"
    if args.reuse_dataset:
        log(f"reusing dataset at {dataset}")
    else:
        generate_dataset(dataset, args.n, args.dim, args.nq, args.topk, args.seed)

    template = args.evidence / "template"
    prepare_template(args, dataset, template)
    if args.prepare_only:
        log("prepare-only: done")
        return 0

    rounds = []
    for index in range(1, args.rounds + 1):
        rounds.append(run_round(args, dataset, template, index))
        log(f"round {index}: build={rounds[-1]['build_seconds']}s "
            f"ddl={rounds[-1]['ddl_seconds']}s first_query={rounds[-1]['first_query_seconds']}s")

    summary = {
        "label": args.label,
        "lib": args.lib,
        "observer": args.observer,
        "n": args.n,
        "dim": args.dim,
        "topk": args.topk,
        "nq": args.nq,
        "rounds": rounds,
        "median_build_seconds": median([r["build_seconds"] for r in rounds]),
        "median_ddl_seconds": median([r["ddl_seconds"] for r in rounds]),
    }
    if rounds and "query" in rounds[0]:
        summary["recall_at_10"] = median([r["query"]["recall_at_10"] for r in rounds])
        summary["recall_at_100"] = median([r["query"]["recall_at_100"] for r in rounds])
        summary["qps"] = median([r["query"]["qps"] for r in rounds])
        summary["p50_ms"] = median([r["query"]["p50_ms"] for r in rounds])
        summary["p95_ms"] = median([r["query"]["p95_ms"] for r in rounds])
    out = args.evidence / "summary.json"
    out.write_text(json.dumps(summary, indent=2) + "\n")
    log(f"summary written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
