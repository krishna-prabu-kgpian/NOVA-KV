#!/usr/bin/env python3
"""Summarises results/b1/ (scripts/run_b1_observation.sh) into summary.csv and
summary.md, next to the Part A medians for the same scenario and reader count
where results/part_a/summary/concurrent_summary.csv has them.

Medians with min-max over the repetitions; no run is dropped.
"""
import csv
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "results/b1")


def rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


runs = rows(os.path.join(OUT, "runs.csv"))
cow = {r["run_id"]: r for r in rows(os.path.join(OUT, "cow.csv"))}
part_a = {}
pa_path = os.path.join(ROOT, "results/part_a/summary/concurrent_summary.csv")
if os.path.exists(pa_path):
    for r in rows(pa_path):
        part_a[(r["variant"], r["scenario"], int(r["readers"]))] = r

groups = {}
for r in runs:
    groups.setdefault((r["scenario"], int(r["readers"])), []).append(r)

med = statistics.median
order = {"readonly": 0, "inline_writer": 1, "writer_paced": 2, "writer_max": 3}
header = ["scenario", "readers", "reps", "window_s", "read_rps_median", "read_rps_min", "read_rps_max",
          "write_rps_median", "p50_ns", "p99_ns", "p999_ns", "max_ns_worst",
          "write_p50_ns", "write_p99_ns",
          "segments_cloned_per_publication", "index_bytes_copied_per_publication",
          "directory_bytes_copied_per_publication", "owner_refs_per_publication",
          "chunk_pins_per_publication", "memory_at_end_mib_median", "versions_retained_at_end_median",
          "part_a_global_lock_read_rps", "part_a_rwlock_read_rps",
          "part_a_global_lock_write_rps", "part_a_rwlock_write_rps"]
out, md = [], []
for key in sorted(groups, key=lambda k: (order.get(k[0], 9), k[1])):
    g = groups[key]
    c = [cow[r["run_id"]] for r in g if r["run_id"] in cow]

    def per_pub(field):
        vals = [float(x[field]) / float(x["publications"]) for x in c if float(x["publications"]) > 0]
        return med(vals) if vals else 0.0

    rps = [float(r["read_rps"]) for r in g]
    gl = part_a.get(("global_lock", key[0], key[1]))
    rw = part_a.get(("rwlock", key[0], key[1]))
    row = [key[0], key[1], len(g), round(med([float(r["seconds_actual"]) for r in g]), 2),
           round(med(rps)), round(min(rps)), round(max(rps)),
           round(med([float(r["write_rps"]) for r in g])),
           med([float(r["p50_ns"]) for r in g]), med([float(r["p99_ns"]) for r in g]),
           med([float(r["p999_ns"]) for r in g]), max(float(r["max_ns"]) for r in g),
           med([float(r["write_p50_ns"]) for r in g]), med([float(r["write_p99_ns"]) for r in g]),
           round(per_pub("segments_cloned"), 3), round(per_pub("index_bytes_copied")),
           round(per_pub("directory_bytes_copied")), round(per_pub("owner_refs_taken"), 1),
           round(per_pub("chunk_pins_taken"), 1),
           round(med([float(x["memory_at_end_bytes"]) for x in c]) / 1048576.0, 1) if c else 0,
           round(med([float(x["versions_retained_at_end"]) for x in c])) if c else 0,
           round(float(gl["read_rps_median"])) if gl else "", round(float(rw["read_rps_median"])) if rw else "",
           round(float(gl["write_rps_median"])) if gl else "", round(float(rw["write_rps_median"])) if rw else ""]
    out.append(row)
    m = lambda x: f"{x / 1e6:.2f}"
    md.append([key[0].replace("_", "\\_"), key[1], row[3], m(row[4]), f"{m(row[5])}–{m(row[6])}",
               f"{row[7]:,}", f"{row[8] / 1000:.2f}", f"{row[9] / 1000:.2f}", f"{row[10] / 1000:.2f}",
               f"{row[11] / 1000:,.0f}", f"{row[12] / 1000:.1f}", f"{row[15]:,}", f"{row[16]:,}",
               f"{row[19]:,}",
               m(row[21]) if gl else "n/a", m(row[22]) if rw else "n/a"])

with open(os.path.join(OUT, "summary.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(header)
    w.writerows(out)
cols = ["scenario", "R", "window s", "read M/s", "min–max", "writes/s", "p50 µs", "p99 µs", "p99.9 µs",
        "max µs", "write p50 µs", "index bytes copied / write", "directory bytes / write",
        "held at end MiB", "Part A global\\_lock read M/s", "Part A rwlock read M/s"]
with open(os.path.join(OUT, "summary.md"), "w") as f:
    f.write("| " + " | ".join(cols) + " |\n|" + "|".join(["---"] + ["---:"] * (len(cols) - 1)) + "|\n")
    for r in md:
        f.write("| " + " | ".join(str(x) for x in r) + " |\n")
print(open(os.path.join(OUT, "summary.md")).read())
