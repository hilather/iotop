#!/usr/bin/env python3
"""Compare two bench_restore CSV files (before vs after).

Usage:
  ./scripts/perf/compare_restore.py before.csv after.csv
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


def parse_csv(path: Path) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    for line in path.read_text(errors="replace").splitlines():
        if not line or line.startswith("#") or line.startswith("scenario"):
            continue
        name = line.split(",", 1)[0]
        metrics: dict[str, float] = {}
        for m in re.finditer(r"([a-z0-9_]+)=([0-9.]+)", line):
            k, v = m.group(1), m.group(2)
            try:
                metrics[k] = float(v)
            except ValueError:
                pass
        rows[name] = metrics
    return rows


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    before = parse_csv(Path(sys.argv[1]))
    after = parse_csv(Path(sys.argv[2]))
    keys = [
        "avg_fetch_ms",
        "avg_print_ms",
        "avg_n_netlink",
        "avg_n_getpwuid",
        "avg_n_cmdline",
        "avg_n_ioprio",
        "wall_ms",
    ]
    print(f"{'scenario':<22} {'metric':<16} {'before':>10} {'after':>10} {'delta':>10} {'pct':>8}")
    for name in sorted(set(before) | set(after)):
        b, a = before.get(name, {}), after.get(name, {})
        for k in keys:
            if k not in b and k not in a:
                continue
            bv, av = b.get(k, 0.0), a.get(k, 0.0)
            d = av - bv
            pct = (d / bv * 100.0) if bv else (0.0 if av == 0 else float("inf"))
            pct_s = f"{pct:+.1f}%" if pct != float("inf") else "n/a"
            print(f"{name:<22} {k:<16} {bv:10.1f} {av:10.1f} {d:10.1f} {pct_s:>8}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
