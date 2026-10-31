# Official-V5 development benchmark

This directory supplies development measurements, not the organizer's evaluator.
The current task Markdown omits the fixed official build/SQL code blocks. The
defaults below are explicit development choices, not a claim to reproduce the
unavailable official SQL. Requires Python 3, NumPy and PyMySQL at runtime.

`prepare` creates independent FP32 queries, non-contiguous external IDs, immutable
NumPy arrays and top-100 truth. Truth uses FP64 norm/dot arithmetic in bounded
query/base blocks and merges block top-k results; it never materializes Q×N×D.
Tie ordering uses source base-row order. Arrays are hashed and checked before
every run. Raw vectors and truth are prepared outside the construction timer.

```bash
OPENBLAS_NUM_THREADS=8 python tools/amd_v5_bench/bench.py prepare \
  --output /root/amd-v5-data/synthetic128 \
  --rows 250000 --dim 128 --queries 4096

OPENBLAS_NUM_THREADS=8 python tools/amd_v5_bench/bench.py prepare \
  --output /root/amd-v5-data/synthetic256 \
  --rows 250000 --dim 256 --queries 4096
```

Optional `--base-fvecs /path/to/base.fvecs --query-fvecs /path/to/query.fvecs`
uses the first requested rows/queries from public fvecs. Such a subset is still
development data unless confirmed identical to the organizer's SIFT250K. Gaussian
data is explicitly labelled synthetic and must never be called Hidden250K.

Each `run` starts a new seekdb process, refuses an existing work directory,
imports vectors into a table with no vector index, then times CREATE VECTOR
INDEX through the first fixed query's complete result. No ANN warmup precedes
this interval. It subsequently runs all prepared queries independently as SQL,
first Top-10 then Top-100. It never calls `dbms_vector.batch_knn`.

```bash
python tools/amd_v5_bench/bench.py run \
  --observer /path/to/baseline/seekdb \
  --bridge /path/to/baseline/libseekdb_cuvs_bridge.so \
  --source /workspace/seekdb-amd2 \
  --dataset /root/amd-v5-data/synthetic128 \
  --work-dir /root/amd-v5-runs/baseline-synthetic128 \
  --cpu-count 16 --memory-budget 32G --ef-search 64
```

Default import is server-side `LOAD DATA INFILE` with a generated tab-separated
FP32-roundtrip input. If this server's LOAD DATA configuration rejects it, the
run fails without silently changing the import method. `--import-mode insert`
selects a documented development alternative outside the construction timer.
Defaults use `m=16, ef_construction=200, ef_search=64`; `--ddl-file` and
`--query-file` allow a known official SQL template to replace these defaults.
DDL placeholders are `{backend}`, `{dim}`, `{m}`, `{ef_construction}` and
`{ef_search}`. A query template uses `{k}` and exactly one PyMySQL `%s` vector
parameter and returns the external ID in its first column.

Query literals are prepared before timing. QPS uses the entire query loop wall
time, including result-ID validation and collection. `qps_sql_only` additionally
reports time spent in SQL calls; P50/P95 use complete SQL call latency. Every
query must return k unique legal IDs. Predictions and per-query timings remain
available as `.npy` artifacts. JSON includes source SHA/diff, dataset hashes,
binary/bridge hashes, SQL, command, timings, recall and failure details. PASS
means execution/result-integrity checks passed, not all competition gates.

Stock V5 performs offline GPU construction during snapshot serialization and
does not emit `cuvs_build` in that path. Therefore `--trace` requires precisely
`1+2*nq` `cuvs_serve` records rather than incorrectly requiring a lazy-build trace.
Trace runs are diagnostic; performance runs leave runtime trace disabled.
GPU build proof requires profiler or examined build logs; the harness does not
claim that disabled trace or a device-memory reading proves GPU execution.
For `lib=cuvs`, even trace-disabled runs fail on negative diagnostics in this
run's own `observer.stdout.log` or `log/seekdb.log*`: bridge search errors, bridge
build failures, or a lazy VSAG fallback. Checks run after the timed first query,
after each timed query group and after shutdown; evidence is retained without
overwriting an earlier failure. This catches known silent CPU fallback cases,
but absence of these messages alone is not positive GPU-profiler proof.
`--log-level INFO` retains stock GPU snapshot build/restore log evidence if
needed, but changes logging overhead and must be matched between compared runs.

