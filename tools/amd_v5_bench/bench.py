#!/usr/bin/env python3
"""V5 development benchmark: fresh offline DDL, then independent single-row SQL.

This is NOT the organizer's evaluator. Fixed official SQL/parameters and data
must be supplied separately before results can be called official-equivalent.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import threading
import time
import traceback

import numpy as np


def discover_gpu_devices(dev_dir=Path("/dev/dri"), sysfs_dir=Path("/sys/class/drm")):
    """Select sysfs memory counters through render nodes exposed to this job."""
    devices, render_nodes = set(), []
    for node in dev_dir.glob("renderD*"):
        counter = sysfs_dir / node.name / "device" / "mem_info_vram_used"
        if (node.is_char_device() and os.access(node, os.R_OK | os.W_OK)
                and counter.is_file() and os.access(counter, os.R_OK)):
            devices.add(str(counter.resolve()))
            render_nodes.append(str(node))
    mode = "accessible_render_nodes"
    if not devices:
        # Some environments omit render-node sysfs links. Retain diagnostic
        # counters but explicitly mark them as an unfiltered host-wide fallback.
        devices = {str(path.resolve()) for path in
                   sysfs_dir.glob("card[0-9]*/device/mem_info_vram_used")
                   if path.is_file() and os.access(path, os.R_OK)}
        mode = "all_sysfs_cards_fallback_no_accessible_render_mapping" if devices else "unavailable"
    return sorted(devices), {"mode": mode, "accessible_render_nodes": sorted(render_nodes),
                             "counter_paths": sorted(devices),
                             "note": "device-wide counters include other processes on selected devices"}


class ResourceSampler:
    """Low-overhead process RSS and device-wide VRAM observations, not a profiler."""
    def __init__(self, observer, wrapped, interval=0.05):
        self.observer = observer.resolve()
        self.wrapped = wrapped
        self.interval = interval
        self.phase = "startup"
        self.pid = None
        self.stopped = threading.Event()
        self.thread = None
        self.devices, device_selection = discover_gpu_devices()
        idle = self.gpu_values()
        self.result = {"scope": "sampled seekdb RSS and device-wide VRAM; not exact profiler peaks",
                       "sample_interval_seconds": interval, "samples": 0, "seekdb_pids": [],
                       "gpu_device_selection": device_selection,
                       "peak_rss_sampled_bytes": 0, "peak_vm_hwm_single_process_bytes": 0,
                       "gpu_idle_bytes": idle, "gpu_peak_sampled_bytes": dict(idle),
                       "phase_peaks": {}}

    def gpu_values(self):
        values = {}
        for path in self.devices:
            try:
                values[path] = int(Path(path).read_text().strip())
            except (OSError, ValueError):
                pass
        return values

    def seekdb_pids(self):
        if not self.wrapped:
            return [self.pid]
        found = []
        for path in Path("/proc").iterdir():
            if not path.name.isdigit():
                continue
            try:
                stat = (path / "stat").read_text()
                # Fields after the parenthesized comm start with state, ppid, pgrp.
                fields = stat[stat.rfind(")") + 2:].split()
                if int(fields[2]) == self.pid and (path / "exe").resolve() == self.observer:
                    found.append(int(path.name))
            except (OSError, ValueError, IndexError):
                pass
        return found

    def sample(self):
        rss = 0
        for pid in self.seekdb_pids():
            try:
                values = {}
                for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                    if line.startswith(("VmRSS:", "VmHWM:")):
                        key, value, _ = line.split()
                        values[key.rstrip(":")] = int(value) * 1024
                if values and pid not in self.result["seekdb_pids"]:
                    self.result["seekdb_pids"].append(pid)
                rss += values.get("VmRSS", 0)
                self.result["peak_vm_hwm_single_process_bytes"] = max(
                    self.result["peak_vm_hwm_single_process_bytes"], values.get("VmHWM", 0))
            except (OSError, ValueError):
                pass
        gpu = self.gpu_values()
        self.result["samples"] += 1
        self.result["peak_rss_sampled_bytes"] = max(self.result["peak_rss_sampled_bytes"], rss)
        phase = self.result["phase_peaks"].setdefault(self.phase, {"rss_bytes": 0, "gpu_bytes": {}})
        phase["rss_bytes"] = max(phase["rss_bytes"], rss)
        for path, value in gpu.items():
            self.result["gpu_peak_sampled_bytes"][path] = max(
                self.result["gpu_peak_sampled_bytes"].get(path, 0), value)
            phase["gpu_bytes"][path] = max(phase["gpu_bytes"].get(path, 0), value)

    def start(self, pid):
        self.pid = pid

        def poll():
            while not self.stopped.is_set():
                try:
                    self.sample()
                except Exception as error:
                    self.result["sampling_error"] = repr(error)
                    break
                self.stopped.wait(self.interval)

        self.thread = threading.Thread(target=poll, name="amd-v5-resource-sampler", daemon=True)
        self.thread.start()

    def stop(self):
        self.stopped.set()
        if self.thread is not None:
            self.thread.join(timeout=5)
            if self.thread.is_alive():
                self.result["sampling_error"] = "sampler did not stop within five seconds"
        self.result["gpu_peak_delta_from_idle_bytes"] = {
            path: max(0, peak - self.result["gpu_idle_bytes"].get(path, peak))
            for path, peak in self.result["gpu_peak_sampled_bytes"].items()}
        return self.result


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def literal(vector):
    return "[" + ",".join(format(float(x), ".9g") for x in vector) + "]"


def load_fvecs(path, count, expected_dim):
    words = np.memmap(path, dtype="<i4", mode="r")
    if not len(words) or int(words[0]) != expected_dim or len(words) % (expected_dim + 1):
        raise ValueError(f"Invalid fvecs shape: {path}")
    records = words.reshape(-1, expected_dim + 1)
    if len(records) < count or not np.all(records[:count, 0] == expected_dim):
        raise ValueError(f"Insufficient or inconsistent fvecs records: {path}")
    return records[:count, 1:].view("<f4").copy()


def ground_truth(base, queries, ids, k=100, query_batch=32, base_block=8192):
    """FP64 squared L2 in bounded QxB blocks, never QxNxD storage.

    Ties use original base-row order. For integer-valued SIFT data the FP64 dot
    products are exact at this scale; arbitrary FP32 data use FP64 arithmetic.
    """
    answer = np.empty((len(queries), k), dtype=np.int64)
    for qs in range(0, len(queries), query_batch):
        query = np.asarray(queries[qs:qs + query_batch], dtype=np.float64)
        qnorm = np.einsum("ij,ij->i", query, query)
        best_d = np.full((len(query), k), np.inf)
        best_i = np.full((len(query), k), len(base), dtype=np.int64)
        for bs in range(0, len(base), base_block):
            block = np.asarray(base[bs:bs + base_block], dtype=np.float64)
            distances = qnorm[:, None] + np.einsum("ij,ij->i", block, block)[None, :]
            distances -= 2.0 * (query @ block.T)
            np.maximum(distances, 0, out=distances)
            take = min(k, len(block))
            part = np.argpartition(distances, take - 1, axis=1)[:, :take]
            for row in range(len(query)):
                local = part[row]
                # argpartition may arbitrarily drop a tied boundary ID.
                cutoff = np.max(distances[row, local])
                below = np.flatnonzero(distances[row] < cutoff)
                equal = np.flatnonzero(distances[row] == cutoff)[:take - len(below)]
                local = np.concatenate((below, equal))
                cand_d = np.concatenate((best_d[row], distances[row, local]))
                cand_i = np.concatenate((best_i[row], local + bs))
                order = np.lexsort((cand_i, cand_d))[:k]
                best_d[row], best_i[row] = cand_d[order], cand_i[order]
        answer[qs:qs + len(query)] = ids[best_i]
        print(f"truth {min(qs + len(query), len(queries))}/{len(queries)}", flush=True)
    return answer


def prepare(args):
    args.output.mkdir(parents=True, exist_ok=False)
    if args.base_fvecs:
        base = load_fvecs(args.base_fvecs, args.rows, args.dim)
        queries = load_fvecs(args.query_fvecs, args.queries, args.dim)
        kind = "public fvecs subset development data; NOT confirmed official SIFT250K"
    else:
        rng = np.random.default_rng(args.seed)
        base = rng.standard_normal((args.rows, args.dim)).astype(np.float32)
        queries = rng.standard_normal((args.queries, args.dim)).astype(np.float32)
        kind = "seeded synthetic FP32 Gaussian; NOT Hidden250K"
    if not np.isfinite(base).all() or not np.isfinite(queries).all():
        raise ValueError("Dataset must contain only finite FP32 values")
    ids = np.arange(args.rows, dtype=np.int64) * 3 + 7
    for name, array in (("base", base), ("queries", queries), ("ids", ids)):
        np.save(args.output / (name + ".npy"), array, allow_pickle=False)
    started = time.perf_counter()
    truth = ground_truth(base, queries, ids, query_batch=args.truth_query_batch,
                         base_block=args.truth_base_block)
    np.save(args.output / "truth.npy", truth, allow_pickle=False)
    manifest = {"status": "READY", "scope": kind, "rows": args.rows, "dim": args.dim,
                "queries": args.queries, "seed": args.seed, "ids": "row*3+7",
                "truth": "FP64 squared-L2 norm/dot, blocked, top100, row-order ties",
                "truth_seconds": time.perf_counter() - started,
                "base_fvecs": str(args.base_fvecs) if args.base_fvecs else None,
                "query_fvecs": str(args.query_fvecs) if args.query_fvecs else None,
                "hashes": {name: sha256(args.output / name)
                           for name in ("base.npy", "queries.npy", "ids.npy", "truth.npy")}}
    save_json(args.output / "manifest.json", manifest)
    print(str(args.output / "manifest.json"), flush=True)


def sql(connection, statement, values=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, values)
        rows = cursor.fetchall()
        while cursor.nextset():
            cursor.fetchall()
        return rows


def validate_ids(predicted, valid, k):
    if len(predicted) != k or len(set(predicted)) != k or any(x not in valid for x in predicted):
        raise ValueError(f"Invalid result: expected {k} unique legal external IDs")


def trace_counts(path):
    counts = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if line.strip():
                key = line.split()[0]
                counts[key] = counts.get(key, 0) + 1
    return counts


def stop_process_group(process):
    """Only stop the process group created for this benchmark's child."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=30)
        return False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=10)
        return True


