#!/usr/bin/env python3
"""Summarizes results/c2/runs.csv: median [min-max] over the repetitions of each
(experiment, threads, writer, txn_size, index, metric, detail). Writes summary.csv
and summary.md next to it. Fails if any row is marked invalid."""
import csv
import statistics
import sys
from collections import OrderedDict

out = sys.argv[1] if len(sys.argv) > 1 else "results/c2"
rows = list(csv.DictReader(open(f"{out}/runs.csv")))
if not rows:
    sys.exit("no rows")
invalid = [r for r in rows if r["valid"] != "1"]
groups = OrderedDict()
for r in rows:
    key = (r["experiment"], int(r["threads"]), r["writer"], int(r["txn_size"]), r["index"], r["metric"], r["detail"], r["unit"])
    groups.setdefault(key, []).append(float(r["value"]))

with open(f"{out}/summary.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["experiment", "threads", "writer", "txn_size", "index", "metric", "detail", "unit", "reps", "median", "min", "max"])
    for k, v in groups.items():
        w.writerow(list(k) + [len(v), f"{statistics.median(v):.6g}", f"{min(v):.6g}", f"{max(v):.6g}"])


def med(experiment, metric, threads=1, writer="off", txn_size=1, index="on", detail=""):
    for k, v in groups.items():
        if (k[0], k[1], k[2], k[3], k[4], k[5], k[6]) == (experiment, threads, writer, txn_size, index, metric, detail):
            return statistics.median(v), min(v), max(v), len(v)
    return None


def cell(x, fmt="{:.2f}"):
    if x is None:
        return "—"
    m, lo, hi, _ = x
    return f"{fmt.format(m)} [{fmt.format(lo)}–{fmt.format(hi)}]"


threads = sorted({k[1] for k in groups if k[0] == "join"})
lines = ["# C2 observation summary", "",
         "Median [min–max] over the repetitions. Preliminary; see config.txt and conditions.log.",
         f"Runs: {len({r['run_id'] for r in rows})}; invalid rows: {len(invalid)}.", ""]

lines += ["## Index creation (backfill)", "", "| Indexed field | Backfill ms | ns per row | Index bytes |", "|---|---:|---:|---:|"]
for d in ("trades by symbol", "trades by trade_id (all distinct)"):
    lines.append(f"| {d} | {cell(med('create', 'backfill_ms', detail=d))} | {cell(med('create', 'backfill_ns_per_row', detail=d), '{:.0f}')} | {cell(med('create', 'index_bytes', detail=d), '{:.0f}')} |")

lines += ["", "## Index maintenance: single-key puts on Trades (writer only)", "",
          "| Case | puts/s | index segments rebuilt per put | index bytes built per put |", "|---|---:|---:|---:|"]
for d in ("no index", "index, field unchanged", "index, field changes"):
    lines.append(f"| {d} | {cell(med('maintain', 'put_per_s', detail=d), '{:.0f}')} | {cell(med('maintain', 'index_segments_per_put', detail=d))} | {cell(med('maintain', 'index_bytes_per_put', detail=d), '{:.0f}')} |")

lines += ["", "## Transaction commit on Trades, every write changing the indexed field", "",
          "| Keys per transaction | Index | commits/s | rows/s | commit p50 µs | p99 µs | max µs |", "|---:|---|---:|---:|---:|---:|---:|"]
for size in (1, 4, 16):
    for idx in ("off", "on"):
        g = lambda m: med("txn", m, txn_size=size, index=idx)
        lines.append(f"| {size} | {idx} | {cell(g('commits_per_s'), '{:.0f}')} | {cell(g('rows_per_s'), '{:.0f}')} | {cell(g('commit_p50_us'), '{:.1f}')} | {cell(g('commit_p99_us'), '{:.1f}')} | {cell(g('commit_max_us'), '{:.0f}')} |")

lines += ["", "## index_get on Instruments (one match per probe)", "",
          "| Threads | Writer | M lookups/s | p50 µs | p99 µs | p99.9 µs | max µs | writer puts/s |", "|---:|---|---:|---:|---:|---:|---:|---:|"]
for t in threads:
    for wr in ("off", "on"):
        g = lambda m: med("lookup", m, threads=t, writer=wr)
        lines.append(f"| {t} | {wr} | {cell(g('index_get_mops'))} | {cell(g('index_get_p50_us'), '{:.3f}')} | {cell(g('index_get_p99_us'), '{:.3f}')} | {cell(g('index_get_p999_us'), '{:.3f}')} | {cell(g('index_get_max_us'), '{:.1f}')} | {cell(g('writer_puts_per_s'), '{:.0f}')} |")

lines += ["", "## Index-nested-loop join (Trades ⋈ Instruments, one snapshot per store)", "",
          "| Joiner threads | Writer | joins/s | M joined rows/s | join p50 ms | p99 ms | max ms | Trade puts/s | Instrument txns/s | held max MiB | backlog max |",
          "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
for t in threads:
    for wr in ("off", "on"):
        g = lambda m: med("join", m, threads=t, writer=wr)
        lines.append(f"| {t} | {wr} | {cell(g('joins_per_s'), '{:.1f}')} | {cell(g('joined_rows_per_s'))} | {cell(g('join_p50_ms'))} | {cell(g('join_p99_ms'))} | {cell(g('join_max_ms'))} | {cell(g('writer_trade_puts_per_s'), '{:.0f}')} | {cell(g('writer_instrument_txns_per_s'), '{:.0f}')} | {cell(g('memory_held_max_mib'), '{:.1f}')} | {cell(g('backlog_max_versions'), '{:.0f}')} |")
idle = med("join", "memory_held_idle_mib", threads=threads[0], writer="off") if threads else None
lines += ["", f"Memory held by both stores when idle: {cell(idle, '{:.1f}')} MiB.", ""]
open(f"{out}/summary.md", "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
if invalid:
    sys.exit(f"{len(invalid)} invalid rows")
