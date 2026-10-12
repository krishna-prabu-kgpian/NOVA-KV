#!/usr/bin/env python3
"""Summarises results/b2/ (scripts/run_b2_observation.sh) into summary.csv and
summary.md. Medians with min-max over the repetitions; no run is dropped.
Only runs of the same session are placed side by side."""
import csv
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "results/b2")
med = statistics.median


def rows(name):
    path = os.path.join(OUT, name)
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def group(rs):
    g = {}
    for r in rs:
        g.setdefault((r["variant"], r["scenario"], int(r["readers"])), []).append(r)
    return g


runs = group(rows("runs.csv"))
cow = {r["run_id"]: r for r in rows("cow.csv")}
ref_b1 = group(rows("reference_b1_runs.csv"))
ref_a = group(rows("reference_part_a_runs.csv"))


def f(g, field):
    return med([float(r[field]) for r in g])


def cf(g, field):
    vals = [float(cow[r["run_id"]][field]) for r in g if r["run_id"] in cow]
    return med(vals) if vals else 0.0


def m(x):
    return f"{x / 1e6:.2f}"


def rng(g):
    v = [float(r["read_rps"]) for r in g]
    return f"{m(min(v))}–{m(max(v))}"


def us(ns):
    return f"{ns / 1000:.2f}"


header = ["variant", "scenario", "readers", "reps", "window_s", "read_rps_median", "read_rps_min",
          "read_rps_max", "write_rps_median", "p50_ns", "p99_ns", "p999_ns", "max_ns_worst",
          "write_p50_ns", "write_p99_ns", "reclaim_batch", "versions_reclaimed", "backlog_at_end",
          "backlog_max", "reclaim_passes", "epoch_advances", "epoch_advances_blocked",
          "max_versions_in_pass", "interrupted_versions", "chunk_ids_reused", "validation_retries",
          "cleaner_cpu_s", "memory_at_end_mib", "memory_settled_mib",
          "index_bytes_copied_per_publication", "owner_refs_per_publication",
          "chunk_refs_per_publication"]
out = []


def summary_row(key, g, with_cow):
    rps = [float(r["read_rps"]) for r in g]
    row = [key[0], key[1], key[2], len(g), round(f(g, "seconds_actual"), 2), round(med(rps)),
           round(min(rps)), round(max(rps)), round(f(g, "write_rps")), f(g, "p50_ns"),
           f(g, "p99_ns"), f(g, "p999_ns"), max(float(r["max_ns"]) for r in g),
           f(g, "write_p50_ns"), f(g, "write_p99_ns")]
    if with_cow and any(r["run_id"] in cow for r in g):
        pubs = [float(cow[r["run_id"]]["publications"]) for r in g if r["run_id"] in cow]

        def per_pub(field):
            vals = [float(cow[r["run_id"]][field]) / float(cow[r["run_id"]]["publications"])
                    for r in g if r["run_id"] in cow and float(cow[r["run_id"]]["publications"]) > 0]
            return round(med(vals), 1) if vals else 0
        row += [int(cf(g, "reclaim_batch")), round(cf(g, "versions_reclaimed")),
                round(cf(g, "backlog_at_end")), round(cf(g, "backlog_max")),
                round(cf(g, "reclaim_passes")), round(cf(g, "epoch_advances")),
                round(cf(g, "epoch_advances_blocked")), round(cf(g, "reclaim_max_versions_in_pass")),
                round(cf(g, "reclaim_interrupted_versions")), round(cf(g, "chunk_ids_reused")),
                round(cf(g, "reader_validation_retries")), round(cf(g, "cleaner_cpu_ns") / 1e9, 3),
                round(cf(g, "memory_at_end_bytes") / 1048576, 1),
                round(cf(g, "memory_settled_bytes") / 1048576, 1),
                per_pub("index_bytes_copied"), per_pub("owner_refs_taken"), per_pub("chunk_refs_taken")]
        _ = pubs
    else:
        row += [""] * (len(header) - len(row))
    return row


order = {"readonly": 0, "inline_writer": 1, "writer_paced": 2, "writer_max": 3}
for src, with_cow in ((runs, True), (ref_b1, False), (ref_a, False)):
    for key in sorted(src, key=lambda k: (k[0], order.get(k[1], 9), k[1], k[2])):
        out.append(summary_row(key, src[key], with_cow))
