#!/usr/bin/env bash
# Build a release-style iotop-perf binary for Rocky Linux 8 (el8/x86_64).
# Intended for GitHub Actions and local parity with CI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

PRODUCT="$(grep -E '^#define PRODUCT_NAME ' src/iotop.h | sed -E 's/.*"([^"]+)".*/\1/')"
VERSION="$(grep -E '^#define VERSION ' src/iotop.h | sed -E 's/.*"([^"]+)".*/\1/')"
OUT_DIR="${OUT_DIR:-dist}"
NAME="${PRODUCT}-${VERSION}-rocky8.x86_64"

mkdir -p "$OUT_DIR"
make clean
make -j"$(nproc)" NO_FLTO=1 CFLAGS="-O3 -g -fno-omit-frame-pointer" V=1
strip --strip-unneeded "${PRODUCT}" 2>/dev/null || strip "${PRODUCT}"

cp -a "${PRODUCT}" "${OUT_DIR}/${NAME}"
cp -a "${PRODUCT}" "${OUT_DIR}/${PRODUCT}"

(
  cd "$OUT_DIR"
  sha256sum "${NAME}" "${PRODUCT}" > SHA256SUMS
)

{
  echo "product=${PRODUCT}"
  echo "name=${NAME}"
  echo "version=${VERSION}"
  echo "target=rocky8.x86_64"
  echo "glibc=el8"
  echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname=$(uname -a)"
  if command -v rpm >/dev/null 2>&1; then
    echo "ncurses=$(rpm -q ncurses-libs 2>/dev/null || true)"
  fi
  echo "ldd:"
  ldd "${PRODUCT}" 2>/dev/null | sed 's/^/  /' || true
} | tee "${OUT_DIR}/BUILDINFO.txt"

echo
echo "Built ${OUT_DIR}/${NAME}"
"./${PRODUCT}" -v || true
file "${OUT_DIR}/${NAME}" 2>/dev/null || true
