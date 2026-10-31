#!/usr/bin/env python3
"""Restart a SMALL disposable benchmark DB; add and reopen a CPU reference table.

This mutates only the supplied small regression database by adding cpu_items.
It must not target full-scale comparison directories. INFO log evidence is a
local diagnostic and does not replace the organizer's GPU profiler.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time
import traceback

import numpy as np

from bench import ground_truth, literal, save_json, sha256, sql, stop_process_group, trace_counts, validate_ids
from compare import PROFILES


def log_offsets(directory):
    offsets = {}
    for path in (directory / "log").glob("*.log*"):
        if path.is_file() and path.suffix not in (".gz", ".zst"):
            stat = path.stat()
            offsets[(stat.st_dev, stat.st_ino)] = stat.st_size
    return offsets


def new_log_evidence(directory, offsets, extra_files=()):
    messages = []
    fallback = []
    errors = []
    for path in list((directory / "log").glob("*.log*")) + list(extra_files):
        if path.is_file() and path.suffix not in (".gz", ".zst"):
            stat = path.stat()
            previous = offsets.get((stat.st_dev, stat.st_ino), 0)
            # Handle truncation, as well as rotated files retaining the inode.
            start = previous if previous <= stat.st_size else 0
            with path.open("rb") as stream:
                stream.seek(start)
                for raw in stream:
                    line = raw.decode("utf-8", errors="replace").rstrip()
                    if "deserialized GPU CAGRA snapshot" in line:
                        messages.append(line[-3000:])
                    if "lazily built VSAG fallback" in line:
                        fallback.append(line[-3000:])
                    search_error = re.search(r"\[seekdb_cuvs\]\s+search rc=([+-]?\d+)", line)
                    if "[seekdb_cuvs] build failed" in line or (search_error and int(search_error.group(1)) != 0):
                        errors.append(line[-3000:])
    return {"deserialized_gpu_snapshot": messages[-10:], "lazy_vsag_fallback": fallback[-10:],
            "gpu_errors": errors[-10:],
            "scope": "new INFO log bytes for this restart, not GPU-profiler proof"}


def query_checks(connection, table, query_values, truth, valid, *, base, ids, query_vectors,
                 profile, reference=None):
    if not len(query_values) or len(query_values) != len(truth) or len(query_vectors) != len(truth):
        raise ValueError("Missing query truth rows")
    positions = {int(value): index for index, value in enumerate(ids)}
    result = {"queries_per_k": len(query_values), "neighbors": {}, "profile": profile}
    for k in (10, 100):
        hits = 0
        neighbors = []
        for index, (value, expected) in enumerate(zip(query_values, truth)):
            rows = sql(connection,
                       f"SELECT id,l2_distance(v,%s) FROM {table} "
                       f"ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {k}", (value, value))
            found = [int(row[0]) for row in rows]
            validate_ids(found, valid, k)
            distances = np.asarray([float(row[1]) for row in rows], dtype=np.float64)
            if not np.isfinite(distances).all() or np.any(distances < 0) or np.any(np.diff(distances) < -1e-4):
                raise ValueError("Invalid or unordered SQL L2 distances")
            selected = np.asarray(base[[positions[value] for value in found]], dtype=np.float64)
            exact = np.sqrt(np.sum((selected - np.asarray(query_vectors[index], dtype=np.float64)) ** 2, axis=1))
            np.testing.assert_allclose(distances, exact, rtol=3e-5, atol=1e-4,
                                       err_msg="SQL L2 distance disagrees with source row identified by external ID")
            if reference is not None:
                prior = reference["neighbors"][str(k)][index]
                if set(found) != set(prior["ids"]):
                    raise ValueError(f"Restart changed top{k} ID set at query {index}")
                if "distances" in prior:
                    prior_distances = dict(zip(prior["ids"], prior["distances"]))
                    np.testing.assert_allclose(distances, [prior_distances[value] for value in found],
                                               rtol=3e-5, atol=1e-4, err_msg="Restart changed SQL L2 distances")
            neighbors.append({"ids": found, "distances": distances.tolist()})
            hits += len(set(found).intersection(expected[:k]))
        result[f"recall_at_{k}"] = hits / (len(query_values) * k)
        if result[f"recall_at_{k}"] + 1e-12 < PROFILES[profile][k]:
            raise ValueError(f"{profile} Recall@{k} below absolute development floor")
        if reference is not None and result[f"recall_at_{k}"] - reference[f"recall_at_{k}"] < -0.005 - 1e-12:
            raise ValueError(f"Restart Recall@{k} dropped by more than 0.005")
        result["neighbors"][str(k)] = neighbors
    return result


def original_predictions(directory, truth, valid, nq):
    result = {"neighbors": {}}
    for k in (10, 100):
        prediction = np.load(directory / f"predicted-k{k}.npy", allow_pickle=False)
        if prediction.ndim != 2 or len(prediction) < nq or prediction.shape[1] != k:
            raise ValueError("Missing original predictions for restart comparison")
        rows = prediction[:nq].tolist()
        for row in rows:
            validate_ids(row, valid, k)
        result["neighbors"][str(k)] = [{"ids": row} for row in rows]
        result[f"recall_at_{k}"] = sum(len(set(row).intersection(expected[:k]))
                                       for row, expected in zip(rows, truth[:nq])) / (nq * k)
    return result


@contextmanager
def instance(args, stage, result):
    import pymysql
    for port in (args.port, args.rpc_port):
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", port))
    env = os.environ.copy()
    trace = args.output.with_name(args.output.stem + f".{stage}.gpu.trace")
    if trace.exists():
        raise FileExistsError(trace)
    env.update(LD_LIBRARY_PATH=":".join([str(args.bridge.parent), "/opt/hipvs/lib", "/opt/rocm/lib",
                                        env.get("LD_LIBRARY_PATH", "")]),
               OB_VSAG_TRACE="1", OB_VSAG_TRACE_FILE=str(trace))
    env.pop("OB_VSAG_USE_CUVS", None)
    command = [str(args.observer), "--nodaemon", "--base-dir", str(args.existing_dir),
               "--port", str(args.port), "--log-level", "INFO"]
    for parameter in (f"rpc_port={args.rpc_port}", f"memory_budget={args.memory_budget}",
                      f"cpu_count={args.cpu_count}", "datafile_size=2G", "datafile_maxsize=8G",
                      "log_disk_size=2G", "local_ip=127.0.0.1"):
        command += ["--parameter", parameter]
    connection = process = None
    log_path = args.output.with_name(args.output.stem + f".{stage}.stdout.log")
    if log_path.exists():
        raise FileExistsError(f"Refusing to overwrite: {log_path}")
    stage_info = {"command": command, "stdout_log": str(log_path), "trace_path": str(trace)}
    result["instances"][stage] = stage_info
    with log_path.open("w") as log:
        try:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            stage_info["pid"] = process.pid
            deadline = time.monotonic() + args.startup_timeout
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"seekdb exited: {process.returncode}")
                try:
                    connection = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                                                 autocommit=True, connect_timeout=3,
                                                 read_timeout=600, write_timeout=600)
                    sql(connection, "SELECT 1")
                    break
                except pymysql.MySQLError:
                    if connection is not None:
                        connection.close()
                        connection = None
                    time.sleep(1)
            if connection is None:
                raise TimeoutError("SQL startup timeout")
            sql(connection, "SET SESSION ob_query_timeout=600000000")
            sql(connection, "USE amd_v5_dev")
            yield connection
        finally:
            if connection is not None:
                try:
                    connection.close()
                except Exception as error:
                    stage_info["connection_close_error"] = repr(error)
            if process is not None:
                stage_info["forced_shutdown"] = stop_process_group(process)
                if stage_info["forced_shutdown"]:
                    raise RuntimeError("Lifecycle server required SIGKILL; graceful persistence is not established")
            stage_info["trace_final"] = trace_counts(trace)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("observer", "bridge", "dataset", "existing-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--port", type=int, default=3081)
    parser.add_argument("--rpc-port", type=int, default=3082)
    parser.add_argument("--cpu-count", type=int, default=16)
    parser.add_argument("--memory-budget", default="32G")
    parser.add_argument("--queries", type=int, default=10)
    parser.add_argument("--profile", required=True, choices=tuple(PROFILES),
                        help="Apply explicit development recall floors, not an assertion of official dataset identity")
    parser.add_argument("--startup-timeout", type=int, default=180)
    args = parser.parse_args()
    for name in ("observer", "bridge", "dataset", "existing_dir", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output.exists() or not (args.existing_dir / "store").is_dir():
        parser.error("Need an existing small benchmark store and fresh output JSON")
    if args.port == args.rpc_port or min(args.cpu_count, args.queries, args.startup_timeout) < 1:
        parser.error("Distinct ports and positive sizes required")
    manifest = json.loads((args.dataset / "manifest.json").read_text())
    if manifest.get("status") != "READY" or not 256 <= manifest["rows"] <= 10000:
        parser.error("Only disposable development datasets of 256..10000 rows are permitted")
    original = json.loads((args.existing_dir / "result.json").read_text())
    if original.get("dataset", {}).get("hashes") != manifest["hashes"]:
        parser.error("Existing database and supplied prepared dataset hashes do not match")
    for name, expected in manifest["hashes"].items():
        if sha256(args.dataset / name) != expected:
            parser.error(f"Dataset hash mismatch: {name}")
    data = {name: np.load(args.dataset / (name + ".npy"), mmap_mode="r", allow_pickle=False)
            for name in ("base", "queries", "ids", "truth")}
    nq = min(args.queries, len(data["queries"]))
    query_vectors = data["queries"][:nq]
    query_values = [literal(value) for value in query_vectors]
    valid_gpu = set(map(int, data["ids"]))
    gpu_reference = original_predictions(args.existing_dir, data["truth"], valid_gpu, nq)
    cpu_count = min(2048, len(data["base"]))
    cpu_base, cpu_ids = data["base"][:cpu_count], data["ids"][:cpu_count]
    cpu_truth = ground_truth(cpu_base, query_vectors, cpu_ids)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {"status": "FAIL", "scope": "small disposable lifecycle regression; not performance scoring",
              "existing_dir": str(args.existing_dir), "dataset_manifest": manifest,
              "mutation": "creates cpu_items and idx_cpu_v; leaves items unchanged",
              "observer_sha256": sha256(args.observer), "bridge_sha256": sha256(args.bridge),
              "instances": {}}
    save_json(args.output, result)
    try:
        first_offsets = log_offsets(args.existing_dir)
        with instance(args, "restart1", result) as connection:
            if sql(connection, "SELECT COUNT(*) FROM items")[0][0] != manifest["rows"]:
                raise ValueError("Original GPU table row count mismatch")
            result["gpu_restart"] = query_checks(connection, "items", query_values,
                                                  data["truth"][:nq], valid_gpu, base=data["base"], ids=data["ids"],
                                                  query_vectors=query_vectors, profile=args.profile, reference=gpu_reference)
            first_trace = Path(result["instances"]["restart1"]["trace_path"])
            if trace_counts(first_trace).get("cuvs_serve", 0) != 2 * nq:
                raise RuntimeError("Restart GPU SQL did not have one CAGRA serving trace per query")
            # Refuse to overwrite a prior regression table. A failure may leave
            # this table behind; the next run must use another disposable DB.
            sql(connection, f"CREATE TABLE cpu_items(id BIGINT PRIMARY KEY,v VECTOR({manifest['dim']}))")
            for offset in range(0, cpu_count, 200):
                values = [(int(index), literal(vector)) for index, vector in
                          zip(cpu_ids[offset:offset + 200], cpu_base[offset:offset + 200])]
                with connection.cursor() as cursor:
                    cursor.executemany("INSERT INTO cpu_items VALUES (%s,%s)", values)
            sql(connection, "CREATE VECTOR INDEX idx_cpu_v ON cpu_items(v) WITH "
                            "(distance=l2,type=hnsw,lib=vsag,m=16,ef_construction=200,ef_search=64)")
            result["cpu_created"] = query_checks(connection, "cpu_items", query_values,
                                                  cpu_truth, set(map(int, cpu_ids)), base=cpu_base, ids=cpu_ids,
                                                  query_vectors=query_vectors, profile=args.profile)
            if trace_counts(first_trace).get("cuvs_serve", 0) != 2 * nq:
                raise RuntimeError("Explicit lib=vsag CPU SQL unexpectedly used GPU serving")
        result["restart1_log_evidence"] = new_log_evidence(args.existing_dir, first_offsets,
            [Path(result["instances"]["restart1"]["stdout_log"])])
        if not result["restart1_log_evidence"]["deserialized_gpu_snapshot"]:
            raise RuntimeError("Restart did not log restoration of a persisted GPU CAGRA snapshot")
        if result["restart1_log_evidence"]["lazy_vsag_fallback"]:
            raise RuntimeError("GPU restart unexpectedly built a lazy CPU VSAG fallback")
        if result["restart1_log_evidence"]["gpu_errors"]:
            raise RuntimeError("GPU diagnostics report a build/search error during restart")
        second_offsets = log_offsets(args.existing_dir)
        with instance(args, "restart2", result) as connection:
            if sql(connection, "SELECT COUNT(*) FROM cpu_items")[0][0] != cpu_count:
                raise ValueError("Persisted CPU table row count mismatch")
            result["cpu_restart"] = query_checks(connection, "cpu_items", query_values,
                                                  cpu_truth, set(map(int, cpu_ids)), base=cpu_base, ids=cpu_ids,
                                                  query_vectors=query_vectors, profile=args.profile,
                                                  reference=result["cpu_created"])
            second_trace = Path(result["instances"]["restart2"]["trace_path"])
            second_counts = trace_counts(second_trace)
            if not second_trace.is_file() or sum(second_counts.get(key, 0) for key in ("knn_simple", "knn_iterctx")) < 2 * nq:
                raise RuntimeError("Restarted CPU index lacks enabled indexed-query trace evidence")
            if second_counts.get("cuvs_serve", 0):
                raise RuntimeError("Restarted lib=vsag CPU SQL unexpectedly used GPU serving")
        result["restart2_log_evidence"] = new_log_evidence(args.existing_dir, second_offsets,
            [Path(result["instances"]["restart2"]["stdout_log"])])
        if result["restart2_log_evidence"]["lazy_vsag_fallback"] or result["restart2_log_evidence"]["gpu_errors"]:
            raise RuntimeError("CPU restart emitted GPU error/fallback diagnostics")
        result["status"] = "PASS"
    except Exception:
        result["error"] = traceback.format_exc()
    save_json(args.output, result)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