def import_vectors(connection, args, ids, base):
    if args.import_mode == "load-data":
        # File is visible to this server; do not silently fall back to INSERT.
        data_file = args.dataset / "vectors.tsv"
        if not data_file.exists():
            with data_file.open("w", encoding="ascii", newline="\n") as stream:
                for index, vector in zip(ids, base):
                    stream.write(f"{int(index)}\t{literal(vector)}\n")
        sql(connection, "LOAD DATA INFILE %s INTO TABLE items "
                        "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v)",
            (str(data_file.resolve()),))
    else:
        for start in range(0, len(base), 200):
            values = [(int(index), literal(vector))
                      for index, vector in zip(ids[start:start + 200], base[start:start + 200])]
            with connection.cursor() as cursor:
                cursor.executemany("INSERT INTO items VALUES (%s,%s)", values)


def inspect_gpu_logs(sample):
    # Stock V5 has no cuvs_build trace in the offline serialize path.
    findings = []
    for path in (sample / "log").glob("*.log*"):
        if path.is_file() and path.suffix != ".zst":
            with path.open(errors="replace") as stream:
                for line in stream:
                    if "serialized GPU CAGRA snapshot" in line or "deserialized GPU CAGRA snapshot" in line:
                        findings.append(line[-3000:].rstrip())
    return findings[-20:]


