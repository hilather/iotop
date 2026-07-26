# Memory / dirty metric cost (Rocky 8+)

Measured: container with host PID, `ITERS=4`, `-d 1`, `-s 100`, ~404 processes.

## Results (`bench_mem.sh`)

| Scenario | Flags | avg_fetch_ms | avg_print_ms | avg_n_status | avg_n_smaps | Notes |
|----------|-------|--------------|--------------|--------------|-------------|-------|
| **M0** | `-E` | 14 | **40** | 404 | 0 | status/stat every printed row |
| **M1** | default | 14 | **55** | 404 | 0 | + USER/PRIO/comm enrich |
| **M2** | `-D` | 14 | **438** | 404 | **210** | smaps_rollup (kthreads often skip) |
| **M3** | `-D -N 20` | 13 | **4** | 20 | **1** | recommended if dirty needed |
| **M4** | `-D -c` | 13 | **409** | 404 | 210 | dirty + full cmdline |
| **M5** | `-T -D` | **44** | **389** | 404 | 210 | thread walk + dirty |

## Conclusions

1. **Low-cost metrics (RSS/SWAP/state/minflt/CS/FREE/THR)** stay on the **print path**;
   `avg_fetch_ms` is unchanged (~14 ms process-only).
2. **Full-list `-D` is expensive**: print jumps **~40 ms → ~440 ms** (~10×) on this host.
3. **`-D -N 20`** keeps dirty useful and cheap (**~4 ms** print).
4. **`n_smaps` < `n_status`**: kernel threads often have no `smaps_rollup` (no mm).
5. Prefer **default without `-D`**; enable dirty for investigations or top-N dashboards.

## Re-run

```bash
docker compose -f docker/docker-compose.yml run --rm --entrypoint bash iotop -lc '
  make NO_FLTO=1 CFLAGS="-O3 -g" -j$(nproc)
  LABEL=run ITERS=4 ./scripts/perf/bench_mem.sh
'
```
