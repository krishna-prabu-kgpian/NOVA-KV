#!/usr/bin/env python3
"""Aggregates a Part D campaign (scripts/run_part_d.sh) into summary tables and figures.

    scripts/aggregate_part_d.py [results/part_d] [--pilot]

Writes <dir>/summary/*.csv, <dir>/summary/summary.md, <dir>/summary/problems.txt and
<dir>/figures/*.png. Exits non-zero if the campaign is incomplete, mixed or invalid:

  - a stage directory stamped with another campaign id (results from two runs mixed);
  - an INVALID marker, or a conditions.log line without mains power / performance governor;
  - an expected cell of the matrix with fewer runs than the campaign's repetition count;
  - a run without exit status 0, without its histogram, without its perf stat output;
  - a row a harness marked invalid; a duplicate run id; a malformed row;
  - a reported percentile that is not what the run's raw histogram gives;
  - (campaigns with an unwrapped timing track) a timing run whose recorded command starts a
    profiler or tracer, or a profiling run without its perf stat output.

Two campaign layouts are understood. In the first campaign every timed run was wrapped in
perf stat (counters beside the runs, profiles under perf/). Since decision D-071 the timing
track is unwrapped and the counters come from separate runs under profiling/perf_stat/; the
throughput of those profiled runs is reported only as the profiler's effect on each cell.

Nothing is estimated: every number is the median, minimum or maximum of values read from
the raw files, and no run is dropped. `--pilot` checks the same things for the reduced
matrix a pilot runs.
"""
import csv
import os
import re
import statistics
import sys

OUT = next((a for a in sys.argv[1:] if not a.startswith("--")), "results/part_d")
PILOT = "--pilot" in sys.argv
SUM = os.path.join(OUT, "summary")
FIG = os.path.join(OUT, "figures")
os.makedirs(SUM, exist_ok=True)
os.makedirs(FIG, exist_ok=True)
problems = []


def problem(msg):
    problems.append(msg)


def read_csv(path):
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        problem(f"missing or empty: {path}")
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def med(v):
    return statistics.median(v) if v else float("nan")


