#!/usr/bin/env bash
# Performance harness for iotop Tier 1/2 improvements.
#
# Usage:
#   ./scripts/perf/bench.sh              # default scenarios
#   ./scripts/perf/bench.sh --binary ./iotop
#   IOTOP=./iotop ./scripts/perf/bench.sh
#
# Emits CSV lines to stdout and a human summary. Each scenario is labeled so
# you can compare before/after commits.
#
# Scenarios map to improvement tasks:
#   S1_default       process-only + TGID (P13/P14) + freelist + merge (P1-P12)
#   S2_threads       -T full thread walk (baseline-like cost)
#   S3_fast_sample   -s 50  high rate intermediate samples (P9)
#   S4_topN          -N 20  print/sort top only (P18)
#   S5_only          -o     active-only print filter
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${IOTOP:-${ROOT}/iotop}"
ITERS="${ITERS:-5}"
DELAY="${DELAY:-1}"
SAMPLE="${SAMPLE:-100}"
OUT_DIR="${PERF_OUT:-${ROOT}/scripts/perf/results}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

mkdir -p "$OUT_DIR"
CSV="${OUT_DIR}/bench_${STAMP}.csv"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not found/executable: $BIN" >&2
  echo "hint: make NO_FLTO=1 CFLAGS='-O3 -g'  or docker compose run iotop build" >&2
  exit 1
fi

need_root() {
  if [[ "$(id -u)" -ne 0 ]] && ! capsh --print 2>/dev/null | grep -q cap_net_admin; then
    # docker privileged path usually is root inside container
    if [[ ! -w /proc/1/ns/pid ]]; then
      :
    fi
  fi
}

run_scenario() {
  local name="$1"
  shift
  local tmp
  tmp="$(mktemp)"
  local t0 t1 wall_ms
  t0=$(date +%s%3N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1000))')
  # shellcheck disable=SC2086
  set +e
  "$BIN" -b -q -n "$ITERS" -d "$DELAY" -s "$SAMPLE" -f "$@" >"$tmp.out" 2>"$tmp.err"
  local rc=$?
  set -e
  t1=$(date +%s%3N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1000))')
  wall_ms=$((t1 - t0))

  # Parse PERF lines and Samples lines
  local fetch_sum=0 diff_sum=0 print_sum=0 nl_sum=0 proc_sum=0 n_perf=0
  local sample_time_sum=0 n_samples=0 arr_last=0

  while IFS= read -r line; do
    if [[ "$line" == PERF,* ]]; then
      # PERF,fetch_ms=..,diff_ms=..,print_ms=..,n_netlink=..,n_proc=..,arr=..
      local f d p nl pr ar
      f=$(echo "$line" | sed -n 's/.*fetch_ms=\([0-9]*\).*/\1/p')
      d=$(echo "$line" | sed -n 's/.*diff_ms=\([0-9]*\).*/\1/p')
      p=$(echo "$line" | sed -n 's/.*print_ms=\([0-9]*\).*/\1/p')
      nl=$(echo "$line" | sed -n 's/.*n_netlink=\([0-9]*\).*/\1/p')
      pr=$(echo "$line" | sed -n 's/.*n_proc=\([0-9]*\).*/\1/p')
      ar=$(echo "$line" | sed -n 's/.*arr=\([0-9]*\).*/\1/p')
      fetch_sum=$((fetch_sum + ${f:-0}))
      diff_sum=$((diff_sum + ${d:-0}))
      print_sum=$((print_sum + ${p:-0}))
      nl_sum=$((nl_sum + ${nl:-0}))
      proc_sum=$((proc_sum + ${pr:-0}))
      arr_last=${ar:-$arr_last}
      n_perf=$((n_perf + 1))
    fi
  done <"$tmp.err"

  while IFS= read -r line; do
    if [[ "$line" == Samples=* ]]; then
      local tt
      tt=$(echo "$line" | sed -n 's/.*Time Taken: \([0-9.]*\) sec.*/\1/p')
      if [[ -n "$tt" ]]; then
        sample_time_sum=$(python3 -c "print($sample_time_sum + float('$tt'))")
        n_samples=$((n_samples + 1))
      fi
    fi
  done <"$tmp.out"

  local avg_fetch=0 avg_diff=0 avg_print=0 avg_nl=0 avg_proc=0 avg_sample=0
  if [[ "$n_perf" -gt 0 ]]; then
    avg_fetch=$((fetch_sum / n_perf))
    avg_diff=$((diff_sum / n_perf))
    avg_print=$((print_sum / n_perf))
    avg_nl=$((nl_sum / n_perf))
    avg_proc=$((proc_sum / n_perf))
  fi
  if [[ "$n_samples" -gt 0 ]]; then
    avg_sample=$(python3 -c "print(round($sample_time_sum / $n_samples, 3))")
  fi

  echo "${name},rc=${rc},wall_ms=${wall_ms},avg_fetch_ms=${avg_fetch},avg_diff_ms=${avg_diff},avg_print_ms=${avg_print},avg_n_netlink=${avg_nl},avg_n_proc=${avg_proc},avg_sample_sec=${avg_sample},arr=${arr_last},args=$*"

  # keep artifacts
  cp "$tmp.out" "${OUT_DIR}/${name}_${STAMP}.out" 2>/dev/null || true
  cp "$tmp.err" "${OUT_DIR}/${name}_${STAMP}.err" 2>/dev/null || true
  rm -f "$tmp.out" "$tmp.err"
}

echo "scenario,metrics" | tee "$CSV"
echo "# binary=$BIN iters=$ITERS delay=$DELAY sample_ms=$SAMPLE" | tee -a "$CSV"

{
  run_scenario S1_default
  run_scenario S2_threads -T
  run_scenario S3_fast_sample -s 50
  run_scenario S4_topN -N 20
  run_scenario S5_only -o
  run_scenario S6_threads_fast -T -s 50
} | tee -a "$CSV"

echo
echo "Results written to $CSV"
echo
echo "How to compare improvements:"
echo "  1) Run this script on commit A, save CSV."
echo "  2) Run on commit B."
echo "  3) Diff avg_fetch_ms and avg_n_netlink (S1 vs S2 shows P13/P14 win)."
echo "  Key signals:"
echo "    avg_n_netlink  ~ number of taskstats round-trips per printed sample window"
echo "    avg_fetch_ms   ~ time in process walk + netlink"
echo "    avg_diff_ms    ~ create_quick_diff (merge-walk)"
echo "    avg_print_ms   ~ sort + format + fwrite"
echo "    S2_threads / S1_default ratio should be >>1 on multi-threaded hosts"
