# Releasing iotop-perf (Rocky Linux 8 prebuilds)

CI is free on this public GitHub repository.

The product binary is **`iotop-perf`** (not distro `iotop`).

## Version source of truth

```c
// src/iotop.h
#define PRODUCT_NAME "iotop-perf"
#define VERSION "X.Y.Z"
#define VERSION_EXTRA "hilather/perf"
```

Git tags **must** match: `vX.Y.Z` ↔ `VERSION "X.Y.Z"`.

`-v` prints: `iotop-perf X.Y.Z (hilather/perf)`

## Cut a release

```bash
# 1) Bump VERSION in src/iotop.h if needed
# 2) Commit on master
git add src/iotop.h
git commit -m "Release X.Y.Z"
git push origin master

# 3) Tag and push (triggers Release workflow)
git tag -a "vX.Y.Z" -m "iotop-perf X.Y.Z"
git push origin "vX.Y.Z"
```

GitHub Actions will:

1. Build inside `rockylinux:8`
2. Produce `iotop-perf-X.Y.Z-rocky8.x86_64` (+ `.tar.gz`, `SHA256SUMS`, `BUILDINFO.txt`)
3. Create a GitHub Release attached to the tag

## Manual rebuild of an existing tag

Actions → **Release** → **Run workflow** → enter tag (e.g. `v1.18.0`).

## Local parity with CI

```bash
docker run --rm -v "$PWD:/src" -w /src rockylinux:8 \
  bash -lc 'dnf -y install gcc make pkgconfig ncurses-devel binutils file which && ./scripts/ci/build-rocky8.sh'
ls -la dist/
./dist/iotop-perf -v
```

## Runtime requirements (target Rocky 8 host)

```bash
sudo dnf install -y ncurses-libs
sudo install -m 0755 iotop-perf-*-rocky8.x86_64 /usr/local/sbin/iotop-perf
sudo iotop-perf -b -n 1 -d 1
```

Needs root or `cap_net_admin`.
