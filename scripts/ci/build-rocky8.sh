#!/usr/bin/env bash
# Build a release-style iotop binary for Rocky Linux 8 (el8/x86_64).
# Intended for GitHub Actions and local parity with CI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

VERSION="$(grep -E '^#define VERSION ' src/iotop.h | sed -E 's/.*"([^"]+)".*/\1/')"
OUT_DIR="${OUT_DIR:-dist}"
NAME="iotop-${VERSION}-rocky8.x86_64"

mkdir -p "$OUT_DIR"
make clean
# Match el8-friendly flags used in our Docker debug env for releases:
# NO_FLTO for broader toolchain stability; -O3 for production speed.
make -j"$(nproc)" NO_FLTO=1 CFLAGS="-O3 -g -fno-omit-frame-pointer" V=1
strip --strip-unneeded iotop 2>/dev/null || strip iotop

cp -a iotop "${OUT_DIR}/${NAME}"
# Also ship a plain name for convenience
cp -a iotop "${OUT_DIR}/iotop"

# Checksums
(
  cd "$OUT_DIR"
  sha256sum "${NAME}" iotop > SHA256SUMS
)

# Metadata
{
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
  ldd iotop 2>/dev/null | sed 's/^/  /' || true
} | tee "${OUT_DIR}/BUILDINFO.txt"

echo
echo "Built ${OUT_DIR}/${NAME}"
./iotop -v || true
file "${OUT_DIR}/${NAME}" 2>/dev/null || true
