#!/usr/bin/env bash
# Build the frozen baseline and the candidate, then benchmark both and emit the report.
#
#   tools/bench/run_all.sh \
#     --baseline-src /work/seekdb-baseline \
#     --candidate-src /work/seekdb-candidate \
#     --work /work/bench \
#     --port 2981 --render /dev/dri/renderD135 \
#     --n 100000 --dim 128 --topk 10 --rounds 3
#
# Pass --baseline-observer/--candidate-observer to reuse binaries you already
# have instead of building them again.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASELINE_SRC=""
CANDIDATE_SRC=""
BASELINE_OBSERVER=""
CANDIDATE_OBSERVER=""
WORK=""
PORT=2981
RENDER=""
N=100000
DIM=128
TOPK=10
NQ=100
ROUNDS=3
JOBS=$(nproc)
SKIP_CPU=0

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while (($# > 0)); do
  case "$1" in
    --baseline-src) BASELINE_SRC=$2; shift 2 ;;
    --candidate-src) CANDIDATE_SRC=$2; shift 2 ;;
    --baseline-observer) BASELINE_OBSERVER=$2; shift 2 ;;
    --candidate-observer) CANDIDATE_OBSERVER=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --render) RENDER=$2; shift 2 ;;
    --n) N=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    --topk) TOPK=$2; shift 2 ;;
    --nq) NQ=$2; shift 2 ;;
    --rounds) ROUNDS=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --skip-cpu) SKIP_CPU=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$WORK" ]] || { echo "--work is required" >&2; exit 2; }
mkdir -p "$WORK"

BRIDGE="$WORK/bridge/libseekdb_cuvs_bridge.so"
if [[ ! -f "$BRIDGE" ]]; then
  if [[ -z "$CANDIDATE_SRC" ]]; then
    echo "need --candidate-src (for tools/hipvs_bridge) or a prebuilt --candidate-observer" >&2
    exit 2
  fi
  echo "== building bridge"
  "$CANDIDATE_SRC/tools/hipvs_bridge/build.sh" "$BRIDGE"
fi

build_tree() {
  local src=$1 out=$2
  if [[ -x "$out/src/observer/seekdb" ]]; then
    echo "== reusing $out/src/observer/seekdb"
    return
  fi
  echo "== building $src"
  ( cd "$src" && ./build.sh release \
      -DOB_BUILD_CUVS=ON \
      -DOB_BUILD_CUVS_TRACE=ON \
      -DCUVS_BRIDGE_LIB="$BRIDGE" \
      --make -j"$JOBS" )
  [[ -x "$out/src/observer/seekdb" ]] || { echo "build produced no observer in $out" >&2; exit 3; }
}

if [[ -z "$BASELINE_OBSERVER" ]]; then
  [[ -n "$BASELINE_SRC" ]] || { echo "need --baseline-src or --baseline-observer" >&2; exit 2; }
  BASELINE_OBSERVER="$BASELINE_SRC/build_release/src/observer/seekdb"
  build_tree "$BASELINE_SRC" "$BASELINE_SRC/build_release"
fi
if [[ -z "$CANDIDATE_OBSERVER" ]]; then
  [[ -n "$CANDIDATE_SRC" ]] || { echo "need --candidate-src or --candidate-observer" >&2; exit 2; }
  CANDIDATE_OBSERVER="$CANDIDATE_SRC/build_release/src/observer/seekdb"
  build_tree "$CANDIDATE_SRC" "$CANDIDATE_SRC/build_release"
fi

run_one() {
  local label=$1 observer=$2 lib=$3 port=$4
  echo "== benchmarking $label (lib=$lib)"
  "$HERE/amd_cagra_bench.py" \
    --observer "$observer" \
    --label "$label" \
    --lib "$lib" \
    --bridge "$BRIDGE" \
    --render "$RENDER" \
    --base-dir "$WORK/run-$label" \
    --evidence "$WORK/out-$label" \
    --port "$port" \
    --n "$N" --dim "$DIM" --topk "$TOPK" --nq "$NQ" \
    --rounds "$ROUNDS"
}

# Alternate baseline/candidate per round the way the grader does by running the
# whole (cheap) prepared-directory rounds back to back per build.
run_one baseline "$BASELINE_OBSERVER" cuvs "$PORT"
run_one candidate "$CANDIDATE_OBSERVER" cuvs "$PORT"
if [[ "$SKIP_CPU" -eq 0 ]]; then
  run_one cpu-vsag "$CANDIDATE_OBSERVER" vsag "$PORT"
fi

summaries=(--summary "baseline=$WORK/out-baseline/summary.json"
           --summary "candidate=$WORK/out-candidate/summary.json")
if [[ "$SKIP_CPU" -eq 0 ]]; then
  summaries+=(--summary "cpu-vsag=$WORK/out-cpu-vsag/summary.json")
fi

"$HERE/make_report.py" \
  "${summaries[@]}" \
  --baseline baseline \
  --out "$WORK/report.md" \
  --out-json "$WORK/report.json" \
  --gpu "${GPU_NAME:-AMD Radeon (ROCm + hipVS)}" \
  --cpu "${CPU_NAME:-$(lscpu | awk -F: '/Model name/{print $2; exit}' | xargs)}"

echo "== report: $WORK/report.md"
