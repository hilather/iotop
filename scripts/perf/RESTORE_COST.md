# Batch print-time restore cost (Phase R1)

Measured in Rocky 8 container (`ITERS=4`, `-d 1`, `-s 100`).

## Same-binary cost matrix

| Scenario | Flags | avg_fetch_ms | avg_print_ms | avg_n_ioprio | avg_n_cmdline | notes |
|----------|-------|--------------|--------------|--------------|---------------|-------|
| **R0_plain** | `-E` | ~11.5 | **~8** | 0 | 0 | no USER/PRIO/comm |
| **R1_enrich** | default | ~8–14 | **~25** | ~459/print | ~456/print | USER+PRIO+comm fallback |
| **R2_cmdline** | `-c` | ~11.5 | **~28** | ~459 | ~654 | + full cmdline |
| **R3_topN** | `-N 20` | ~15 | **~1.3** | **20** | **20** | enrich only top 20 |
| **R4_enrich_threads** | `-T` | **~46** | ~9.5 | ~457 | 0 | fetch dominates |
| **R5_plain_threads** | `-E -T` | **~45** | ~4.5 | 0 | 0 | thread walk only |

## Deltas (what restores cost)

| Comparison | Effect |
|------------|--------|
| **R1 − R0 print** | **~+17 ms** per full dump for USER+PRIO (+ short `/proc/comm` when `ac_comm` empty) |
| **R2 − R1 print** | **~+3 ms** for full cmdline on all rows |
| **R0/R1/R2 fetch** | **Flat** — restores are not on the sample path |
| **R3** | Enrich cost ≈ free when limited to top 20 rows |
| **`-T` vs default fetch** | **~3–4×** more netlink; unrelated to enrich |

UID cache: `sum_getpwuid` ≈ 1 per run (not per row).

## How to re-measure

```bash
docker compose -f docker/docker-compose.yml run --rm --entrypoint bash iotop -lc '
  make NO_FLTO=1 CFLAGS="-O3 -g" -j$(nproc)
  LABEL=after ITERS=4 ./scripts/perf/bench_restore.sh
'
```
