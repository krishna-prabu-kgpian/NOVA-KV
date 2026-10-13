#!/usr/bin/env python3
"""Sets the three Part D campaigns side by side, to show what the two method errors did.

    scripts/compare_part_d_campaigns.py <original> <unwrapped, placement uncontrolled> <final> <output dir>

  original   every timed run wrapped in perf stat; lock placement not controlled
  middle     timed runs started directly;           lock placement not controlled
  final      timed runs started directly;           lock placement controlled (offsets 48 and 0)

Writes <output dir>/original_vs_corrected.md and .csv from the campaigns' raw runs.csv files
(medians of the repetitions) and, for the lock offset of the two earlier campaigns, from the
address each run printed. This is the ONLY place where figures of different campaigns appear
together; it explains the corrections and is not a result. No speed-up or stall ratio in the
report is computed across campaigns.

The supplied-generator runs (track R) were started directly, with fixed arguments, in all
three campaigns: their spread is the session-to-session variation of the machine.
"""
import csv
import os
import re
import statistics
import sys

orig, mid, final, outdir = sys.argv[1:5]
med = statistics.median


def raw(base, rel):
    p = os.path.join(base, rel)
    if not os.path.isfile(p):
        return []
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


def summ(base, name):
    return raw(base, os.path.join("summary", name))


def lock_offset(base, row):
    """Offset of the lock object in its cache line for one D.1 run."""
    if row.get("lock_offset_in_line"):
        return int(row["lock_offset_in_line"])
    p = os.path.join(base, "d1/concurrent/stdout", row["run_id"] + ".txt")
    m = re.search(r"store lock object at (0x[0-9a-f]+)", open(p, errors="replace").read()) if os.path.isfile(p) else None
    return int(m.group(1), 16) % 64 if m else None


def d1_cells(base):
    cells = {}
    for r in raw(base, "d1/concurrent/runs.csv"):
        cells.setdefault((r["variant"], r["scenario"], int(r["readers"])), []).append(r)
    out = {}
    for k, rs in cells.items():
        offs = sorted({lock_offset(base, r) for r in rs} - {None})
        out[k] = {"reads": med([float(r["read_rps"]) for r in rs]), "writes": med([float(r["write_rps"]) for r in rs]),
                  "offsets": "/".join(str(o) for o in offs), "n": len(rs)}
    return out


def fmt(x):
    if x is None:
        return "—"
    return f"{x:,.0f}" if abs(x) >= 1000 else f"{x:.3f}"


def M(x):
    return "—" if x is None else f"{x / 1e6:.2f}"


o1, m1, f1 = d1_cells(orig), d1_cells(mid), d1_cells(final)
name = lambda b: open(os.path.join(b, "CAMPAIGN")).read().strip()
md = ["# The three Part D campaigns side by side", "",
      "| | Campaign | Timed runs | Lock placement (lock-based stores) | Status |", "|---|---|---|---|---|",
      f"| original | `{name(orig)}` | **wrapped in `perf stat`** | not controlled | superseded (D-070, D-071) |",
      f"| middle | `{name(mid)}` | started directly | **not controlled** | superseded for the lock-based stores (D-072) |",
      f"| final | `{name(final)}` | started directly | controlled: offset 48 and offset 0 | **source of every result** |", "",
      "Medians of five repetitions. **This table explains the corrections; it is not a result**, and nothing in the",
      "report is a ratio across campaigns. Three sessions of one machine, all on mains power with the `performance`",
      "governor.", "",
      "## D.1 track C, lock-based stores: reads/s (millions), with the lock's offset in its cache line", "",
      "In the two earlier campaigns the offset was an accident of each cell's command line (shown in brackets). Read",
      "each earlier figure against the final column with the same offset: 16 and 32 behave like 0.", "",
      "| Store | Writer | Readers | Original, wrapped [offset] | Middle, unwrapped [offset] | Final, offset 48 | Final, offset 0 |",
      "|---|---|---:|---:|---:|---:|---:|"]
