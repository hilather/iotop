#!/usr/bin/env bash
# Rebuild and re-test iotop inside Rocky Linux 8 after the segfault fixes.
set -euo pipefail
cd "$(dirname "$0")/.."

DOCKER=(./scripts/docker.sh)

echo "==> Host-side GetTimeAndDate reproducer"
gcc -O0 -g -o /tmp/test-gettime scripts/test-gettime-crash.c
/tmp/test-gettime

echo
echo "==> Fix ownership of previous root-owned build artifacts (if any)"
if [[ -e iotop ]] && [[ "$(stat -c %U iotop 2>/dev/null || true)" == "root" ]]; then
  sudo chown -R "$USER:$USER" iotop bld 2>/dev/null || true
fi

echo "==> Rebuild image (picks up source bind-mount; rebuild only if Dockerfile changed)"
"${DOCKER[@]}" compose -f docker/docker-compose.yml build

echo "==> Clean + debug build in Rocky 8"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop build

echo "==> Batch smoke (10 iters)"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop run 10 1 100

echo "==> ASan (10 iters)"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop asan

echo "==> Stress (50 rounds)"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop stress 50

echo
echo "All retests completed successfully."