def gpu_error_evidence(sample):
    """Negative GPU diagnostics from this run's new directory only."""
    patterns = ("[seekdb_cuvs] build failed", "lazily built VSAG fallback")
    paths = [sample / "observer.stdout.log"] + sorted((sample / "log").glob("seekdb.log*"))
    evidence = []
    seen_files = set()
    for path in paths:
        if not path.is_file() or path.suffix in (".gz", ".zst"):
            continue
        stat = path.stat()
        identity = (stat.st_dev, stat.st_ino)
        if identity in seen_files:
            continue
        seen_files.add(identity)
        with path.open(errors="replace") as stream:
            for number, line in enumerate(stream, 1):
                matched = next((pattern for pattern in patterns if pattern in line), None)
                search_error = re.search(r"\[seekdb_cuvs\]\s+search rc=([+-]?\d+)", line)
                if search_error and int(search_error.group(1)) != 0:
                    matched = "[seekdb_cuvs] search rc="
                if matched:
                    evidence.append({"file": str(path), "line": number, "pattern": matched,
                                     "text": line[-3000:].rstrip()})
                    if len(evidence) >= 50:
                        return evidence
    return evidence


def check_gpu_errors(args, result, stage, raise_error=True):
    if args.backend != "cuvs":
        return
    evidence = gpu_error_evidence(args.work_dir)
    if evidence:
        previous = result.setdefault("gpu_error_evidence", [])
        known = {(item["file"], item["line"], item["text"]) for item in previous}
        for item in evidence:
            identity = (item["file"], item["line"], item["text"])
            if identity not in known:
                previous.append(dict(item, detected_stage=stage))
                known.add(identity)
        result["status"] = "FAIL"
        message = f"GPU error/CPU fallback diagnostics found after {stage}"
        if raise_error:
            raise RuntimeError(message)
        # Cleanup-time checks must preserve any original SQL/traceback failure.
        result.setdefault("error", message)


