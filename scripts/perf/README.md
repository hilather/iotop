# iotop performance tests

Harness for evaluating Tier 1 & 2 sampling improvements.

## Build

```bash
# Host (needs ncurses + root/CAP_NET_ADMIN to run):
make clean && make NO_FLTO=1 CFLAGS='-O3 -g'

# Rocky 8 container:
docker compose -f docker/docker-compose.yml run --rm iotop build
# binary appears as ./iotop on the bind mount
```

## Run full suite

```bash
# Inside Rocky 8 container (privileged + host PID):
docker compose -f docker/docker-compose.yml run --rm iotop shell
# then:
./scripts/perf/bench.sh

# Or one-shot:
docker compose -f docker/docker-compose.yml run --rm iotop \
  bash -lc './scripts/perf/bench.sh'
```

Environment overrides:

| Var | Default | Meaning |
|-----|---------|---------|
| `IOTOP` | `./iotop` | Binary path |
| `ITERS` | `5` | `-n` print iterations |
| `DELAY` | `1` | `-d` seconds |
| `SAMPLE` | `100` | `-s` ms |
| `PERF_OUT` | `scripts/perf/results` | Output directory |

## Scenarios → tasks

| Scenario | Flags | What it stresses |
|----------|-------|------------------|
| **S1_default** | (none) | Process-only + TGID (**P13/P14**), freelist (**P8**), merge-walk (**P7**), fixed `comm` (**P1**), batch print buffer (**P20**) |
| **S2_threads** | `-T` | Full `/proc/*/task` walk + per-tid fold (pre-P13 cost) |
| **S3_fast_sample** | `-s 50` | Intermediate samples without exited copy (**P9**) |
| **S4_topN** | `-N 20` | Truncated print path (**P18**) |
| **S5_only** | `-o` | Active-row filter at print |
| **S6_threads_fast** | `-T -s 50` | Worst case: threads + high sample rate |

Each print window with `-f` emits:

```
PERF,fetch_ms=…,diff_ms=…,print_ms=…,n_netlink=…,n_proc=…,arr=…
```

## Interpreting results

- **S2.avg_n_netlink / S1.avg_n_netlink** ≈ thread amplification (expect 2–10×+).
- **avg_fetch_ms** should drop most from P13/P14 + P1/P8.
- **avg_diff_ms** should drop from P7 (merge) vs old `arr_find` per row.
- **avg_print_ms** should drop from P3/P10/P11/P18/P20.

## Quick single-scenario

```bash
./iotop -b -q -n 3 -d 1 -s 100 -f 2>perf.err | head
grep ^PERF perf.err
```

## A/B method

```bash
git checkout <before>
make clean && make NO_FLTO=1 CFLAGS='-O3 -g'
ITERS=8 ./scripts/perf/bench.sh
# save scripts/perf/results/bench_*.csv

git checkout <after>
make clean && make NO_FLTO=1 CFLAGS='-O3 -g'
ITERS=8 ./scripts/perf/bench.sh
# compare avg_fetch_ms / avg_n_netlink for S1 and S2
```