with open(os.path.join(OUT, "summary.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(header)
    w.writerows(out)

lines = []


def table(title, cols, body):
    lines.append(f"### {title}\n")
    lines.append("| " + " | ".join(cols) + " |")
    lines.append("|" + "|".join(["---"] + ["---:"] * (len(cols) - 1)) + "|")
    for r in body:
        lines.append("| " + " | ".join(str(x) for x in r) + " |")
    lines.append("")


# 1. Scaling of the Part B store (batch 64)
body = []
for scen in ("readonly", "inline_writer", "writer_paced", "writer_max"):
    for readers in (1, 2, 4, 8):
        g = runs.get(("rcu", scen, readers))
        if not g:
            continue
        body.append([scen.replace("_", "\\_"), readers, m(f(g, "read_rps")), rng(g),
                     f"{round(f(g, 'write_rps')):,}", us(f(g, "p50_ns")), us(f(g, "p99_ns")),
                     us(f(g, "p999_ns")), f"{max(float(r['max_ns']) for r in g) / 1000:,.0f}",
                     f"{cf(g, 'backlog_max'):,.0f}", f"{cf(g, 'cleaner_cpu_ns') / 1e9:.2f}",
                     f"{cf(g, 'memory_at_end_bytes') / 1048576:.1f}"])
table("Part B store, epoch-based reclamation, reclaim batch 64",
      ["scenario", "R", "read M/s", "min–max", "writes/s", "p50 µs", "p99 µs", "p99.9 µs", "max µs",
       "backlog max", "cleaner CPU s", "held at end MiB"], body)

# 2. Batch sweep
body = []
keys = [k for k in runs if k[1].startswith("batch_")]
for key in sorted(keys, key=lambda k: int(k[1].split("_")[1])):
    g = runs[key]
    body.append([key[1].split("_")[1], m(f(g, "read_rps")), rng(g), f"{round(f(g, 'write_rps')):,}",
                 us(f(g, "p99_ns")), us(f(g, "p999_ns")),
                 f"{max(float(r['max_ns']) for r in g) / 1000:,.0f}",
                 f"{f(g, 'max_ns') / 1000:,.0f}",
                 f"{cf(g, 'versions_reclaimed'):,.0f}", f"{cf(g, 'reclaim_passes'):,.0f}",
                 f"{cf(g, 'reclaim_max_versions_in_pass'):,.0f}", f"{cf(g, 'backlog_max'):,.0f}",
                 f"{cf(g, 'cleaner_cpu_ns') / 1e9:.2f}", f"{cf(g, 'memory_at_end_bytes') / 1048576:.1f}"])
for scen, label in (("reclaim_disabled", "disabled"), ("reclaim_stw", "stop-the-world")):
    g = runs.get(("rcu", scen, 8))
    if g:
        body.append([label + f" ({f(g, 'seconds_actual'):.0f} s)", m(f(g, "read_rps")), rng(g),
                     f"{round(f(g, 'write_rps')):,}", us(f(g, "p99_ns")), us(f(g, "p999_ns")),
                     f"{max(float(r['max_ns']) for r in g) / 1000:,.0f}",
                     f"{f(g, 'max_ns') / 1000:,.0f}",
                     f"{cf(g, 'versions_reclaimed'):,.0f}", f"{cf(g, 'reclaim_passes'):,.0f}", "n/a",
                     f"{cf(g, 'backlog_max'):,.0f}", f"{cf(g, 'cleaner_cpu_ns') / 1e9:.2f}",
                     f"{cf(g, 'memory_at_end_bytes') / 1048576:.1f}"])
table("Reclaim batch at 8 readers, unthrottled writer (3 s window unless noted)",
      ["batch", "read M/s", "min–max", "writes/s", "p99 µs", "p99.9 µs", "worst max µs",
       "median max µs", "versions reclaimed", "passes", "max versions in a pass", "backlog max",
       "cleaner CPU s", "held at end MiB"], body)

# 3. Same-session references
body = []
for label, src, variant in (("Part B (B2), batch 64", runs, "rcu"), ("B1 checkpoint (no reclamation)", ref_b1, "b1"),
                            ("Part A rwlock", ref_a, "rwlock"), ("Part A global\\_lock", ref_a, "global_lock"),
                            ("Part A single thread, no lock", ref_a, "single")):
    for scen in ("readonly", "writer_paced", "writer_max"):
        for readers in (1, 8):
            g = src.get((variant, scen, readers))
            if not g:
                continue
            body.append([label, scen.replace("_", "\\_"), readers, f"{f(g, 'seconds_actual'):.0f}",
                         m(f(g, "read_rps")), rng(g), f"{round(f(g, 'write_rps')):,}",
                         us(f(g, "p50_ns")), us(f(g, "p99_ns")), us(f(g, "p999_ns"))])
table("Same-session references (same harness, pinning, machine state)",
      ["store", "scenario", "R", "window s", "read M/s", "min–max", "writes/s", "p50 µs", "p99 µs",
       "p99.9 µs"], body)

# 4. Copy cost
g = runs.get(("rcu", "writer_max", 8))
if g:
    def pp(field):
        vals = [float(cow[r["run_id"]][field]) / float(cow[r["run_id"]]["publications"]) for r in g if r["run_id"] in cow]
        return med(vals)
    table("Copy-on-write cost per publication (8 readers, unthrottled writer)",
          ["segments cloned", "index bytes copied", "directory bytes copied", "segment owner refs",
           "chunk refs", "value bytes"],
          [[f"{pp('segments_cloned'):.2f}", f"{pp('index_bytes_copied'):,.0f}",
            f"{pp('directory_bytes_copied'):,.0f}", f"{pp('owner_refs_taken'):.0f}",
            f"{pp('chunk_refs_taken'):.1f}", f"{pp('value_bytes_appended'):.0f}"]])

with open(os.path.join(OUT, "summary.md"), "w") as fh:
    fh.write("\n".join(lines))
print("\n".join(lines))