def run(args):
    import pymysql
    manifest = json.loads((args.dataset / "manifest.json").read_text())
    if manifest.get("status") != "READY":
        raise ValueError("Dataset was not completely prepared")
    arrays = {name: np.load(args.dataset / (name + ".npy"), mmap_mode="r", allow_pickle=False)
              for name in ("base", "queries", "ids", "truth")}
    base, queries, ids, truth = (arrays[name] for name in ("base", "queries", "ids", "truth"))
    if len(base) != manifest["rows"] or base.shape[1] != manifest["dim"] or len(queries) != manifest["queries"]:
        raise ValueError("Dataset manifest shape mismatch")
    for name, expected in manifest["hashes"].items():
        if sha256(args.dataset / name) != expected:
            raise ValueError(f"Dataset hash mismatch: {name}")
    query_values = [literal(vector) for vector in queries]
    valid = set(map(int, ids))
    for port in (args.port, args.rpc_port):
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", port))
    args.work_dir.mkdir(parents=True, exist_ok=False)
    trace = args.work_dir / "gpu.trace"
    env = os.environ.copy()
    env.update(LD_LIBRARY_PATH=":".join([str(args.bridge.parent), "/opt/hipvs/lib", "/opt/rocm/lib",
                                        env.get("LD_LIBRARY_PATH", "")]),
               OB_VSAG_TRACE="1" if args.trace else "0", OB_VSAG_TRACE_FILE=str(trace))
    env.pop("OB_VSAG_USE_CUVS", None)
    command = shlex.split(args.observer_prefix) + [str(args.observer), "--nodaemon", "--base-dir",
               str(args.work_dir), "--port", str(args.port), "--log-level", args.log_level]
    for parameter in (f"rpc_port={args.rpc_port}", f"memory_budget={args.memory_budget}",
                      f"cpu_count={args.cpu_count}", f"datafile_size={args.datafile_size}",
                      f"datafile_maxsize={args.datafile_maxsize}", f"log_disk_size={args.log_disk_size}",
                      "local_ip=127.0.0.1"):
        command += ["--parameter", parameter]
    options = {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items() if k != "func"}
    result = {"status": "RUNNING", "scope": "development benchmark; official evaluator remains authoritative",
              "args": options, "dataset": manifest, "command": command,
              "launcher_pid": None, "server_pid": None,
              "observer_sha256": sha256(args.observer), "bridge_sha256": sha256(args.bridge),
              "source_sha": subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip(),
              "source_diff": subprocess.check_output(["git", "-C", str(args.source), "diff", "--stat", "HEAD"], text=True),
              "gpu_build_proof": "not established by this harness; requires profiler or examined build diagnostics",
              "gpu_query_proof": "pending trace check" if args.trace else "not established (runtime trace disabled)"}
    report = args.work_dir / "result.json"
    save_json(report, result)
    connection = process = None
    monitor = ResourceSampler(args.observer, bool(args.observer_prefix), args.sample_interval)
    with (args.work_dir / "observer.stdout.log").open("w") as log:
        try:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env,
                                       start_new_session=True)
            result["launcher_pid"] = process.pid
            result["server_pid"] = None if args.observer_prefix else process.pid
            monitor.start(process.pid)
            deadline = time.monotonic() + args.startup_timeout
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"seekdb/wrapper exited with {process.returncode}")
                try:
                    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root", autocommit=True,
                                                 connect_timeout=3, read_timeout=args.sql_timeout,
                                                 write_timeout=args.sql_timeout)
                    sql(connection, "SELECT 1")
                    break
                except pymysql.MySQLError:
                    if connection is not None:
                        connection.close()
                        connection = None
                    time.sleep(1)
            if connection is None:
                raise TimeoutError("SQL startup timeout")
            result["server_version"] = sql(connection, "SELECT VERSION()")[0][0]
            sql(connection, f"SET SESSION ob_query_timeout={args.sql_timeout * 1000000}")
            sql(connection, "ALTER SYSTEM SET ob_vector_memory_limit_percentage=30")
            sql(connection, "CREATE DATABASE amd_v5_dev")
            sql(connection, "USE amd_v5_dev")
            sql(connection, f"CREATE TABLE items(id BIGINT PRIMARY KEY,v VECTOR({manifest['dim']}))")
            monitor.phase = "import"
            imported = time.perf_counter()
            import_vectors(connection, args, ids, base)
            result["import_seconds_not_scored"] = time.perf_counter() - imported
            if sql(connection, "SELECT COUNT(*) FROM items")[0][0] != len(base):
                raise ValueError("Vector import incomplete")
            ddl = (args.ddl_file.read_text() if args.ddl_file else
                   "CREATE VECTOR INDEX idx_v ON items(v) WITH (distance=l2,type=hnsw,lib={backend},"
                   "m={m},ef_construction={ef_construction},ef_search={ef_search})")
            ddl = ddl.format(backend=args.backend, dim=manifest["dim"], m=args.m,
                             ef_construction=args.ef_construction, ef_search=args.ef_search).strip()
            query = (args.query_file.read_text() if args.query_file else
                     "SELECT id FROM items ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {k}").strip()
            result.update(ddl=ddl, query_template=query)
            monitor.phase = "build"
            t0 = time.perf_counter()
            sql(connection, ddl)
            t1 = time.perf_counter()
            first = sql(connection, query.format(k=10), (query_values[0],))
            t2 = time.perf_counter()
            validate_ids([int(row[0]) for row in first], valid, 10)
            result.update(ddl_seconds=t1 - t0, first_query_seconds=t2 - t1, build_e2e_seconds=t2 - t0,
                          trace_after_first_query=trace_counts(trace))
            check_gpu_errors(args, result, "first_query")
            save_json(report, result)
            for k in (10, 100):
                monitor.phase = f"query_k{k}"
                elapsed, predictions = [], []
                statement = query.format(k=k)
                loop_started = time.perf_counter()
                for value in query_values:
                    started = time.perf_counter()
                    rows = sql(connection, statement, (value,))
                    elapsed.append(time.perf_counter() - started)
                    found = [int(row[0]) for row in rows]
                    validate_ids(found, valid, k)
                    predictions.append(found)
                loop_seconds = time.perf_counter() - loop_started
                hits = sum(len(set(found).intersection(expected[:k]))
                           for found, expected in zip(predictions, truth))
                if len(predictions) != len(truth):
                    raise ValueError("Missing query results")
                np.save(args.work_dir / f"predicted-k{k}.npy", np.asarray(predictions, dtype=np.int64))
                np.save(args.work_dir / f"latency-k{k}.npy", np.asarray(elapsed))
                result[f"single_k{k}"] = {"queries": len(queries), "seconds": loop_seconds,
                    "qps": len(queries) / loop_seconds, "qps_sql_only": len(queries) / sum(elapsed),
                    "p50_ms": float(np.percentile(elapsed, 50) * 1000),
                    "p95_ms": float(np.percentile(elapsed, 95) * 1000), "recall": hits / (len(queries) * k)}
                check_gpu_errors(args, result, f"query_k{k}")
                save_json(report, result)
            result["trace_final"] = trace_counts(trace)
            if args.trace:
                expected = 1 + 2 * len(queries) if args.backend == "cuvs" else 0
                if result["trace_final"].get("cuvs_serve", 0) != expected:
                    raise RuntimeError(f"GPU serving count mismatch, expected {expected}")
                result["gpu_query_proof"] = "all measured SQL queries served by GPU trace" if expected else "CPU path has no GPU-serving trace"
            result["status"] = "PASS"
        except Exception:
            result.update(status="FAIL", error=traceback.format_exc())
        finally:
            monitor.phase = "shutdown"
            if connection is not None:
                try:
                    connection.close()
                except Exception as error:
                    result["connection_close_error"] = repr(error)
            if process is not None:
                # Only this subprocess's session/process group is signalled.
                # Signal even if an optional profiler wrapper exited first.
                try:
                    result["forced_shutdown"] = stop_process_group(process)
                except Exception as error:
                    result.update(status="FAIL", shutdown_error=repr(error))
            result["resources"] = monitor.stop()
            if result["server_pid"] is None and len(result["resources"]["seekdb_pids"]) == 1:
                result["server_pid"] = result["resources"]["seekdb_pids"][0]
            try:
                check_gpu_errors(args, result, "shutdown", raise_error=False)
            except Exception as error:
                result.update(status="FAIL", gpu_error_scan_error=repr(error))
                result.setdefault("error", "Could not complete negative GPU diagnostic check")
            result["gpu_snapshot_log_evidence"] = inspect_gpu_logs(args.work_dir)
            save_json(report, result)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    return 0 if result["status"] == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare", help="Create immutable development data and exact top100")
    prep.add_argument("--output", type=Path, required=True)
    prep.add_argument("--rows", type=int, default=250000)
    prep.add_argument("--dim", type=int, default=128)
    prep.add_argument("--queries", type=int, default=4096)
    prep.add_argument("--seed", type=int, default=20261031)
    prep.add_argument("--base-fvecs", type=Path)
    prep.add_argument("--query-fvecs", type=Path)
    prep.add_argument("--truth-query-batch", type=int, default=32)
    prep.add_argument("--truth-base-block", type=int, default=8192)
    prep.set_defaults(func=prepare)
    execute = commands.add_parser("run", help="Run one binary/backend in a fresh instance")
    for name in ("observer", "bridge", "source", "dataset", "work-dir"):
        execute.add_argument("--" + name, type=Path, required=True)
    execute.add_argument("--label", default="baseline-v5")
    execute.add_argument("--backend", choices=("cuvs", "vsag"), default="cuvs")
    execute.add_argument("--port", type=int, default=2981)
    execute.add_argument("--rpc-port", type=int, default=2982)
    execute.add_argument("--cpu-count", type=int, default=16)
    execute.add_argument("--memory-budget", default="32G")
    execute.add_argument("--datafile-size", default="2G")
    execute.add_argument("--datafile-maxsize", default="8G")
    execute.add_argument("--log-disk-size", default="2G")
    execute.add_argument("--m", type=int, default=16)
    execute.add_argument("--ef-construction", type=int, default=200)
    execute.add_argument("--ef-search", type=int, default=64)
    execute.add_argument("--import-mode", choices=("load-data", "insert"), default="load-data")
    execute.add_argument("--sql-timeout", type=int, default=600)
    execute.add_argument("--startup-timeout", type=int, default=180)
    execute.add_argument("--log-level", choices=("WARN", "INFO"), default="WARN")
    execute.add_argument("--observer-prefix", default="", help="Optional shlex-split profiler command")
    execute.add_argument("--sample-interval", type=float, default=0.05, help="RSS/VRAM sampling interval in seconds")
    execute.add_argument("--ddl-file", type=Path, help="DDL template with backend/dim/m/ef_construction/ef_search placeholders")
    execute.add_argument("--query-file", type=Path, help="SQL template: {k} limit and one PyMySQL %s vector parameter")
    execute.add_argument("--trace", action="store_true", help="Require GPU-serving proof; diagnostic timing")
    execute.set_defaults(func=run)
    args = parser.parse_args()
    if args.command == "prepare":
        if args.rows < 256 or min(args.dim, args.queries, args.truth_query_batch, args.truth_base_block) < 1:
            parser.error("Need >=256 rows and positive sizes")
        if bool(args.base_fvecs) != bool(args.query_fvecs):
            parser.error("Supply both base/query fvecs paths")
    else:
        if args.port == args.rpc_port or min(args.cpu_count, args.sql_timeout, args.startup_timeout) < 1 or args.sample_interval <= 0:
            parser.error("Distinct ports and positive limits required")
        for attr in ("observer", "bridge", "source", "dataset", "work_dir"):
            setattr(args, attr, getattr(args, attr).resolve())
    return args.func(args) or 0


if __name__ == "__main__":
    raise SystemExit(main())
