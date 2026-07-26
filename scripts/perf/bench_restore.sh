#!/usr/bin/env bash
# Before/after cost of Phase R1 print-time restores (USER / PRIO / cmdline).
#
# Scenarios:
#   R0_plain          -E           no enrich (ac_comm only)
#   R1_enrich                      USER+PRIO at print (default restore)
#   R2_cmdline        -c           + lazy full cmdline
#   R3_topN           -N 20        enrich limited to top 20 rows
#   R4_enrich_threads -T           thread walk + enrich
#   R5_plain_threads  -E -T        thread walk, no enrich
#
# Usage:
#   LABEL=before ./scripts/perf/bench_restore.sh
#   LABEL=after  ./scripts/perf/bench_restore.sh
#   ./scripts/perf/compare_restore.py results/restore_before/*.csv results/restore_after/*.csv
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${IOTOP:-${ROOT}/iotop-perf}"
ITERS="${ITERS:-4}"
DELAY="${DELAY:-1}"
SAMPLE="${SAMPLE:-100}"
LABEL="${LABEL:-run}"
OUT_DIR="${PERF_OUT:-${ROOT}/scripts/perf/results/restore_${LABEL}}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

mkdir -p "$OUT_DIR"
CSV="${OUT_DIR}/restore_${LABEL}_${STAMP}.csv"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not executable: $BIN" >&2
  exit 1
fi

parse_perf() {
  local name="$1" rc="$2" wall_ms="$3" err="$4" args_s="$5"
  python3 - "$name" "$rc" "$wall_ms" "$err" "$args_s" <<'PY'
import sys, re
name, rc, wall_ms, errp, args_s = sys.argv[1:6]
fetch=diff=prt=nl=proc=pw=cmd=io=arr=n=0
for line in open(errp, errors="replace"):
    if not line.startswith("PERF,"):
        continue
    def g(k, default=0):
        m = re.search(rf"{k}=([0-9]+)", line)
        return int(m.group(1)) if m else default
    fetch += g("fetch_ms"); diff += g("diff_ms"); prt += g("print_ms")
    nl += g("n_netlink"); proc += g("n_proc")
    pw += g("n_getpwuid"); cmd += g("n_cmdline"); io += g("n_ioprio")
    arr = g("arr", arr); n += 1
avg = lambda s: (round(s / n, 2) if n else 0)
print(
    f"{name},rc={rc},wall_ms={wall_ms},n_perf={n},"
    f"avg_fetch_ms={avg(fetch)},avg_diff_ms={avg(diff)},avg_print_ms={avg(prt)},"
    f"avg_n_netlink={avg(nl)},avg_n_proc={avg(proc)},"
    f"avg_n_getpwuid={avg(pw)},avg_n_cmdline={avg(cmd)},avg_n_ioprio={avg(io)},"
    f"sum_getpwuid={pw},sum_cmdline={cmd},sum_ioprio={io},"
    f"arr={arr},args={args_s}"
)
PY
}

run_one() {
  local name="$1"
  shift
  local out err
  out="$(mktemp)"
  err="$(mktemp)"
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

{
  run_one R0_plain -E
  run_one R1_enrich
  run_one R2_cmdline -c
  run_one R3_topN -N 20
  run_one R4_enrich_threads -T
  run_one R5_plain_threads -E -T
} | tee -a "$CSV"

echo
echo "Wrote $CSV"
echo "Compare:"
echo "  avg_print_ms  R1 vs R0  = cost of USER+PRIO restore"
echo "  avg_print_ms  R2 vs R1  = cost of lazy cmdline (-c)"
echo "  avg_fetch_ms  should stay flat R0/R1/R2 (enrich not on fetch path)"
echo "  avg_n_ioprio  R3_topN should be ~20 * n_perf (only printed rows)"
