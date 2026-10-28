#!/bin/bash
set -euo pipefail
root=/workspace/seekdb/file-sql-validation
out=/workspace/seekdb/fts-hotpath-validation
run="$out/run"
mkdir -p "$run"
export MYSQL='mysql -h127.0.0.1 -P32851 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P32851 -uroot -A --default-character-set=utf8mb4'
start_instance() {
  "$1" --nodaemon --base-dir "$run" --port 32851 --parameter rpc_port=32852 --parameter memory_budget=16G --parameter datafile_size=4G --parameter datafile_maxsize=8G --parameter log_disk_size=2G --log-level ERROR > "$out/console-$2.log" 2>&1 &
  server_pid=$!
  echo "$server_pid" > "$out/server.pid"
  for i in $(seq 1 90); do
    if $MYSQL -e 'SELECT 1' >/dev/null 2>&1; then return; fi
    kill -0 "$server_pid" || return 1
    sleep 2
  done
  return 1
}
stop_instance() {
  kill -TERM "$server_pid"
  for i in $(seq 1 90); do
    if ! kill -0 "$server_pid" 2>/dev/null; then wait "$server_pid" || true; return; fi
    sleep 2
  done
  echo 'Server did not stop cleanly' >&2
  return 1
}
bench() {
  test ! -e "$out/$1-report.txt"
  LABEL="$1" ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$out/$1-report.txt" bash "$root/tools/benchmark/fts_large_bench.sh" > "$out/$1-bench.log" 2>&1
  python3 "$root/tools/benchmark/fts_large_bench_score.py" "$out/$1-report.txt" > "$out/$1-score.txt"
}
for iteration in 2 3; do
  echo "PHASE repeat-$iteration before"
  start_instance "$out/seekdb-before" "before-$iteration"
  bench "before-$iteration"
  stop_instance
  echo "PHASE repeat-$iteration after"
  start_instance "$root/build_release/src/observer/seekdb" "after-$iteration"
  bench "after-$iteration"
  stop_instance
done
echo 'REPEATS COMPLETE'