def write_csv(name, header, rows):
    with open(os.path.join(SUM, name), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def fmt(x, digits=2):
    if x is None or x != x:
        return "—"
    if abs(x) >= 1000:
        return f"{x:,.0f}"
    return f"{x:.{digits}f}"


def mmm(v, digits=2, scale=1.0):
    """median [min–max]"""
    if not v:
        return "—"
    v = [x * scale for x in v]
    return f"{fmt(med(v), digits)} [{fmt(min(v), digits)}–{fmt(max(v), digits)}]"


# ---- Campaign identity, configuration, conditions ------------------------------------------
campaign = open(os.path.join(OUT, "CAMPAIGN")).read().strip() if os.path.isfile(os.path.join(OUT, "CAMPAIGN")) else ""
if not campaign:
    problem("no CAMPAIGN file: not a run_part_d.sh output directory")
cfg = {}
cfg_path = os.path.join(OUT, "env", "campaign_config.txt")
if os.path.isfile(cfg_path):
    text = open(cfg_path).read()
    for key in ("reps", "d1_duration_s", "reference_duration_s", "d2_d3_duration_s"):
        m = re.search(rf"\b{key}=(\S+)", text)
        if m:
            cfg[key] = m.group(1)
    for key in ("reader_counts", "reclaim_batches", "txn_sizes", "join_threads"):
        m = re.search(rf'{key}="([^"]*)"', text)
        if m:
            cfg[key] = m.group(1).split()
else:
    problem(f"missing: {cfg_path}")
V2 = os.path.isfile(cfg_path) and "timing_track=unwrapped" in open(cfg_path).read()
# Since decision D-072 the lock object of the lock-based stores is placed explicitly and every
# such cell exists twice: scenario <name> (offset 48 in its cache line) and <name>_o0 (offset 0).
PLACED = os.path.isfile(cfg_path) and "lock_placement=controlled" in open(cfg_path).read()
PROF = "profiling" if V2 else "perf"                 # where flame graphs and c2c reports are
PSTAT = os.path.join(OUT, "profiling", "perf_stat") if V2 else OUT   # base of the perf stat runs
REPS = int(cfg.get("reps", 0))
READERS = [int(x) for x in cfg.get("reader_counts", [])]
BATCHES = [int(x) for x in cfg.get("reclaim_batches", [])]
TXN_SIZES = [int(x) for x in cfg.get("txn_sizes", [])]
JOIN_THREADS = [int(x) for x in cfg.get("join_threads", [])]

STAGE_DIRS = ["d1/concurrent", "d1/reference", "d2/steady", "d2/backlog", "d3/txn", "d3/join", "d3/cow",
              "d3/generator", PROF + "/flamegraphs", PROF + "/c2c"]
PROFILED_STAGES = ["d1/concurrent", "d2/steady", "d2/backlog", "d3/txn", "d3/join"]
if V2:
    STAGE_DIRS += ["profiling/perf_stat"] + ["profiling/perf_stat/" + d for d in PROFILED_STAGES]
for d in STAGE_DIRS:
    stamp = os.path.join(OUT, d, ".campaign")
    if not os.path.isfile(stamp):
        problem(f"stage not run: {d}")
    elif open(stamp).read().strip() != campaign:
        problem(f"stage {d} belongs to another campaign ({open(stamp).read().strip()} != {campaign})")
if os.path.isfile(os.path.join(OUT, "INVALID")):
    problem("INVALID marker present: " + open(os.path.join(OUT, "INVALID")).read().strip().replace("\n", "; "))
cond_lines = []
cond_path = os.path.join(OUT, "conditions.log")
if os.path.isfile(cond_path):
    cond_lines = [line.strip() for line in open(cond_path) if line.strip()]
    for line in cond_lines:
        if "ac_online=1 " not in line or "governor=performance " not in line:
            problem(f"conditions changed: {line}")
else:
    problem("missing conditions.log")


TRACERS = re.compile(r"^# command: *(perf|strace|ltrace|valgrind|/usr/bin/time|time|gdb)\b", re.M)
checked = {"timing": 0, "profiling": 0, "percentiles": 0}


def check_run_files(stage, run_id, need_hist=True, need_perf=True, base_dir=None):
    """base_dir None: a timing run (must be unwrapped in a V2 campaign)."""
    timing = base_dir is None
    base = os.path.join(OUT if timing else base_dir, stage)
    so = os.path.join(base, "stdout", run_id + ".txt")
    text = open(so, errors="replace").read() if os.path.isfile(so) else ""
    if "# exit_status: 0" not in text:
        problem(f"{stage}: run {run_id} has no stdout with exit status 0")
    if "# command: " not in text:
        problem(f"{stage}: run {run_id} has no recorded command")
    if V2 and timing:
        need_perf = False
        if TRACERS.search(text):
            problem(f"{stage}: TIMING run {run_id} was started under a profiler or tracer")
        if stage not in ("d1/reference", "d3/generator") and "# wrapper: none" not in text:
            problem(f"{stage}: TIMING run {run_id} does not record 'wrapper: none'")
        for junk in ("perf_stat", "rss"):
            if os.path.isdir(os.path.join(base, junk)):
                problem(f"{stage}: the timing track has a {junk}/ directory")
    checked["timing" if timing else "profiling"] += 1
    if need_hist:
        h = os.path.join(base, "hist", run_id + ".csv")
        if not os.path.isfile(h) or os.path.getsize(h) < 40:
            problem(f"{stage}: run {run_id} has no latency histogram")
    if need_perf:
        p = os.path.join(base, "perf_stat", run_id + ".txt")
        if not os.path.isfile(p) or "task-clock" not in open(p).read():
            problem(f"{stage}: run {run_id} has no perf stat output")


def perf_of(stage, run_id):
    """Counters of one run from its perf stat file: {event: value}, plus elapsed seconds."""
    p = os.path.join(PSTAT, stage, "perf_stat", run_id + ".txt")
    out = {}
    if not os.path.isfile(p):
        return out
    for line in open(p):
        m = re.match(r"\s*([\d,.]+)\s+(?:msec\s+)?([A-Za-z0-9_\-]+)", line)
        if m and not line.strip().startswith("#"):
            try:
                out[m.group(2)] = float(m.group(1).replace(",", ""))
            except ValueError:
                pass
        m = re.match(r"\s*([\d.]+)\s+seconds time elapsed", line)
        if m:
            out["elapsed_s"] = float(m.group(1))
    return out


def rss_of(stage, run_id):
    p = os.path.join(PSTAT, stage, "rss", run_id + ".txt")
    if os.path.isfile(p):
        m = re.search(r"maxrss_kb=(\d+)", open(p).read())
        if m:
            return int(m.group(1)) / 1024.0
    return float("nan")


def audit_rows(stage, path, rows, keyf=lambda r: r["run_id"]):
    """Malformed rows (wrong field count) and duplicate records."""
    seen = set()
    for i, r in enumerate(rows):
        if None in r or any(v is None for v in r.values()):
            problem(f"{stage}: malformed row {i + 2} in {path}")
        k = keyf(r)
        if k in seen:
            problem(f"{stage}: duplicate record {k} in {path}")
        seen.add(k)


def hist_percentiles(path, series):
    """p50, p99, p99.9 exactly as bench::LatencyHistogram::percentile computes them (rank =
    floor(total * p), value = lower bound of the bucket holding that rank), from the dumped
    histogram. Returns None if the series is absent."""
    buckets = []
    if not os.path.isfile(path):
        return None
    for line in open(path):
        parts = line.strip().split(",")
        if len(parts) == 3 and parts[0] == series and parts[1].isdigit():
            buckets.append((int(parts[1]), int(parts[2])))
    total = sum(c for _, c in buckets)
    if total == 0:
        return None
    out = []
    for q in (0.50, 0.99, 0.999):
        rank = min(int(total * q), total - 1)
        seen = 0
        for lower, c in buckets:
            seen += c
            if seen > rank:
                out.append(lower)
                break
    return out


def check_percentiles(stage, run_id, series, reported_ns, base_dir=None):
    """reported_ns: (p50, p99, p999) in nanoseconds as the run reported them."""
    got = hist_percentiles(os.path.join(base_dir or OUT, stage, "hist", run_id + ".csv"), series)
    if got is None:
        problem(f"{stage}: {run_id}: histogram has no '{series}' series")
        return
    for name, a, b in zip(("p50", "p99", "p99.9"), got, reported_ns):
        if abs(a - b) > max(1.0, 1e-5 * a):      # CSV values are printed with 6 significant digits or 3 decimals
            problem(f"{stage}: {run_id}: reported {name} {b:g} ns, histogram gives {a} ns")
    checked["percentiles"] += 1


def prof_rows(stage):
    """Rows of the perf stat profiling track for a stage (the campaign's own rows in the first layout)."""
    if not V2:
        return None
    rows = read_csv(os.path.join(PSTAT, stage, "runs.csv"))
    for run_id in sorted({r["run_id"] for r in rows}):
        check_run_files(stage, run_id, need_hist=True, need_perf=True, base_dir=PSTAT)
    return rows


overhead = []      # (stage, cell label, measure, timing median, profiled median, n timing, n profiled)


def group(rows, keyf):
    g = {}
    for r in rows:
        g.setdefault(keyf(r), []).append(r)
    return g


def expect_cells(stage, groups, cells, what):
    for c in cells:
        n = len(groups.get(c, []))
        if n < REPS:
            problem(f"{stage}: cell {what} {c} has {n} of {REPS} runs")


md = [f"# Part D summary — campaign `{campaign}`", "",
      *((["**Corrected campaign: UNWRAPPED timing track.** Every throughput, latency and stall figure comes from an executable",
          "started directly. Profiler output (perf stat, perf record, perf c2c) comes from separate runs and is confined to",
          "the section headed PROFILING TRACK.", ""]) if V2 else
        (["**Original campaign: every timed run was wrapped in `perf stat`**, which changed some results (decision D-070).",
          "Superseded as a source of timing figures by the unwrapped campaign (decision D-071); kept for audit.", ""])),
      "Generated by `scripts/aggregate_part_d.py` from the raw files in this directory. Every cell is",
      "**median [min–max]** over the campaign's repetitions unless it says otherwise; no run is dropped.", ""]

# =============================================================================================
# D.1 track C
# =============================================================================================
d1 = read_csv(os.path.join(OUT, "d1/concurrent/runs.csv"))
audit_rows("d1/concurrent", "runs.csv", d1)
for r in d1:
    check_run_files("d1/concurrent", r["run_id"])
    check_percentiles("d1/concurrent", r["run_id"], "read", (float(r["p50_ns"]), float(r["p99_ns"]), float(r["p999_ns"])))
    if int(r["read_corrupt"]) != 0:
        problem(f"d1/concurrent: {r['run_id']} read a foreign record")
    if int(r["read_hits"]) + int(r["read_misses"]) + int(r["read_corrupt"]) != int(r["read_ops"]):
        problem(f"d1/concurrent: {r['run_id']}: hits + misses do not add up to the reads")
g1 = group(d1, lambda r: (r["variant"], r["scenario"], int(r["readers"])))
p1 = prof_rows("d1/concurrent")
gp1 = group(p1, lambda r: (r["variant"], r["scenario"], int(r["readers"]))) if p1 is not None else g1
cells = [(v, s, n) for v in ("global_lock", "rwlock", "rcu") for s in ("writer_paced", "writer_max") for n in READERS]
if PLACED:
    cells += [(v, s + "_o0", n) for v in ("global_lock", "rwlock") for s in ("writer_paced", "writer_max") for n in READERS]
    for r in d1:
        lock_variant = r["variant"] in ("global_lock", "rwlock")
        want = "0" if r["scenario"].endswith("_o0") else "48"
        if lock_variant and (r.get("lock_placement") != "controlled" or r.get("lock_offset_in_line") != want):
            problem(f"d1/concurrent: {r['run_id']}: lock at offset {r.get('lock_offset_in_line')} ({r.get('lock_placement')}), expected {want}")
cells += [("single", "readonly", 1), ("single", "inline_writer", 1)]
# The series of every D.1 table and figure: (variant, scenario suffix, name)
SERIES = ([("global_lock", "", "global_lock, lock at offset 48"), ("global_lock", "_o0", "global_lock, lock at offset 0"),
           ("rwlock", "", "rwlock, lock at offset 48"), ("rwlock", "_o0", "rwlock, lock at offset 0"), ("rcu", "", "rcu")]
          if PLACED else [("global_lock", "", "global_lock"), ("rwlock", "", "rwlock"), ("rcu", "", "rcu")])
expect_cells("d1/concurrent", g1, cells, "(variant, scenario, readers)")
d1_rows = []
d1_stats = {}
for key in sorted(g1):
    runs = g1[key]
    pruns = gp1.get(key, [])          # the runs the counters come from (the same runs in the first layout)
    if V2 and not pruns:
        problem(f"profiling/perf_stat/d1/concurrent: no profiled run for cell {key}")
    perf = [perf_of("d1/concurrent", r["run_id"]) for r in pruns]
    f = lambda col: [float(r[col]) for r in runs]
    cpus = [p["task-clock"] / 1000.0 / p["elapsed_s"] for p in perf if "task-clock" in p and p.get("elapsed_s")]
    ipc = [p["instructions"] / p["cycles"] for p in perf if p.get("cycles")]
    reads = [float(r["read_ops"]) for r in pruns]
    if V2 and pruns:
        overhead.append(("D.1", f"{key[0]}, {key[1]}, {key[2]} reader(s)", "reads/s", med(f("read_rps")),
                         med([float(r["read_rps"]) for r in pruns]), len(runs), len(pruns)))
    st = {
        "reps": len(runs), "read_rps": f("read_rps"), "write_rps": f("write_rps"),
        "p50": f("p50_ns"), "p99": f("p99_ns"), "p999": f("p999_ns"), "max": f("max_ns"),
        "hit_rate": [float(r["read_hits"]) / float(r["read_ops"]) for r in runs if float(r["read_ops"]) > 0],
        "hits_rps": [float(r["read_hits"]) / float(r["seconds_actual"]) for r in runs],
        "cpus": cpus, "ipc": ipc,
        "ctx": [p.get("context-switches", float("nan")) for p in perf],
        "cache_miss_per_read": [p["cache-misses"] / n for p, n in zip(perf, reads) if "cache-misses" in p and n > 0],
        "rss": [rss_of("d1/concurrent", r["run_id"]) for r in pruns],
        "write_p99": f("write_p99_ns"), "write_max": f("write_max_ns"),
    }
    d1_stats[key] = st
    d1_rows.append(list(key) + [st["reps"]] + [f"{x:.6g}" for x in (
        med(st["read_rps"]), min(st["read_rps"]), max(st["read_rps"]), med(st["hits_rps"]), med(st["hit_rate"]),
        med(st["write_rps"]), min(st["write_rps"]), max(st["write_rps"]),
        med(st["p50"]), med(st["p99"]), med(st["p999"]), med(st["max"]), max(st["max"]),
        med(st["cpus"]), med(st["ipc"]), med(st["ctx"]), med(st["cache_miss_per_read"]), med(st["rss"]))])
write_csv("d1_concurrent.csv",
          ["variant", "scenario", "readers", "reps", "read_rps_median", "read_rps_min", "read_rps_max",
           "successful_reads_per_s_median", "hit_rate_median", "write_rps_median", "write_rps_min", "write_rps_max",
           "p50_ns_median", "p99_ns_median", "p999_ns_median", "max_ns_median", "max_ns_worst",
           "cpus_utilized_median", "instructions_per_cycle_median", "context_switches_median",
           "cache_misses_per_read_median", "max_rss_mib_median"], d1_rows)

md += ["## D.1 — point-get scaling, track C: writer ACTIVE during the measured window", "",
       "`benchmarks/concurrent_bench.cpp`, the generator's schema with every requested key present (hit rate 100 %",
       "unless stated), one writer thread overwriting existing trades for the whole window.", ""]
if V2:
    md += ["**Timing track: every executable was started directly, with no profiler or other wrapper.** The two columns",
           "marked *(prof.)* are perf stat counters from the separate profiling track (other runs of the same commands);",
           "throughput and latency are never taken from those runs.", ""]
if PLACED:
    md += ["**Lock placement.** The two lock-based stores run at two different speeds depending on where their lock object",
           "lies inside its 64-byte cache line (decision D-072). Every such cell is therefore measured twice, with the lock",
           "placed at offset 48 (its first 16 bytes end one line, the rest begins the next: the faster placement) and at",
           "offset 0 (cache-line aligned: the slower one; offsets 16 and 32 behave like 0). The RCU store's readers take",
           "no lock, so it has one row.", ""]
PF = " *(prof.)*" if V2 else ""
for scen, title in (("writer_paced", "Writer paced at 10,000 updates/s"), ("writer_max", "Writer unthrottled")):
    md += [f"### {title}", "",
           f"| Store | Readers | Reads/s (M) | Hit rate | Writer updates/s | p50 µs | p99 µs | p99.9 µs | max µs (median / worst) | CPUs used{PF} | ctx switches{PF} |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for v, sfx, name in SERIES:
        for n in READERS:
            st = d1_stats.get((v, scen + sfx, n))
            if not st:
                continue
            md.append(f"| {name} | {n} | {mmm(st['read_rps'], 2, 1e-6)} | {fmt(med(st['hit_rate']) * 100, 0)} % | {mmm(st['write_rps'], 0)} | {fmt(med(st['p50']) / 1e3, 3)} | "
                      f"{fmt(med(st['p99']) / 1e3, 3)} | {fmt(med(st['p999']) / 1e3, 3)} | {fmt(med(st['max']) / 1e3, 0)} / {fmt(max(st['max']) / 1e3, 0)} | "
                      f"{fmt(med(st['cpus']), 2)} | {fmt(med(st['ctx']), 0)} |")
    md.append("")
md += ["### One thread, and reads only", "",
       "| Store | Scenario | Readers | Reads/s (M) | Writer updates/s | p50 µs | p99 µs | p99.9 µs |", "|---|---|---:|---:|---:|---:|---:|---:|"]
for key in sorted(g1):
    if key[1] in ("readonly", "inline_writer"):
        st = d1_stats[key]
        md.append(f"| {key[0]} | {key[1]} | {key[2]} | {mmm(st['read_rps'], 2, 1e-6)} | {mmm(st['write_rps'], 0)} | "
                  f"{fmt(med(st['p50']) / 1e3, 3)} | {fmt(med(st['p99']) / 1e3, 3)} | {fmt(med(st['p999']) / 1e3, 3)} |")
md += ["", "### The generator's key distribution, with a writer (8 readers, paced writer)", "",
       "80 % hot ids (present) and 20 % `rng % (m*n)` (almost all absent), as in the supplied generator.", "",
       "| Store | Lookups/s (M) | Successful reads/s (M) | Hit rate | p50 µs | p99 µs | p99.9 µs |", "|---|---:|---:|---:|---:|---:|---:|"]
for v, sfx, name in SERIES:
    st = d1_stats.get((v, "refkeys_paced" + sfx, 8))
    if st:
        md.append(f"| {name} | {mmm(st['read_rps'], 2, 1e-6)} | {mmm(st['hits_rps'], 2, 1e-6)} | {fmt(med(st['hit_rate']) * 100, 2)} % | "
                  f"{fmt(med(st['p50']) / 1e3, 3)} | {fmt(med(st['p99']) / 1e3, 3)} | {fmt(med(st['p999']) / 1e3, 3)} |")
md.append("")

# Where scaling stops: the reader count after which the median no longer rises by more than 5 %.
md += ["### Where scaling stops (track C)", "",
       "The largest reader count whose median throughput is more than 5 % above the best median at any smaller",
       "count; beyond it adding readers does not help.", "",
       "| Store | Writer | Median reads/s (M) at 1 / 2 / 4 / 8 readers | Stops scaling after |", "|---|---|---|---|"]
scaling = {}
for v, sfx, name in SERIES:
    for scen in ("writer_paced" + sfx, "writer_max" + sfx):
        meds = [(n, med(d1_stats[(v, scen, n)]["read_rps"])) for n in READERS if (v, scen, n) in d1_stats]
        if not meds:
            continue
        stop, best = meds[0][0], meds[0][1]
        for n, m in meds[1:]:
            if m > best * 1.05:
                stop, best = n, m
        scaling[(v, scen)] = stop
        tail = "" if stop != meds[-1][0] else " (still rising at the largest count measured)"
        md.append(f"| {name} | {scen.replace('writer_', '').replace('_o0', '')} | {' / '.join(fmt(m / 1e6, 2) for _, m in meds)} | {stop} reader(s){tail} |")
md.append("")

# What the RCU store's reclamation did during the D.1 runs (from cow.csv).
cow1 = {r["run_id"]: r for r in read_csv(os.path.join(OUT, "d1/concurrent/cow.csv"))}
md += ["### RCU store during D.1: copy-on-write and reclamation", "",
       "\"Backlog max\" and \"Peak RSS\" include the loading of the 100,000 trades before the measured window; \"at end\"",
       *(["Peak RSS is from the profiling track (`/usr/bin/time` around other runs of the same command); everything else",
          "is the store's own counters in the timing runs."] if V2 else []),
       "is the state when the window closes. A backlog at the end that is close to the maximum means it was still growing.", "",
       "| Writer | Readers | Peak RSS MiB | Memory held at end MiB | Backlog max (versions) | Backlog at end | Cleaner CPU s | Bytes copied per update |",
       "|---|---:|---:|---:|---:|---:|---:|---:|"]
rcu_recl_rows = []
for scen in ("writer_paced", "writer_max"):
    for n in READERS:
        runs = g1.get(("rcu", scen, n), [])
        c = [cow1[r["run_id"]] for r in runs if r["run_id"] in cow1]
        if not c:
            continue
        cf = lambda col: [float(x[col]) for x in c]
        copied = [(float(x["index_bytes_copied"]) + float(x["directory_bytes_copied"])) / float(x["publications"]) for x in c if float(x["publications"]) > 0]
        rss = d1_stats[("rcu", scen, n)]["rss"]
        md.append(f"| {scen.replace('writer_', '')} | {n} | {mmm(rss, 0)} | {mmm([x / 1048576.0 for x in cf('memory_at_end_bytes')], 0)} | "
                  f"{mmm(cf('backlog_max'), 0)} | {fmt(med(cf('backlog_at_end')), 0)} | {fmt(med(cf('cleaner_cpu_ns')) / 1e9, 2)} | {fmt(med(copied), 0)} |")
        rcu_recl_rows.append([scen, n, len(c)] + [f"{x:.6g}" for x in (med(rss), med(cf("memory_at_end_bytes")) / 1048576.0, max(cf("memory_at_end_bytes")) / 1048576.0,
                                                                       med(cf("backlog_max")), max(cf("backlog_max")), med(cf("backlog_at_end")),
                                                                       med(cf("cleaner_cpu_ns")) / 1e9, med(copied))])
write_csv("d1_rcu_reclamation.csv", ["scenario", "readers", "reps", "max_rss_mib_median", "memory_at_end_mib_median", "memory_at_end_mib_max",
                                     "backlog_max_median", "backlog_max_worst", "backlog_at_end_median", "cleaner_cpu_s_median",
                                     "bytes_copied_per_update_median"], rcu_recl_rows)
md.append("")

rcu8 = {s: d1_stats.get(("rcu", s, 8)) for s in ("writer_paced", "writer_max")}
md += ["### The 100,000 reads/s target (RCU, 8 readers)", "",
       "| Measurement | Median reads/s | Minimum of the repetitions | Target met |", "|---|---:|---:|---|"]
for s, st in rcu8.items():
    if st:
        md.append(f"| track C, {s.replace('_', ' ')} | {fmt(med(st['read_rps']), 0)} | {fmt(min(st['read_rps']), 0)} | "
                  f"{'YES' if min(st['read_rps']) >= 100000 else 'NO'} |")

# =============================================================================================
# D.1 track R
# =============================================================================================
ref = read_csv(os.path.join(OUT, "d1/reference/runs.csv"))
audit_rows("d1/reference", "runs.csv", ref)
for r in ref:
    check_run_files("d1/reference", r["run_id"], need_hist=False, need_perf=False)
gr = group(ref, lambda r: (r["driver"], int(r["readers"])))
expect_cells("d1/reference", gr, [(d, n) for d in ("global_lock", "rwlock", "rcu") for n in READERS] + [("single_thread", 1)],
             "(driver, readers)")
ref_rows = []
for key in sorted(gr):
    runs = gr[key]
    f = lambda col: [float(r[col]) for r in runs]
    ref_rows.append(list(key) + [len(runs)] + [f"{x:.6g}" for x in (med(f("rps")), min(f("rps")), max(f("rps")), med(f("p50_us")),
                                                                     med(f("p99_us")), med(f("p999_us")), med(f("expected_overall_hit_rate")))])
write_csv("d1_reference.csv", ["driver", "readers", "reps", "rps_median", "rps_min", "rps_max", "p50_us_median", "p99_us_median",
                               "p999_us_median", "expected_overall_hit_rate"], ref_rows)
md += ["", "## D.1 — track R: supplied generator — PRELOAD-ONLY", "",
       "The unmodified `supplied/workload_gen.cpp` with its default parameters. **Its writer has finished before any",
       "reader starts: no writer is active in these numbers.** 20 % of its reads use `rng() % (m*n)` while trade ids",
       "are `i*100000 + j`, so they almost all miss; the hit rate below is measured from the store after the run.",
       "The generator prints latencies with one decimal of a microsecond, so sub-100 ns values appear as 0.0 or 0.1.", "",
       *(["The drivers' lock placement cannot be chosen (the generator's arguments are fixed); the offset each driver",
          "happened to get is shown, because it decides which of their two speeds the lock-based drivers run at.", ""]
         if ref and "lock_offset_in_line" in ref[0] else []),
       "| Driver | Readers | Lookups/s (M) | Hit rate | p50 µs | p99 µs | p99.9 µs | Lock offset in line (not controlled) |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
ref_stats = {}
for key in sorted(gr):
    runs = gr[key]
    rps = [float(r["rps"]) for r in runs]
    ref_stats[key] = rps
    md.append(f"| {key[0]} | {key[1]} | {mmm(rps, 2, 1e-6)} | {fmt(med([float(r['expected_overall_hit_rate']) for r in runs]) * 100, 2)} % | "
              f"{fmt(med([float(r['p50_us']) for r in runs]), 1)} | {fmt(med([float(r['p99_us']) for r in runs]), 1)} | "
              f"{fmt(med([float(r['p999_us']) for r in runs]), 1)} | "
              f"{'/'.join(sorted({r.get('lock_offset_in_line') or '—' for r in runs})) if key[0] in ('global_lock', 'rwlock') else 'no reader lock'} |")
if ("rcu", 8) in ref_stats:
    md += ["", f"RCU at 8 readers, track R: median {fmt(med(ref_stats[('rcu', 8)]), 0)} lookups/s, minimum "
               f"{fmt(min(ref_stats[('rcu', 8)]), 0)}: target of 100,000/s "
               f"{'met' if min(ref_stats[('rcu', 8)]) >= 100000 else 'NOT met'} (preload-only)."]

# =============================================================================================
# D.2
# =============================================================================================
SETTINGS = [f"batch_{b}" for b in BATCHES] + ["reclaim_stw", "reclaim_disabled"]


def label(s):
    return {"reclaim_stw": "stop-the-world", "reclaim_disabled": "disabled"}.get(s, s.replace("batch_", "batch "))


steady = read_csv(os.path.join(OUT, "d2/steady/runs.csv"))
cow = {r["run_id"]: r for r in read_csv(os.path.join(OUT, "d2/steady/cow.csv"))}
audit_rows("d2/steady", "runs.csv", steady)
for r in steady:
    check_run_files("d2/steady", r["run_id"])
    check_percentiles("d2/steady", r["run_id"], "read", (float(r["p50_ns"]), float(r["p99_ns"]), float(r["p999_ns"])))
    if int(r["read_corrupt"]) != 0:
        problem(f"d2/steady: {r['run_id']} read a foreign record")
gs = group(steady, lambda r: r["scenario"])
ps_ = prof_rows("d2/steady")
if ps_ is not None:
    for s_, pr in group(ps_, lambda r: r["scenario"]).items():
        if s_ in gs:
            overhead.append(("D.2 A", label(s_), "reads/s", med([float(r["read_rps"]) for r in gs[s_]]),
                             med([float(r["read_rps"]) for r in pr]), len(gs[s_]), len(pr)))
            overhead.append(("D.2 A", label(s_), "longest stall ns", med([float(r["max_ns"]) for r in gs[s_]]),
                             med([float(r["max_ns"]) for r in pr]), len(gs[s_]), len(pr)))
expect_cells("d2/steady", gs, SETTINGS, "setting")
md += ["", "## D.2 — reclamation batch, workload A: STEADY point gets", "",
       "The D.1 track C benchmark (RCU, 8 readers, unthrottled writer, 32-byte values). \"Steady\" describes the",
       "workload, not the outcome: at 8 readers the writer retires versions faster than the cleaner destroys them, so",
       "the backlog and the memory held grow for the whole window (columns \"Backlog max\", \"Memory at end\").", "",
       "| Setting | Reads/s (M) | Writer updates/s | p50 µs | p99 µs | p99.9 µs | Longest reader stall µs (median / worst run) | Passes | Most versions freed in a pass | Backlog max | Memory at end MiB | Cleaner CPU s | Versions reclaimed/s |",
       "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
steady_rows, steady_stats = [], {}
for s in SETTINGS:
    runs = gs.get(s, [])
    if not runs:
        continue
    f = lambda col: [float(r[col]) for r in runs]
    c = [cow.get(r["run_id"]) for r in runs]
    if any(x is None for x in c):
        problem(f"d2/steady: {s} has runs without a row in cow.csv")
        c = [x for x in c if x is not None]
    cf = lambda col: [float(x[col]) for x in c]
    secs = [float(x["seconds_actual"]) for x in c]
    st = {"read_rps": f("read_rps"), "write_rps": f("write_rps"), "p50": f("p50_ns"), "p99": f("p99_ns"), "p999": f("p999_ns"),
          "max": f("max_ns"), "passes": cf("reclaim_passes"), "max_pass": cf("reclaim_max_versions_in_pass"),
          "backlog": cf("backlog_max"), "mem_end": [x / 1048576.0 for x in cf("memory_at_end_bytes")],
          "cleaner": [x / 1e9 for x in cf("cleaner_cpu_ns")],
          "reclaim_rate": [float(x["versions_reclaimed"]) / t for x, t in zip(c, secs) if t > 0],
          "stw_pauses": cf("stw_pauses")}
    steady_stats[s] = st
    if s.startswith("batch_") and st["max_pass"] and max(st["max_pass"]) > int(s.split("_")[1]):
        problem(f"d2/steady: {s}: a pass freed {max(st['max_pass'])} versions, more than the batch")
    md.append(f"| {label(s)} | {mmm(st['read_rps'], 2, 1e-6)} | {mmm(st['write_rps'], 0)} | {fmt(med(st['p50']) / 1e3, 3)} | {fmt(med(st['p99']) / 1e3, 3)} | "
              f"{fmt(med(st['p999']) / 1e3, 3)} | {fmt(med(st['max']) / 1e3, 0)} / {fmt(max(st['max']) / 1e3, 0)} | {fmt(med(st['passes']), 0)} | "
              f"{fmt(max(st['max_pass']) if st['max_pass'] else float('nan'), 0)} | {fmt(med(st['backlog']), 0)} | {fmt(med(st['mem_end']), 1)} | "
              f"{fmt(med(st['cleaner']), 2)} | {fmt(med(st['reclaim_rate']), 0)} |")
    steady_rows.append([s, len(runs)] + [f"{x:.6g}" for x in (
        med(st["read_rps"]), min(st["read_rps"]), max(st["read_rps"]), med(st["write_rps"]), med(st["p50"]), med(st["p99"]),
        med(st["p999"]), med(st["max"]), max(st["max"]), med(st["passes"]), max(st["max_pass"]) if st["max_pass"] else float("nan"),
        med(st["backlog"]), med(st["mem_end"]), med(st["cleaner"]), med(st["reclaim_rate"]))])
write_csv("d2_steady.csv", ["setting", "reps", "read_rps_median", "read_rps_min", "read_rps_max", "write_rps_median", "p50_ns_median",
                            "p99_ns_median", "p999_ns_median", "max_stall_ns_median", "max_stall_ns_worst", "passes_median",
                            "max_versions_in_a_pass", "backlog_max_median", "memory_at_end_mib_median", "cleaner_cpu_s_median",
                            "versions_reclaimed_per_s_median"], steady_rows)

backlog = read_csv(os.path.join(OUT, "d2/backlog/runs.csv"))
audit_rows("d2/backlog", "runs.csv", backlog)
for r in backlog:
    check_run_files("d2/backlog", r["run_id"])
    check_percentiles("d2/backlog", r["run_id"], "read_measure", (float(r["p50_ns"]), float(r["p99_ns"]), float(r["p999_ns"])))
    if r["valid"] != "1":
        problem(f"d2/backlog: {r['run_id']} marked invalid by the harness")
    if r["stopped_at_retain_limit"] != "0":
        problem(f"d2/backlog: {r['run_id']} stopped at the memory guard")
gb = group(backlog, lambda r: r["scenario"])
pb_ = prof_rows("d2/backlog")
if pb_ is not None:
    for s_, pr in group(pb_, lambda r: r["scenario"]).items():
        if s_ in gb:
            overhead.append(("D.2 B", label(s_), "reads/s", med([float(r["read_rps"]) for r in gb[s_]]),
                             med([float(r["read_rps"]) for r in pr]), len(gb[s_]), len(pr)))
            overhead.append(("D.2 B", label(s_), "longest stall ns", med([float(r["max_ns"]) for r in gb[s_]]),
                             med([float(r["max_ns"]) for r in pr]), len(gb[s_]), len(pr)))
expect_cells("d2/backlog", gb, SETTINGS, "setting")
md += ["", "## D.2 — reclamation batch, workload B: BACKLOG-BUILDING", "",
       "`benchmarks/reclaim_bench.cpp`: 2,000 keys with 4 KiB values, 8 readers, unthrottled writer, and a thread that",
       "holds a Snapshot for 20 ms out of every 50 ms. While a snapshot is held nothing retired after it can be freed,",
       "so a backlog of thousands of versions builds and is released in passes. **Not comparable with workload A**",
       "(other values, other key count, pinning).", "",
       "| Setting | Reads/s (M) | Writer updates/s | p50 µs | p99 µs | p99.9 µs | Longest reader stall µs (median / worst run) | Passes | Most versions freed in a pass | Backlog max | Memory max MiB | Cleaner CPU s | Versions reclaimed/s | Final cleanup: backlog → ms to drain | Longest stall during cleanup µs |",
       "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
backlog_rows, backlog_stats = [], {}
for s in SETTINGS:
    runs = gb.get(s, [])
    if not runs:
        continue
    f = lambda col: [float(r[col]) for r in runs]
    st = {"read_rps": f("read_rps"), "write_rps": f("write_rps"), "p50": f("p50_ns"), "p99": f("p99_ns"), "p999": f("p999_ns"),
          "max": f("max_ns"), "passes": f("reclaim_passes"), "max_pass": f("reclaim_max_versions_in_pass"),
          "backlog": f("backlog_max"), "mem_max": [x / 1048576.0 for x in f("held_max_bytes")], "cleaner": f("cleaner_cpu_s"),
          "reclaim_rate": [float(r["versions_reclaimed"]) / float(r["seconds_actual"]) for r in runs],
          "at_release": f("backlog_at_release"), "drain": f("drain_ms"), "cleanup_max": f("cleanup_max_ns")}
    backlog_stats[s] = st
    if s.startswith("batch_") and max(st["max_pass"]) > int(s.split("_")[1]):
        problem(f"d2/backlog: {s}: a pass freed {max(st['max_pass'])} versions, more than the batch")
    drain = "—" if s == "reclaim_disabled" else f"{fmt(med(st['at_release']), 0)} → {fmt(med(st['drain']), 1)}"
    md.append(f"| {label(s)} | {mmm(st['read_rps'], 2, 1e-6)} | {mmm(st['write_rps'], 0)} | {fmt(med(st['p50']) / 1e3, 3)} | {fmt(med(st['p99']) / 1e3, 3)} | "
              f"{fmt(med(st['p999']) / 1e3, 3)} | {fmt(med(st['max']) / 1e3, 0)} / {fmt(max(st['max']) / 1e3, 0)} | {fmt(med(st['passes']), 0)} | "
              f"{fmt(max(st['max_pass']), 0)} | {fmt(med(st['backlog']), 0)} | {fmt(med(st['mem_max']), 1)} | {fmt(med(st['cleaner']), 2)} | "
              f"{fmt(med(st['reclaim_rate']), 0)} | {drain} | {'—' if s == 'reclaim_disabled' else fmt(med(st['cleanup_max']) / 1e3, 0)} |")
    backlog_rows.append([s, len(runs)] + [f"{x:.6g}" for x in (
        med(st["read_rps"]), min(st["read_rps"]), max(st["read_rps"]), med(st["write_rps"]), med(st["p50"]), med(st["p99"]),
        med(st["p999"]), med(st["max"]), max(st["max"]), med(st["passes"]), max(st["max_pass"]), med(st["backlog"]),
        med(st["mem_max"]), med(st["cleaner"]), med(st["reclaim_rate"]), med(st["at_release"]), med(st["drain"]), med(st["cleanup_max"]))])
write_csv("d2_backlog.csv", ["setting", "reps", "read_rps_median", "read_rps_min", "read_rps_max", "write_rps_median", "p50_ns_median",
                             "p99_ns_median", "p999_ns_median", "max_stall_ns_median", "max_stall_ns_worst", "passes_median",
                             "max_versions_in_a_pass", "backlog_max_median", "memory_max_mib_median", "cleaner_cpu_s_median",
                             "versions_reclaimed_per_s_median", "backlog_at_final_release_median", "drain_ms_median",
                             "cleanup_max_stall_ns_median"], backlog_rows)

md += ["", "### Bounded reclamation against stop-the-world: worst reader pause", "",
       "| Workload | Stop-the-world: longest stall µs (median / worst) | Epoch-based, worst of all batch sizes: longest stall µs (median / worst) | Ratio of medians |",
       "|---|---:|---:|---:|"]
for name, stats in (("A (steady)", steady_stats), ("B (backlog-building)", backlog_stats)):
    if "reclaim_stw" in stats:
        epoch = [stats[s] for s in stats if s.startswith("batch_")]
        if epoch:
            worst_med = max(med(e["max"]) for e in epoch)
            worst = max(max(e["max"]) for e in epoch)
            stw = stats["reclaim_stw"]
            md.append(f"| {name} | {fmt(med(stw['max']) / 1e3, 0)} / {fmt(max(stw['max']) / 1e3, 0)} | {fmt(worst_med / 1e3, 0)} / {fmt(worst / 1e3, 0)} | "
                      f"{fmt(med(stw['max']) / worst_med, 1)}× |")

# =============================================================================================
# D.3
# =============================================================================================
def jb_table(stage, base_dir=None):
    rows = read_csv(os.path.join(base_dir or OUT, stage, "runs.csv"))
    audit_rows(stage, "runs.csv", rows, lambda r: (r["run_id"], r["metric"], r["detail"]))
    for run_id in sorted({r["run_id"] for r in rows}):
        check_run_files(stage, run_id, need_hist=(stage != "d3/cow"), base_dir=base_dir)
    for r in rows:
        if r["valid"] != "1":
            problem(f"{stage}: {r['run_id']} marked invalid by the harness")
    # Percentiles against the raw histogram (timing runs): commit latencies are reported in µs, joins in ms.
    if base_dir is None and stage in ("d3/txn", "d3/join"):
        series, pre, unit = ("commit", "commit", 1e3) if stage == "d3/txn" else ("join", "join", 1e6)
        suffix = "us" if stage == "d3/txn" else "ms"
        by_run = group(rows, lambda r: r["run_id"])
        for run_id, rr in by_run.items():
            m = {r["metric"]: float(r["value"]) for r in rr}
            keys = [f"{pre}_p50_{suffix}", f"{pre}_p99_{suffix}", f"{pre}_p999_{suffix}"]
            if all(k in m for k in keys):
                got = hist_percentiles(os.path.join(OUT, stage, "hist", run_id + ".csv"), series)
                if got is None:
                    problem(f"{stage}: {run_id}: histogram has no '{series}' series")
                    continue
                # join_bench takes these from the sorted samples, the histogram is bucketed (width <= 1/64 of
                # the value): the reported value must lie in the bucket holding that rank, or a neighbour
                # (the two rank conventions can differ by one sample).
                for k, a in zip(keys, got):
                    lo, hi = a / unit * (1 - 1 / 64.0) - 0.002, a / unit * (1 + 2 / 64.0) + 0.002
                    if not lo <= m[k] <= hi:
                        problem(f"{stage}: {run_id}: reported {k} {m[k]:g}, histogram bucket starts at {a / unit:g}")
                checked["percentiles"] += 1
    return rows


txn = jb_table("d3/txn")
gt = group(txn, lambda r: (int(r["txn_size"]), r["index"], r["metric"]))
for size in TXN_SIZES:
    for idx in ("off", "on"):
        if len(gt.get((size, idx, "commits_per_s"), [])) < REPS:
            problem(f"d3/txn: cell (size {size}, index {idx}) has {len(gt.get((size, idx, 'commits_per_s'), []))} of {REPS} runs")
tv = lambda size, idx, metric: [float(r["value"]) for r in gt.get((size, idx, metric), [])]
if V2:
    gpt = group(jb_table("d3/txn", PSTAT), lambda r: (int(r["txn_size"]), r["index"], r["metric"]))
    for size in TXN_SIZES:
        for idx in ("off", "on"):
            pv = [float(r["value"]) for r in gpt.get((size, idx, "commits_per_s"), [])]
            if pv and tv(size, idx, "commits_per_s"):
                overhead.append(("D.3 txn", f"{size} key(s), index {idx}", "commits/s", med(tv(size, idx, "commits_per_s")), med(pv),
                                 len(tv(size, idx, "commits_per_s")), len(pv)))
md += ["", "## D.3 — multi-key transaction commit throughput", "",
       "`join_bench --experiment txn`: one writer committing transactions of N Trades (100,000-row table, keys spread",
       "over the table, every write changes the symbol). \"Index off\" is the Trades table as the assignment loads it.", "",
       "| Keys | Secondary index | Commits/s | Rows/s | p50 µs | p99 µs | p99.9 µs | max µs (median / worst) | Segments cloned / commit | Bytes copied / commit | Bytes copied / row | Memory max MiB | Backlog max |",
       "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
txn_rows = []
for size in TXN_SIZES:
    for idx in ("off", "on"):
        if not tv(size, idx, "commits_per_s"):
            continue
        mx = tv(size, idx, "commit_max_us")
        md.append(f"| {size} | {idx} | {mmm(tv(size, idx, 'commits_per_s'), 0)} | {mmm(tv(size, idx, 'rows_per_s'), 0)} | {fmt(med(tv(size, idx, 'commit_p50_us')), 1)} | "
                  f"{fmt(med(tv(size, idx, 'commit_p99_us')), 1)} | {fmt(med(tv(size, idx, 'commit_p999_us')), 1)} | {fmt(med(mx), 0)} / {fmt(max(mx), 0)} | "
                  f"{fmt(med(tv(size, idx, 'segments_cloned_per_commit')), 2)} | {fmt(med(tv(size, idx, 'bytes_copied_per_commit')), 0)} | "
                  f"{fmt(med(tv(size, idx, 'bytes_copied_per_row')), 0)} | {fmt(med(tv(size, idx, 'memory_held_max_mib')), 1)} | "
                  f"{fmt(med(tv(size, idx, 'backlog_max_versions')), 0)} |")
        metrics = sorted({k[2] for k in gt if k[0] == size and k[1] == idx})
        for m_ in metrics:
            v = tv(size, idx, m_)
            txn_rows.append([size, idx, m_, len(v), f"{med(v):.6g}", f"{min(v):.6g}", f"{max(v):.6g}"])
write_csv("d3_txn.csv", ["txn_size", "index", "metric", "reps", "median", "min", "max"], txn_rows)

join = jb_table("d3/join")
gj = group(join, lambda r: (int(r["threads"]), r["writer"], r["metric"]))
for t in JOIN_THREADS:
    for wr in ("off", "on"):
        if len(gj.get((t, wr, "joins_per_s"), [])) < REPS:
            problem(f"d3/join: cell ({t} joiners, writer {wr}) has {len(gj.get((t, wr, 'joins_per_s'), []))} of {REPS} runs")
jv = lambda t, wr, metric: [float(r["value"]) for r in gj.get((t, wr, metric), [])]
if V2:
    gpj = group(jb_table("d3/join", PSTAT), lambda r: (int(r["threads"]), r["writer"], r["metric"]))
    for t in JOIN_THREADS:
        for wr in ("off", "on"):
            pv = [float(r["value"]) for r in gpj.get((t, wr, "joins_per_s"), [])]
            if pv and jv(t, wr, "joins_per_s"):
                overhead.append(("D.3 join", f"{t} joiner(s), writer {'active' if wr == 'on' else 'paused'}", "joins/s",
                                 med(jv(t, wr, "joins_per_s")), med(pv), len(jv(t, wr, "joins_per_s")), len(pv)))
md += ["", "## D.3 — index-nested-loop join, writer active and paused", "",
       "`join_bench --experiment join` (`benchmarks/join.h`): 100,000 Trades ⋈ 50 Instruments on `symbol`; every join",
       "enumerates the outer table with `keys()`, fetches each row, probes the inner index and fetches each match.",
       "**One Snapshot per store**: each table is read from one immutable version for the whole join; the two versions",
       "are not one atomic instant, and it is two handles, not the assignment's single handle (decision D-060).",
       "Writer when active: single Trade puts that change the symbol, plus one whole-table Instrument transaction per",
       "64 puts.", "",
       "| Joiners | Writer | Joins/s | Joined rows/s (M) | p50 ms | p99 ms | p99.9 ms | max ms (median / worst) | Trade puts/s | Instrument txns/s | Memory max MiB | Backlog max |",
       "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
join_rows = []
for t in JOIN_THREADS:
    for wr in ("off", "on"):
        if not jv(t, wr, "joins_per_s"):
            continue
        mx = jv(t, wr, "join_max_ms")
        md.append(f"| {t} | {'active' if wr == 'on' else 'paused'} | {mmm(jv(t, wr, 'joins_per_s'), 1)} | {mmm(jv(t, wr, 'joined_rows_per_s'), 2)} | "
                  f"{fmt(med(jv(t, wr, 'join_p50_ms')), 2)} | {fmt(med(jv(t, wr, 'join_p99_ms')), 2)} | {fmt(med(jv(t, wr, 'join_p999_ms')), 2)} | "
                  f"{fmt(med(mx), 2)} / {fmt(max(mx), 2)} | {fmt(med(jv(t, wr, 'writer_trade_puts_per_s')), 0) if wr == 'on' else '—'} | "
                  f"{fmt(med(jv(t, wr, 'writer_instrument_txns_per_s')), 0) if wr == 'on' else '—'} | "
                  f"{fmt(med(jv(t, wr, 'memory_held_max_mib')), 1) if wr == 'on' else fmt(med(jv(t, wr, 'memory_held_idle_mib')), 1)} | "
                  f"{fmt(med(jv(t, wr, 'backlog_max_versions')), 0) if wr == 'on' else '0'} |")
        for m_ in sorted({k[2] for k in gj if k[0] == t and k[1] == wr}):
            v = jv(t, wr, m_)
            join_rows.append([t, wr, m_, len(v), f"{med(v):.6g}", f"{min(v):.6g}", f"{max(v):.6g}"])
write_csv("d3_join.csv", ["joiners", "writer", "metric", "reps", "median", "min", "max"], join_rows)
if jv(8, "on", "join_p50_ms") and jv(8, "off", "join_p50_ms"):
    md += ["", f"At 8 joiners the active writer costs a join {fmt(med(jv(8, 'on', 'join_p50_ms')) / med(jv(8, 'off', 'join_p50_ms')), 2)}× its "
               f"paused median latency ({fmt(med(jv(8, 'off', 'join_p50_ms')), 2)} → {fmt(med(jv(8, 'on', 'join_p50_ms')), 2)} ms) and "
               f"{fmt((1 - med(jv(8, 'on', 'joins_per_s')) / med(jv(8, 'off', 'joins_per_s'))) * 100, 0)} % of its throughput."]

cow3 = jb_table("d3/cow")
gc = group(cow3, lambda r: (r["detail"], r["metric"]))
cases = []
for r in cow3:
    if r["detail"] not in cases:
        cases.append(r["detail"])
if cow3 and len({r["run_id"] for r in cow3}) < REPS:
    problem(f"d3/cow: {len({r['run_id'] for r in cow3})} of {REPS} runs")
cv = lambda case, metric: [float(r["value"]) for r in gc.get((case, metric), [])]
md += ["", "## D.3 — where copy-on-write hurts", "",
       "`join_bench --experiment cow`: the same Trade rows (32 bytes each) written as one transaction and as single",
       "puts, 100,000-row table, 256 hash partitions.", "",
       "| Case | Rows | Publications | Elapsed ms | Rows/s | Segments cloned | Bytes copied | Bytes copied per row |", "|---|---:|---:|---:|---:|---:|---:|---:|"]
cow_rows = []
for case in cases:
    md.append(f"| {case} | {fmt(med(cv(case, 'rows')), 0)} | {fmt(med(cv(case, 'publications')), 0)} | {mmm(cv(case, 'elapsed_ms'), 2)} | "
              f"{fmt(med(cv(case, 'rows_per_s')), 0)} | {fmt(med(cv(case, 'segments_cloned')), 0)} | {fmt(med(cv(case, 'bytes_copied')), 0)} | "
              f"{fmt(med(cv(case, 'bytes_copied_per_row')), 0)} |")
    for m_ in sorted({k[1] for k in gc if k[0] == case}):
        v = cv(case, m_)
        cow_rows.append([case, m_, len(v), f"{med(v):.6g}", f"{min(v):.6g}", f"{max(v):.6g}"])
write_csv("d3_cow.csv", ["case", "metric", "reps", "median", "min", "max"], cow_rows)

gen = read_csv(os.path.join(OUT, "d3/generator/runs.csv"))
audit_rows("d3/generator", "runs.csv", gen)
for r in gen:
    check_run_files("d3/generator", r["run_id"], need_hist=False, need_perf=False)
gg = group(gen, lambda r: r["mode"])
for mode in ("put", "txn:16", "join"):
    if len(gg.get(mode, [])) < REPS:
        problem(f"d3/generator: mode {mode} has {len(gg.get(mode, []))} of {REPS} runs")
md += ["", "## Track R — the generator's own transaction and index-probe paths (RCU store, 8 readers, PRELOAD-ONLY)", "",
       "`use_txn` only changes how the generator LOADS (its readers then run with no writer). `use_join` makes each",
       "reader follow its `get` with one `index_get` on the Instruments store: **an index probe, not a join.** The",
       "probe hit rate is measured from the store after the run (decision D-065).", "",
       "| Generator mode | Operations/s (M) | p50 µs | p99 µs | p99.9 µs | Index-probe hit rate |", "|---|---:|---:|---:|---:|---:|"]
gen_rows = []
for mode in ("put", "txn:16", "join"):
    runs = gg.get(mode, [])
    if not runs:
        continue
    rps = [float(r["rps"]) for r in runs]
    hit = [float(r["probe_hit_rate"]) for r in runs if r["probe_hit_rate"] not in ("", None)]
    md.append(f"| {mode} | {mmm(rps, 2, 1e-6)} | {fmt(med([float(r['p50_us']) for r in runs]), 1)} | {fmt(med([float(r['p99_us']) for r in runs]), 1)} | "
              f"{fmt(med([float(r['p999_us']) for r in runs]), 1)} | {(fmt(med(hit) * 100, 1) + ' %') if hit else '—'} |")
    gen_rows.append([mode, len(runs), f"{med(rps):.6g}", f"{min(rps):.6g}", f"{max(rps):.6g}", f"{med(hit):.6g}" if hit else ""])
write_csv("d3_generator.csv", ["mode", "reps", "rps_median", "rps_min", "rps_max", "probe_hit_rate"], gen_rows)

# =============================================================================================
# perf: counters per read, flame graphs, c2c
# =============================================================================================
if V2:
    md += ["", "# PROFILING TRACK (separate runs; not timing results)", "",
           "Everything below comes from runs made under a profiler after the timing track was complete. They explain",
           "where time goes. Their throughput is shown only in the table of profiler effects."]
md += ["", "## perf stat (track C, 8 readers, unthrottled writer; whole process)", "",
       "| Store | CPUs utilized | Instructions per cycle | Cache misses per read | Context switches per second | Peak RSS MiB |", "|---|---:|---:|---:|---:|---:|"]
for v, sfx, name in SERIES:
    st = d1_stats.get((v, "writer_max" + sfx, 8))
    if st:
        dur = float(cfg.get("d1_duration_s", 1)) + 0.5
        md.append(f"| {name} | {fmt(med(st['cpus']), 2)} | {fmt(med(st['ipc']), 2)} | {fmt(med(st['cache_miss_per_read']), 4)} | "
                  f"{fmt(med(st['ctx']) / dur, 0)} | {fmt(med(st['rss']), 0)} |")
fgdir = os.path.join(OUT, PROF, "flamegraphs")
svgs = sorted(f for f in os.listdir(fgdir) if f.endswith(".svg")) if os.path.isdir(fgdir) else []
if not svgs:
    problem("perf/flamegraphs: no flame graph was produced")
for s in svgs:
    if os.path.getsize(os.path.join(fgdir, s)) < 2000:
        problem(f"perf/flamegraphs: {s} is empty")
    base_ = s[:-4]
    rl = os.path.join(fgdir, base_ + ".record.log")
    if not os.path.isfile(rl) or "# exit_status: 0" not in open(rl, errors="replace").read() or f"{base_}.data" not in open(rl, errors="replace").read():
        problem(f"flame graph {s}: no perf record log naming its own data file with exit status 0")
    for ext in (".folded", ".perf-script.txt.xz"):
        if not os.path.isfile(os.path.join(fgdir, base_ + ext)) or os.path.getsize(os.path.join(fgdir, base_ + ext)) == 0:
            problem(f"flame graph {s}: missing {base_}{ext}")
md += ["", f"Flame graphs ({len(svgs)}): " + ", ".join(f"`{PROF}/flamegraphs/{s}`" for s in svgs) + "."]
c2dir = os.path.join(OUT, PROF, "c2c")
reports = sorted(f for f in os.listdir(c2dir) if f.endswith(".c2c-report.txt")) if os.path.isdir(c2dir) else []
if not reports:
    problem("perf/c2c: no report was produced")
md += ["", "## perf c2c (cache lines written by one core and loaded by others)", "",
       "| Run | Load samples | Loads that hit a line modified by another core (HITM) | Shared cache lines |", "|---|---:|---:|---:|"]
c2c_rows = []
for rep in reports:
    text = open(os.path.join(c2dir, rep), errors="replace").read()
    def g(pat):
        m_ = re.search(pat + r"\s*:\s*(\d+)", text)
        return int(m_.group(1)) if m_ else 0
    loads, hitm, lines = g(r"Load Operations"), g(r"Load Local HITM") + g(r"Load Remote HITM"), g(r"Total Shared Cache Lines")
    if loads == 0:
        problem(f"perf/c2c: {rep} contains no load samples")
    share = f"{100.0 * hitm / loads:.2f} %" if loads else "—"
    md.append(f"| {rep.replace('.c2c-report.txt', '')} | {loads:,} | {hitm:,} ({share}) | {lines:,} |")
    c2c_rows.append([rep.replace(".c2c-report.txt", ""), loads, hitm, lines])
write_csv("perf_c2c.csv", ["run", "load_operations", "load_hitm", "shared_cache_lines"], c2c_rows)

# ---- What each profiler does to the cell it observes (V2 campaigns) ------------------------------
if V2:
    md += ["", "## What the profilers do to the numbers", "",
           "Timing-track median against the median of the same cell run under `perf stat` (with `/usr/bin/time`).",
           "This is why no profiled run supplies a throughput or latency figure. Stalls are single maxima and vary from",
           "run to run by more than this effect; the reads/s rows are the ones to read.", "",
           "| Experiment | Cell | Measure | Timing track (median of n) | Under perf stat (median of n) | Profiled / timing |",
           "|---|---|---|---:|---:|---:|"]
    oh_rows = []
    for stage_, cell, measure, tm, pm, nt, np_ in overhead:
        ratio = pm / tm if tm else float("nan")
        md.append(f"| {stage_} | {cell} | {measure} | {fmt(tm, 1)} ({nt}) | {fmt(pm, 1)} ({np_}) | {ratio:.2f} |")
        oh_rows.append([stage_, cell.replace(",", ";"), measure, f"{tm:.6g}", nt, f"{pm:.6g}", np_, f"{ratio:.4f}"])
    write_csv("profiling_overhead.csv", ["experiment", "cell", "measure", "timing_median", "timing_runs", "perf_stat_median",
                                         "perf_stat_runs", "ratio_profiled_over_timing"], oh_rows)
    # perf record and perf c2c runs: their own throughput against the timing cell they repeat.
    md += ["", "Runs under `perf record` (flame graphs) and `perf c2c record`, against the timing median of the same cell:", "",
           "| Profiled run | Reads/s under the profiler (M) | Timing-track median (M) | Profiled / timing |", "|---|---:|---:|---:|"]
    pr_rows = []
    for sub_ in ("flamegraphs", "c2c"):
        path_ = os.path.join(OUT, PROF, sub_, "profiled_runs.csv")
        for r in (read_csv(path_) if os.path.isfile(path_) else []):
            scen = r["scenario"].replace("_profiled", "").replace("_c2c", "")
            st = d1_stats.get((r["variant"], scen, int(r["readers"])))
            if scen == "reclaim_stw":
                st = {"read_rps": steady_stats["reclaim_stw"]["read_rps"]} if "reclaim_stw" in steady_stats else None
            tm = med(st["read_rps"]) if st else float("nan")
            pm = float(r["read_rps"])
            md.append(f"| {r['run_id']} | {fmt(pm / 1e6, 2)} | {fmt(tm / 1e6, 2)} | {pm / tm:.2f} |" if st else
                      f"| {r['run_id']} | {fmt(pm / 1e6, 2)} | — | — |")
            pr_rows.append([r["run_id"], f"{pm:.6g}", f"{tm:.6g}", f"{pm / tm:.4f}" if st else ""])
    write_csv("profiling_record_overhead.csv", ["run", "read_rps_profiled", "read_rps_timing_median", "ratio"], pr_rows)

# ---- Conditions ---------------------------------------------------------------------------------
md += ["", "## Conditions", "", f"- {len(cond_lines)} condition records; all on mains power with the `performance` governor: "
                                 f"{'yes' if not any('conditions changed' in p for p in problems) else 'NO'}."]
if cond_lines:
    loads = [float(re.search(r"loadavg=([\d.]+)", line).group(1)) for line in cond_lines if re.search(r"loadavg=([\d.]+)", line)]
    temps = [int(re.search(r"temp=(\d+)C", line).group(1)) for line in cond_lines if re.search(r"temp=(\d+)C", line)]
    md.append(f"- 1-minute load average while the campaign ran its own threads: {min(loads):.2f}–{max(loads):.2f}; "
              f"ACPI temperature {min(temps)}–{max(temps)} °C; first record `{cond_lines[0]}`; last `{cond_lines[-1]}`.")
md += ["", "## Completeness", "", f"- Repetitions per cell: {REPS}. Problems found: {len(problems)}.",
       f"- Timing runs checked (recorded command, exit status 0, histogram{', started without any wrapper' if V2 else ''}): {checked['timing']}."
       + (f" Profiling runs checked (perf stat output present): {checked['profiling']}." if V2 else ""),
       f"- Runs whose reported p50 / p99 / p99.9 were recomputed from the raw histogram: {checked['percentiles']} (point reads: equal;",
       "  commits and joins, which the harness computes from the sorted samples: within the histogram bucket, width at most 1.6 %).",
       "- No duplicate run id and no malformed row in any raw CSV, unless listed below."]
md += [f"  - {p}" for p in problems]

open(os.path.join(SUM, "summary.md"), "w").write("\n".join(md) + "\n")
open(os.path.join(SUM, "problems.txt"), "w").write("\n".join(problems) + ("\n" if problems else ""))

# =============================================================================================
# Figures. One y-axis each; series colour follows the store (fixed order), never its rank.
# =============================================================================================
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    problem("matplotlib not available: no figures")
    plt = None

if plt is not None:
    SURFACE, INK, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e6e5e1"
    # Categorical slots in fixed order (the palette's first four); a store keeps its colour everywhere.
    COLOR = {"rcu": "#2a78d6", "rwlock": "#eb6834", "global_lock": "#1baf7a", "single": "#eda100"}
    A, B = "#2a78d6", "#eb6834"     # two-series figures: first / second series
    NAME = {"rcu": "RCU", "rwlock": "shared mutex (rwlock)", "global_lock": "global mutex", "single": "single thread"}
    plt.rcParams.update({"font.size": 9, "axes.edgecolor": GRID, "axes.labelcolor": MUTED, "xtick.color": MUTED,
                         "ytick.color": MUTED, "text.color": INK, "axes.titlesize": 10, "axes.titleweight": "bold",
                         "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
                         "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False})

    def finish(fig, name, note=None):
        if note:
            fig.text(0.01, 0.005, note, fontsize=7, color=MUTED, ha="left", va="bottom")
        fig.tight_layout(rect=(0, 0.035 if note else 0, 1, 1))
        fig.savefig(os.path.join(FIG, name), dpi=170)
        plt.close(fig)

    def style(ax, xlabel, ylabel, title):
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_title(title, loc="left")
        ax.grid(True, axis="y", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)

    def line(ax, xs, ys, lo, hi, color, lab, end_label=True, dashed=False):
        ax.plot(xs, ys, color=color, linewidth=2, marker="o", markersize=6, markeredgecolor=color if dashed else SURFACE,
                markerfacecolor=SURFACE if dashed else color, markeredgewidth=1.5, label=lab, solid_capstyle="round",
                linestyle=(0, (4, 2)) if dashed else "-")
        ax.fill_between(xs, lo, hi, color=color, alpha=0.12, linewidth=0)

    def d1_series(scen, metric, scale):
        out = {}
        for v, sfx, name in SERIES:
            pts = [(n, d1_stats[(v, scen + sfx, n)][metric]) for n in READERS if (v, scen + sfx, n) in d1_stats]
            if pts:
                out[(v, sfx)] = ([p[0] for p in pts], [med(p[1]) * scale for p in pts], [min(p[1]) * scale for p in pts],
                                 [max(p[1]) * scale for p in pts])
        return out

    note_c = "Track C: writer active during the measured window. Median of the repetitions; band = min–max."
    if READERS:
        for metric, scale, ylabel, fname, title, logy in (
                ("read_rps", 1e-6, "reads per second (millions)", "d1_read_throughput.png", "D.1 Read throughput vs reader threads", True),
                ("p999", 1e-3, "p99.9 read latency (µs)", "d1_tail_latency.png", "D.1 Read tail latency (p99.9) vs reader threads", True),
                ("write_rps", 1.0, "successful writer updates per second", "d1_writer_throughput.png", "D.1 Writer throughput vs reader threads", True)):
            fig, axes = plt.subplots(1, 2, figsize=(9.2, 3.7), sharey=True)
            for ax, scen, sub in zip(axes, ("writer_paced", "writer_max"), ("writer paced at 10,000 updates/s", "writer unthrottled")):
                for (v, sfx), (xs, ys, lo, hi) in d1_series(scen, metric, scale).items():
                    # Colour follows the store; the second lock placement is the same colour, dashed and hollow.
                    lab = NAME[v] + ((", lock at offset 48" if not sfx else ", lock at offset 0") if PLACED and v != "rcu" else "")
                    line(ax, xs, ys, lo, hi, COLOR[v], lab, dashed=bool(sfx))
                style(ax, "reader threads", ylabel if ax is axes[0] else "", sub)
                ax.set_xscale("log", base=2)
                ax.set_xticks(READERS)
                ax.set_xticklabels([str(n) for n in READERS])
                if logy:
                    ax.set_yscale("log")
                if metric == "read_rps":
                    ax.axhline(0.1, color=MUTED, linewidth=1)
                    ax.text(READERS[0], 0.1, " 100,000 reads/s target", color=MUTED, fontsize=7, va="bottom")
            axes[1].legend(loc="best", fontsize=7 if PLACED else 8)
            fig.suptitle(title, x=0.01, ha="left", fontsize=11, fontweight="bold")
            finish(fig, fname, note_c + " Log scales." + (" Timing track: no profiler." if V2 else ""))

        # Track R, its own figure: never on the same axes as track C.
        fig, ax = plt.subplots(figsize=(5.6, 3.7))
        for drv, v in (("global_lock", "global_lock"), ("rwlock", "rwlock"), ("rcu", "rcu")):
            pts = [(n, ref_stats[(drv, n)]) for n in READERS if (drv, n) in ref_stats]
            if pts:
                line(ax, [p[0] for p in pts], [med(p[1]) / 1e6 for p in pts], [min(p[1]) / 1e6 for p in pts],
                     [max(p[1]) / 1e6 for p in pts], COLOR[v], NAME[v])
        if ("single_thread", 1) in ref_stats:
            ax.plot([1], [med(ref_stats[("single_thread", 1)]) / 1e6], marker="D", markersize=7, color=COLOR["single"],
                    markeredgecolor=SURFACE, markeredgewidth=1.5, linestyle="none", label=NAME["single"] + " (1 thread)")
        style(ax, "reader threads", "lookups per second (millions)", "Supplied generator — PRELOAD-ONLY (no writer running)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(READERS)
        ax.set_xticklabels([str(n) for n in READERS])
        ax.set_yscale("log")
        ax.legend(loc="best", fontsize=8)
        finish(fig, "d1_reference_generator.png", "Track R: unmodified generator, default parameters; about 80 % of its lookups hit. Band = min–max.")

    # D.2: one figure per measure, the two workloads side by side (they are not comparable with each other).
    # D.2: one figure per measure, the two workloads side by side (they are not
    # comparable with each other). Points with min-max ranges (the axes are
    # logarithmic for stalls, backlog and memory, so there is no bar baseline);
    # batch sizes are filled, the two comparators hollow.
    def d2_bars(metric_a, metric_b, scale, ylabel, fname, title, logy=True):
        fig, axes = plt.subplots(1, 2, figsize=(9.6, 3.9))
        for ax, stats, metric, sub, color in ((axes[0], steady_stats, metric_a, "Workload A: steady point gets", A),
                                              (axes[1], backlog_stats, metric_b, "Workload B: backlog-building", B)):
            names = [s_ for s_ in SETTINGS if s_ in stats and stats[s_].get(metric)]
            vals = [med(stats[s_][metric]) * scale for s_ in names]
            lo = [min(stats[s_][metric]) * scale for s_ in names]
            hi = [max(stats[s_][metric]) * scale for s_ in names]
            xs = list(range(len(names)))
            ax.vlines(xs, lo, hi, color=color, linewidth=2, alpha=0.45)
            nb = sum(1 for s_ in names if s_.startswith("batch_"))
            if nb > 1:
                ax.plot(xs[:nb], vals[:nb], color=color, linewidth=2, solid_capstyle="round")
            for x, s_, v in zip(xs, names, vals):
                hollow = not s_.startswith("batch_")
                ax.plot([x], [v], marker="o", markersize=8, linestyle="none", markerfacecolor=SURFACE if hollow else color,
                        markeredgecolor=color if hollow else SURFACE, markeredgewidth=1.8)
            ax.set_xticks(xs)
            ax.set_xticklabels([label(s_).replace("batch ", "") for s_ in names], fontsize=8)
            ax.set_xlim(-0.6, len(names) - 0.4)
            style(ax, "reclaim batch (versions per pass)  |  comparators (hollow)", ylabel if ax is axes[0] else "", sub)
            if logy and all(v > 0 for v in lo):
                ax.set_yscale("log")
            else:
                ax.set_ylim(bottom=0)
        fig.suptitle(title, x=0.01, ha="left", fontsize=11, fontweight="bold")
        finish(fig, fname, "Points: median of the repetitions; vertical ranges: min-max. The two workloads differ in values, keys and pinning: compare within a panel only.")

    if steady_stats or backlog_stats:
        d2_bars("read_rps", "read_rps", 1e-6, "reads per second (millions)", "d2_batch_throughput.png", "D.2 Reclaim batch vs read throughput", logy=False)
        d2_bars("max", "max", 1e-3, "longest single reader stall (µs)", "d2_batch_worst_stall.png", "D.2 Reclaim batch vs longest reader stall")
        d2_bars("backlog", "backlog", 1.0, "largest retirement backlog (versions)", "d2_batch_backlog.png", "D.2 Reclaim batch vs retirement backlog")
        d2_bars("mem_end", "mem_max", 1.0, "memory held by the store (MiB)", "d2_batch_memory.png", "D.2 Reclaim batch vs memory retained (A: at end of run; B: maximum)")

    # D.3 join: throughput and latency as two figures (two measures, two charts).
    if JOIN_THREADS and jv(JOIN_THREADS[0], "off", "joins_per_s"):
        for metric, ylabel, fname, title in (("joins_per_s", "complete joins per second", "d3_join_throughput.png", "D.3 Join throughput, writer paused vs active"),
                                             ("join_p50_ms", "median join latency (ms)", "d3_join_latency.png", "D.3 Join latency (median; band p50–p99), writer paused vs active")):
            fig, ax = plt.subplots(figsize=(5.8, 3.7))
            for wr, color, lab in (("off", A, "writer paused"), ("on", B, "writer active")):
                xs = [t for t in JOIN_THREADS if jv(t, wr, metric)]
                ys = [med(jv(t, wr, metric)) for t in xs]
                if metric == "join_p50_ms":
                    lo, hi = ys, [med(jv(t, wr, "join_p99_ms")) for t in xs]
                else:
                    lo, hi = [min(jv(t, wr, metric)) for t in xs], [max(jv(t, wr, metric)) for t in xs]
                line(ax, xs, ys, lo, hi, color, lab)
            style(ax, "joiner threads", ylabel, title)
            ax.set_xscale("log", base=2)
            ax.set_xticks(JOIN_THREADS)
            ax.set_xticklabels([str(t) for t in JOIN_THREADS])
            ax.set_ylim(bottom=0)
            ax.legend(loc="best", fontsize=8)
            finish(fig, fname, "100,000 Trades ⋈ 50 Instruments; one Snapshot per store. Median of the repetitions.")

    if TXN_SIZES and tv(TXN_SIZES[0], "off", "commits_per_s"):
        for metric, ylabel, fname, title in (("commits_per_s", "commits per second", "d3_txn_commits.png", "D.3 Transaction commit throughput vs size"),
                                             ("rows_per_s", "rows written per second", "d3_txn_rows.png", "D.3 Rows written per second vs transaction size")):
            fig, ax = plt.subplots(figsize=(5.8, 3.7))
            for idx, color, lab in (("off", A, "no secondary index"), ("on", B, "secondary index on the written field")):
                xs = [s for s in TXN_SIZES if tv(s, idx, metric)]
                if xs:
                    line(ax, xs, [med(tv(s, idx, metric)) for s in xs], [min(tv(s, idx, metric)) for s in xs],
                         [max(tv(s, idx, metric)) for s in xs], color, lab)
            style(ax, "keys per transaction", ylabel, title)
            ax.set_xscale("log", base=2)
            ax.set_xticks(TXN_SIZES)
            ax.set_xticklabels([str(s) for s in TXN_SIZES])
            ax.set_yscale("log")
            ax.legend(loc="best", fontsize=8)
            finish(fig, fname, "One writer, 100,000-row Trades table. Median of the repetitions; band = min–max. Log scales.")

    figs = sorted(f for f in os.listdir(FIG) if f.endswith(".png"))
    expected = 12 if not PILOT else 1
    if len(figs) < expected:
        problem(f"figures: {len(figs)} produced, {expected} expected")
    open(os.path.join(SUM, "problems.txt"), "w").write("\n".join(problems) + ("\n" if problems else ""))

print(f"campaign {campaign}: {len(d1)} D.1 concurrent runs, {len(ref)} reference runs, {len(steady)} + {len(backlog)} D.2 runs, "
      f"{len({r['run_id'] for r in txn})} txn runs, {len({r['run_id'] for r in join})} join runs")
print(f"summary: {SUM}/summary.md   figures: {FIG}/")
if problems:
    print(f"{len(problems)} PROBLEM(S):")
    for p in problems[:40]:
        print("  -", p)
    sys.exit(1)
print("complete: no problems found")