`--observer-prefix 'rocprofv3 ... --'` optionally wraps the seekdb process.
The benchmark creates its own session and only signals its own process group
when stopping it, including the profiler. No existing databases or directories
are deleted, and no unrelated seekdb instances are stopped. Defaults reserve
4 GiB of initial data/log files per run; root-disk run directories are preferable
to a 4 GiB `/tmp`. This script does not drop Linux page cache or guarantee an
otherwise idle machine. External resource monitoring should record host/GPU
peaks alongside the JSON. The harness additionally samples seekdb RSS and GPU
sysfs VRAM every 50 ms (`--sample-interval`), recording idle, observed peaks and
startup/import/build/query/shutdown phases. GPU counters are selected through
read/write-accessible `/dev/dri/renderD*` character devices and deduplicated by
real sysfs path, so unrelated host cards are excluded. If no render mapping is
available, an unfiltered sysfs-card fallback is explicitly marked in
`resources.gpu_device_selection.mode`. Without a profiler prefix it reads the launched PID; with a prefix it
finds the seekdb executable in the newly created process group. JSON records the
launcher/server PIDs and all observed seekdb PIDs. RSS values convert Linux kB
to bytes; `VmHWM` is also recorded. VRAM is device-wide and includes other users,
and short-lived peaks may fall between samples. These observations do not prove
compliance with official profiler/process resource limits.

For an initial smoke, prepare a separate `--rows 1000 --queries 10` dataset.
For alternating before/after measurements over two prepared datasets:

```bash
python tools/amd_v5_bench/compare.py \
  --before /path/to/baseline/seekdb --after /path/to/candidate/seekdb \
  --before-bridge /path/to/baseline/libseekdb_cuvs_bridge.so \
  --after-bridge /path/to/candidate/libseekdb_cuvs_bridge.so \
  --source /workspace/seekdb-amd2 \
  --datasets /root/amd-v5-data/synthetic128 /root/amd-v5-data/synthetic256 \
  --output /root/amd-v5-runs/new-comparison --rounds 1
```

Dataset 0 runs baseline first; dataset 1 candidate first. Each binary receives a
fresh process/directory and the same prepared data. `comparison.json` reports
paired build/query ratios and relative quality checks, but does not assign
official scores or apply SIFT/Hidden absolute thresholds to synthetic data.
`--extra-run-args` passes shared `run` flags and must appear last.

Validate the metric helpers:

```bash
python -m unittest discover -s tools/amd_v5_bench -p test_bench.py -v
```

For restart and independent CPU-path regression, `lifecycle_check.py` reopens an
existing **small disposable** `bench.py run` database (256..10000 rows only),
checks persisted GPU results and fresh GPU-snapshot INFO log evidence, creates
`cpu_items` from at most 2048 original vectors, checks a `lib=vsag` index, then
restarts again and queries that CPU table. It leaves the original `items` table
unchanged but permanently adds `cpu_items`; do not point it at full comparison
databases. It refuses an already existing output JSON or CPU table. It uses its
own process groups/ports (3081/3082), records PIDs, and stops only its processes.

```bash
python tools/amd_v5_bench/lifecycle_check.py \
  --observer /path/to/candidate/seekdb --bridge /path/to/candidate/bridge.so \
  --dataset /root/amd-v5-data/smoke \
  --existing-dir /root/amd-v5-runs/old-disposable-smoke \
  --output /root/amd-v5-results/candidate-lifecycle.json

python -m unittest discover -s tools/amd_v5_bench -p test_lifecycle.py -v
```

This is a functional regression, not timing or profiler proof. The first restart
must log GPU snapshot deserialization without a lazy VSAG fallback; the second
restart does not require the absence of GPU activity, because loading other
tables can legitimately restore the GPU index in the same database. It does
not hide GPU devices or assume cuVS snapshots are readable without a GPU.
