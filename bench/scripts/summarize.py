"""Median of each (file, server) cell from run.sh's JSONL output, as a Markdown table."""

import json
import statistics as st
import sys

rows = [json.loads(line) for line in open(sys.argv[1])]
print("| file | conns | server | req/s (median) | MB/s | p50 µs | p99 µs | non-200 | load |")
print("|---|---:|---|---:|---:|---:|---:|---:|---:|")
for f in ("/1k.bin", "/100k.bin", "/1m.bin"):
    for s in ("hearth", "nginx"):
        rs = [r for r in rows if r["path"] == f and r["server"] == s]
        if not rs:
            continue

        def m(k: str) -> float:
            return st.median(r[k] for r in rs)

        print(
            f"| {f[1:]} | {rs[0]['connections']} | {s} | {m('rps'):,.0f} | {m('mb_per_s'):,.0f} "
            f"| {m('p50_us'):.0f} | {m('p99_us'):.0f} | {sum(r['non200'] for r in rs)} | {m('load1'):.1f} |"
        )
