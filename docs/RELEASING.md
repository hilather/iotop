# Releasing iotop (Rocky Linux 8 prebuilds)

CI is free on this public GitHub repository.

## Version source of truth

```c
// src/iotop.h
#define VERSION "X.Y.Z"
```

Git tags **must** match: `vX.Y.Z` ↔ `VERSION "X.Y.Z"`.

## Cut a release

```bash
# 1) Bump version in src/iotop.h if needed
# 2) Commit on master
git add src/iotop.h
git commit -m "Release X.Y.Z"
git push origin master

# 3) Tag and push (triggers Release workflow)
git tag -a "vX.Y.Z" -m "iotop X.Y.Z"
git push origin "vX.Y.Z"
```

GitHub Actions will:

1. Build inside `rockylinux:8`
2. Produce `iotop-X.Y.Z-rocky8.x86_64` (+ `.tar.gz`, `SHA256SUMS`, `BUILDINFO.txt`)
3. Create a GitHub Release attached to the tag

## Manual rebuild of an existing tag

Actions → **Release** → **Run workflow** → enter tag (e.g. `v1.18.0`).

## Local parity with CI

```bash
docker run --rm -v "$PWD:/src" -w /src rockylinux:8 \
  bash -lc 'dnf -y install gcc make pkgconfig ncurses-devel binutils file which && ./scripts/ci/build-rocky8.sh'
ls -la dist/
```

## Runtime requirements (target Rocky 8 host)

```bash
sudo dnf install -y ncurses-libs
sudo install -m 0755 iotop-*-rocky8.x86_64 /usr/local/sbin/iotop
sudo iotop -b -n 1 -d 1
```

Needs root or `cap_net_admin`.
