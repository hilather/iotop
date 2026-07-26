#!/usr/bin/env bash
# Build Rocky 8 image and run doctor + debug build + ASan smoke test.
set -euo pipefail
cd "$(dirname "$0")/.."

DOCKER=(./scripts/docker.sh)

echo "==> Docker access check"
"${DOCKER[@]}" version

echo "==> Build Rocky Linux 8 image"
"${DOCKER[@]}" compose -f docker/docker-compose.yml build

echo "==> Doctor"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop doctor

echo "==> Debug build"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop build

echo "==> ASan run (10 iters)"
"${DOCKER[@]}" compose -f docker/docker-compose.yml run --rm iotop asan

echo
echo "All done. For stress testing:"
echo "  ./scripts/docker.sh compose -f docker/docker-compose.yml run --rm iotop stress 100"
