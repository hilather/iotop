# Rocky Linux 8 test environment for iotop

Reproduces the Rocky 8 toolchain/runtime where occasional segfaults were seen
after the performance-oriented field-stripping changes (commit `53d5641`).

## Segfault root cause (found + fixed)

Primary crash: `GetTimeAndDate()` returned `NULL` whenever the accumulated
time was not divisible by 1000; `view_batch()` then did `ptm->tm_hour` → SIGSEGV.
taskstats CPU times almost never land on multiples of 1000, so it looked
"occasional" (only when printing rows with non-zero utime/stime).

Also fixed: thread stats leak / wrong `*tp` cache in `pid_cb`, and a debug
`if (1 || !a)` that disabled sorting (plus NULL-safe string compares for
stripped `pw_name`/`cmdline*`).

## Phase A: versioned taskstats (from Tomas-M 1.29+)

Vendored `src/taskstats-v14.h` and `src/taskstats-v15.h`. Field access no longer
depends on the build-host kernel headers. Layout rules:

- version **15** → v15 layout
- version **4..14** and **16+** → v14 layout (Tomas-M policy)
- all metrics this fork still uses are mapped in both layouts
- payloads are `memcpy`'d into aligned locals (ASan-safe)
- exit-time warnings for too-old (`<4`) or newer-than-known (`>15`) kernels

## Phase B: fetch-path hardening

- `is_a_file` / `is_a_dir` / `is_a_process` (prefer `/proc/<tid>/stat`)
- skip invisible/exited PIDs before netlink (`make_stats` + `pidgen_cb`)
- `/proc` mount check via `is_a_process(getpid())`; `/proc/self/io` via `is_a_file`
- read-only `task_delayacct` probe + startup warn (never writes the sysctl)
- signed-char-safe `esc_low_ascii1`

## Phase C: batch polish

- `fflush(stdout)` after every printed sample block (pipes/log shippers)
- NULL-safe batch loop / `create_quick_diff` / `copy_old_processes` / `arr_find`
- `humanize_val` avoids per-loop `strlen`
- safer printf types for majflt; clamp sort print length

Retest:

```bash
./scripts/retest-rocky8.sh
# or:
sg docker -c 'docker compose -f docker/docker-compose.yml run --rm iotop asan'
```

## One-time host setup

```bash
# If `docker version` fails with permission denied:
./scripts/fix-docker-group.sh
newgrp docker
docker version
```

## Build the image

```bash
docker compose -f docker/docker-compose.yml build
```

## Common commands

```bash
# Toolchain / OS check
docker compose -f docker/docker-compose.yml run --rm iotop doctor

# Debug build
docker compose -f docker/docker-compose.yml run --rm iotop build

# Batch smoke test (5 iterations)
docker compose -f docker/docker-compose.yml run --rm iotop run

# Stress loop (100 rounds) to catch intermittent crashes
docker compose -f docker/docker-compose.yml run --rm iotop stress 100

# AddressSanitizer
docker compose -f docker/docker-compose.yml run --rm iotop asan

# Interactive gdb
docker compose -f docker/docker-compose.yml run --rm iotop gdb

# Shell inside Rocky 8
docker compose -f docker/docker-compose.yml run --rm iotop shell
```

The container runs **privileged** with **host PID** so `/proc` and netlink
taskstats behave like a real system (required for iotop).

Source is bind-mounted from the repo root; builds write `iotop` and `bld/` on the host.