rows = []
for v in ("global_lock", "rwlock"):
    for scen in ("writer_paced", "writer_max", "refkeys_paced"):
        for n in (1, 2, 4, 8):
            a, b, c48, c0 = o1.get((v, scen, n)), m1.get((v, scen, n)), f1.get((v, scen, n)), f1.get((v, scen + "_o0", n))
            if not (a or b or c48):
                continue
            cell = lambda x: f"{M(x['reads'])} [{x['offsets']}]" if x else "—"
            md.append(f"| {v} | {scen.replace('writer_', '')} | {n} | {cell(a)} | {cell(b)} | {M(c48['reads']) if c48 else '—'} | {M(c0['reads']) if c0 else '—'} |")
            rows.append(["D.1 track C", f"{v}; {scen}; {n} reader(s)", "reads/s", a and a["reads"], a and a["offsets"], b and b["reads"],
                         b and b["offsets"], c48 and c48["reads"], c0 and c0["reads"]])
            rows.append(["D.1 track C", f"{v}; {scen}; {n} reader(s)", "writer updates/s", a and a["writes"], a and a["offsets"],
                         b and b["writes"], b and b["offsets"], c48 and c48["writes"], c0 and c0["writes"]])
md += ["", "## D.1 track C, stores without a reader-side lock: reads/s (millions)", "",
       "| Store | Scenario | Readers | Original, wrapped | Middle, unwrapped | Final |", "|---|---|---:|---:|---:|---:|"]
for k in sorted(f1):
    if k[0] in ("rcu", "single"):
        a, b, c = o1.get(k), m1.get(k), f1.get(k)
        md.append(f"| {k[0]} | {k[1]} | {k[2]} | {M(a and a['reads'])} | {M(b and b['reads'])} | {M(c['reads'])} |")
        rows.append(["D.1 track C", f"{k[0]}; {k[1]}; {k[2]} reader(s)", "reads/s", a and a["reads"], "", b and b["reads"], "", c["reads"], None])


def three(title, name_csv, keyf, cellf, measures, unit_note=""):
    global md
    A, B, C = ({keyf(r): r for r in summ(x, name_csv)} for x in (orig, mid, final))
    md += ["", f"## {title}", "", unit_note, "", "| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |", "|---|---|---:|---:|---:|---:|"]
    for k in C:
        for col, label in measures(k):
            a = float(A[k][col]) if k in A and A[k].get(col) not in (None, "") else None
            b = float(B[k][col]) if k in B and B[k].get(col) not in (None, "") else None
            c = float(C[k][col])
            md.append(f"| {cellf(k)} | {label} | {fmt(a)} | {fmt(b)} | {fmt(c)} | {(f'{c / a:.2f}') if a else '—'} |")
            rows.append([title, cellf(k).replace(",", ";"), label, a, "", b, "", c, None])


three("D.1 track R: supplied generator, PRELOAD-ONLY — never wrapped, arguments fixed (the session-to-session yardstick)", "d1_reference.csv",
      lambda r: (r["driver"], r["readers"]), lambda k: f"{k[0]}, {k[1]} reader(s)", lambda k: [("rps_median", "lookups/s")])
for csvn, title in (("d2_steady.csv", "D.2 workload A (RCU store)"), ("d2_backlog.csv", "D.2 workload B (RCU store)")):
    three(title, csvn, lambda r: r["setting"], lambda k: k,
          lambda k: [("read_rps_median", "reads/s"), ("write_rps_median", "writer updates/s"), ("max_stall_ns_median", "longest stall ns (median)")])
three("D.3 transactions (RCU store)", "d3_txn.csv", lambda r: (r["txn_size"], r["index"], r["metric"]),
      lambda k: f"{k[0]} key(s), index {k[1]}", lambda k: [("median", k[2])] if k[2] in ("commits_per_s", "commit_p50_us", "commit_p99_us") else [])
three("D.3 join (RCU store)", "d3_join.csv", lambda r: (r["joiners"], r["writer"], r["metric"]),
      lambda k: f"{k[0]} joiner(s), writer {'active' if k[1] == 'on' else 'paused'}",
      lambda k: [("median", k[2])] if k[2] in ("joins_per_s", "join_p50_ms", "join_p99_ms") else [])

os.makedirs(outdir, exist_ok=True)
open(os.path.join(outdir, "original_vs_corrected.md"), "w").write("\n".join(md) + "\n")
with open(os.path.join(outdir, "original_vs_corrected.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["experiment", "cell", "measure", "original_wrapped", "original_lock_offset", "middle_unwrapped", "middle_lock_offset",
                "final", "final_lock_offset_0"])
    for r in rows:
        w.writerow([("" if x is None else (f"{x:.6g}" if isinstance(x, float) else x)) for x in r])
print(f"{len(rows)} rows -> {outdir}/original_vs_corrected.md")
