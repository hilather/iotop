#!/usr/bin/env bash
# Rocky 8 container entrypoint for building and stress-testing iotop.
set -euo pipefail

ROOT=/src
cd "$ROOT"

export CFLAGS="${CFLAGS:--O0 -g -fno-omit-frame-pointer}"
export NO_FLTO="${NO_FLTO:-1}"

build_debug() {
  echo "==> Building debug iotop (NO_FLTO=1, -O0 -g)"
  make clean >/dev/null 2>&1 || true
  make V=1 NO_FLTO=1 CFLAGS="-O0 -g -fno-omit-frame-pointer -Wall -Wextra"
  echo "==> Built: $(pwd)/iotop-perf"
  file ./iotop-perf || true
}

build_asan() {
  echo "==> Building ASan/UBSan iotop"
  make clean >/dev/null 2>&1 || true
  make V=1 NO_FLTO=1 \
    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all" \
    LDFLAGS="-fsanitize=address,undefined"
  echo "==> Built ASan binary: $(pwd)/iotop-perf"
}

run_batch() {
  local iters="${1:-5}"
  local delay="${2:-1}"
  local sample="${3:-200}"
  echo "==> Running batch mode: -b -n $iters -d $delay -s $sample"
  # CAP_NET_ADMIN / privileged needed for taskstats netlink on host proc.
  ./iotop-perf -b -n "$iters" -d "$delay" -s "$sample" -t || true
}

run_stress() {
  local rounds="${1:-50}"
  local sample="${2:-100}"
  echo "==> Stress loop: $rounds quick batch iterations (sample ${sample}ms)"
  local i
  for i in $(seq 1 "$rounds"); do
    echo "--- stress round $i/$rounds ---"
    # Short delay, fast sample rate, few prints — churns process list hard.
    if ! ./iotop-perf -b -n 2 -d 1 -s "$sample" -q; then
      echo "iotop exited non-zero (or crashed) on round $i" >&2
      return 1
    fi
  done
  echo "==> Stress completed without process-level failure"
}

run_gdb() {
  echo "==> Starting gdb on iotop (batch mode, 20 iters)"
  gdb -q -ex 'set pagination off' \
      -ex 'run -b -n 20 -d 1 -s 100 -t' \
      -ex 'bt full' \
      -ex 'info registers' \
      --args ./iotop-perf
}

run_valgrind() {
  echo "==> Valgrind memcheck (batch, 5 iters) — slow"
  valgrind --error-exitcode=99 --leak-check=full --track-origins=yes \
    ./iotop-perf -b -n 5 -d 1 -s 200 -t
}

run_asan() {
  build_asan
  echo "==> Running ASan binary"
  ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ./iotop-perf -b -n 10 -d 1 -s 100 -t
}

cmd_help() {
  cat <<'EOF'
iotop Rocky Linux 8 test container

Usage:
  docker compose -f docker/docker-compose.yml run --rm iotop <command>

Commands:
  help              Show this help
  shell             Interactive bash
  build             Debug build (-O0 -g)
  build-asan        AddressSanitizer + UBSan build
  run [n d s]       Batch run (default: 5 iters, 1s delay, 200ms sample)
  stress [rounds]   Loop batch runs to provoke occasional crashes
  gdb               Build debug (if needed) and run under gdb
  valgrind          Build debug and run under valgrind
  asan              Build+run with ASan/UBSan
  doctor            Show OS/kernel/tool versions

Examples:
  ... run --rm iotop build
  ... run --rm iotop run 10 1 100
  ... run --rm iotop stress 100
  ... run --rm iotop asan
  ... run --rm iotop gdb
EOF
}

cmd_doctor() {
  echo "=== OS ==="
  cat /etc/os-release || true
  echo "=== Kernel (host, via container) ==="
  uname -a
  echo "=== Toolchain ==="
  gcc --version | head -1
  make --version | head -1
  gdb --version | head -1
  valgrind --version || true
  echo "=== taskstats (build-host header + vendored layouts) ==="
  ls -la /usr/include/linux/taskstats.h 2>/dev/null || echo "no system taskstats.h"
  ls -la src/taskstats-v14.h src/taskstats-v15.h 2>/dev/null || true
  grep -E 'IOTOP_TASKSTATS|TASKSTATS_VERSION' src/iotop.h /usr/include/linux/taskstats.h 2>/dev/null | head -20 || true
  echo "=== /proc sample ==="
  ls /proc | head -5
}

main() {
  local cmd="${1:-help}"
  shift || true
  case "$cmd" in
    help|-h|--help) cmd_help ;;
    shell|bash) exec /bin/bash ;;
    doctor) cmd_doctor ;;
    build) build_debug ;;
    build-asan) build_asan ;;
    run)
      [[ -x ./iotop-perf ]] || build_debug
      run_batch "${1:-5}" "${2:-1}" "${3:-200}"
      ;;
    stress)
      [[ -x ./iotop-perf ]] || build_debug
      run_stress "${1:-50}" "${2:-100}"
      ;;
    gdb)
      [[ -x ./iotop-perf ]] || build_debug
      run_gdb
      ;;
    valgrind)
      [[ -x ./iotop-perf ]] || build_debug
      run_valgrind
      ;;
    asan) run_asan ;;
    *)
      echo "Unknown command: $cmd" >&2
      cmd_help
      exit 1
      ;;
  esac
}

main "$@"
