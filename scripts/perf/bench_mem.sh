#!/usr/bin/env bash
# Cost of low-cost metrics (always on) vs optional -D dirty (smaps_rollup).
#
# Scenarios:
#   M0_plain_E     -E              no enrich, no dirty (fastest print)
#   M1_default                     enrich + status RSS/swap/state + taskstats extras
#   M2_dirty       -D              + Private_Dirty via smaps_rollup
#   M3_dirty_topN  -D -N 20        dirty only on top 20 rows
#   M4_dirty_c     -D -c           dirty + full cmdline
#   M5_threads_D   -T -D           expensive fetch + dirty
#
# Usage:
#   ./scripts/perf/bench_mem.sh
#   LABEL=run1 ITERS=4 ./scripts/perf/bench_mem.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${IOTOP:-${ROOT}/iotop-perf}"
ITERS="${ITERS:-4}"
DELAY="${DELAY:-1}"
SAMPLE="${SAMPLE:-100}"
LABEL="${LABEL:-mem}"
OUT_DIR="${PERF_OUT:-${ROOT}/scripts/perf/results/mem_${LABEL}}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

mkdir -p "$OUT_DIR"
CSV="${OUT_DIR}/mem_${LABEL}_${STAMP}.csv"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not executable: $BIN" >&2
  exit 1
fi

parse_perf() {
  local name="$1" rc="$2" wall_ms="$3" err="$4" args_s="$5"
  python3 - "$name" "$rc" "$wall_ms" "$err" "$args_s" <<'PY'
import sys, re
name, rc, wall_ms, errp, args_s = sys.argv[1:6]
fetch=diff=prt=nl=proc=pw=cmd=io=st=sm=arr=n=0
dirty=0
for line in open(errp, errors="replace"):
    if not line.startswith("PERF,"):
        continue
    def g(k, default=0):
        m = re.search(rf"{k}=([0-9]+)", line)
        return int(m.group(1)) if m else default
    fetch += g("fetch_ms"); diff += g("diff_ms"); prt += g("print_ms")
    nl += g("n_netlink"); proc += g("n_proc")
    pw += g("n_getpwuid"); cmd += g("n_cmdline"); io += g("n_ioprio")
    st += g("n_status"); sm += g("n_smaps")
    arr = g("arr", arr); dirty = g("dirty", dirty); n += 1
avg = lambda s: (round(s / n, 2) if n else 0)
print(
    f"{name},rc={rc},wall_ms={wall_ms},n_perf={n},"
    f"avg_fetch_ms={avg(fetch)},avg_diff_ms={avg(diff)},avg_print_ms={avg(prt)},"
    f"avg_n_netlink={avg(nl)},avg_n_status={avg(st)},avg_n_smaps={avg(sm)},"
    f"avg_n_ioprio={avg(io)},avg_n_cmdline={avg(cmd)},"
    f"sum_status={st},sum_smaps={sm},arr={arr},dirty={dirty},args={args_s}"
)
PY
}

run_one() {
  local name="$1"
  shift
  local out err
  out="$(mktemp)"; err="$(mktemp)"
  local t0 t1 wall_ms
  t0=$(date +%s%3N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1000))')
  set +e
  "$BIN" -b -q -n "$ITERS" -d "$DELAY" -s "$SAMPLE" -f "$@" >"$out" 2>"$err"
  local rc=$?
  set -e
  t1=$(date +%s%3N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1000))')
  wall_ms=$((t1 - t0))
  parse_perf "$name" "$rc" "$wall_ms" "$err" "$*"
  cp "$out" "${OUT_DIR}/${name}.out" 2>/dev/null || true
  cp "$err" "${OUT_DIR}/${name}.err" 2>/dev/null || true
  rm -f "$out" "$err"
}

echo "scenario,metrics label=${LABEL}" | tee "$CSV"
echo "# binary=$BIN iters=$ITERS delay=$DELAY sample=$SAMPLE" | tee -a "$CSV"
echo "# have_smaps_rollup: check with ls /proc/self/smaps_rollup" | tee -a "$CSV"

{
  run_one M0_plain_E -E
  run_one M1_default
  run_one M2_dirty -D
  run_one M3_dirty_topN -D -N 20
  run_one M4_dirty_c -D -c
  run_one M5_threads_D -T -D
} | tee -a "$CSV"

echo
echo "Wrote $CSV"
echo
echo "Interpretation:"
echo "  M1 - M0 print_ms     => cost of enrich + status/stat (RSS/SWAP/state) + extra cols"
echo "  M2 - M1 print_ms     => cost of -D smaps_rollup Private_Dirty on all printed rows"
echo "  M3 - M2              => dirty limited to top-N (should cut n_smaps dramatically)"
echo "  avg_fetch_ms flat    => mem/dirty not on sample path"
echo "  M5 fetch high        => -T thread walk, independent of -D"
