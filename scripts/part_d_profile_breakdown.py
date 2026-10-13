#!/usr/bin/env python3
"""Regroups the Part D profiling data so that the report's statements about it can be checked.

    scripts/part_d_profile_breakdown.py <campaign dir> > <campaign dir>/summary/profile_breakdown.txt

In a campaign with an unwrapped timing track (decision D-071) all of this is PROFILING-TRACK
data: the counters come from the runs under <campaign>/profiling/perf_stat/ and are divided by
the reads of those same runs. The reads/s column is the timing track's, for orientation.

1. perf stat (D.1 track C runs, medians of the repetitions): per-read cycles, instructions and
   cache misses of the RCU store at 8 readers with no writer, a paced writer and an unthrottled
   writer, and the spread between the slowest and the fastest reader thread.
2. Flame-graph data (the .folded files): share of on-CPU cycles by role.
3. perf c2c: where sampled loads were served from, and (scripts/c2c_by_symbol.py) which
   functions' loads hit a line modified by another core.
Everything here is computed from files already in the campaign directory.
"""
import csv
import os
import re
import statistics
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "results/part_d"
med = statistics.median


V2 = os.path.isdir(f"{OUT}/profiling")
PBASE = f"{OUT}/profiling/perf_stat" if V2 else OUT
PROF = "profiling" if V2 else "perf"


def perf(run_id):
    d = {}
    for line in open(f"{PBASE}/d1/concurrent/perf_stat/{run_id}.txt"):
        m = re.match(r"\s*([\d,.]+)\s+(?:msec\s+)?([A-Za-z0-9_\-]+)", line)
        if m:
            try:
                d[m.group(2)] = float(m.group(1).replace(",", ""))
            except ValueError:
                pass
    return d


timing_rows = list(csv.DictReader(open(f"{OUT}/d1/concurrent/runs.csv")))
rows = list(csv.DictReader(open(f"{PBASE}/d1/concurrent/runs.csv")))
print("1. perf stat, whole process, medians of the repetitions (D.1 track C)"
      + (" -- PROFILING TRACK runs; reads/s is the TIMING track median" if V2 else ""))
print("   NOTE: counters are per PROCESS (readers + writer + cleaner + loading), divided by the reads of the")
print("   measured window: they bound, but do not isolate, what a read costs.\n")
for variant in ("rcu", "rwlock", "global_lock"):
    for scen in ("readonly", "writer_paced", "writer_max"):
        for readers in ("1", "8"):
            rs = [r for r in rows if r["variant"] == variant and r["scenario"] == scen and r["readers"] == readers]
            if not rs:
                continue
            ps = [perf(r["run_id"]) for r in rs]
            reads = [float(r["read_ops"]) for r in rs]
            ts = [r for r in timing_rows if r["variant"] == variant and r["scenario"] == scen and r["readers"] == readers]
            print(f"   {variant:11s} {scen:13s} r={readers}: reads/s={med([float(r['read_rps']) for r in ts]) / 1e6:7.2f}M  "
                  f"slowest/fastest reader={med([float(r['reader_min_rps']) for r in rs]) / 1e6:6.2f}/{med([float(r['reader_max_rps']) for r in rs]) / 1e6:6.2f}M  "
                  f"IPC={med([p['instructions'] / p['cycles'] for p in ps]):.2f}  cycles/read={med([p['cycles'] / n for p, n in zip(ps, reads)]):6.0f}  "
                  f"instr/read={med([p['instructions'] / n for p, n in zip(ps, reads)]):5.0f}  cache-miss/read={med([p['cache-misses'] / n for p, n in zip(ps, reads)]):6.3f}  "
                  f"L1d-miss/read={med([p['L1-dcache-load-misses'] / n for p, n in zip(ps, reads)]):5.2f}  ctx-switches={med([p['context-switches'] for p in ps]):9.0f}")


def role(frames):
    s = ";".join(frames)
    leaf = frames[-1]
    if "cleaner_main" in s:
        return "cleaner thread (reclamation)"
    # Time spent waiting for or handing over a lock is counted as such, whichever call it is under.
    if "[kernel]" in leaf:
        return "kernel (no symbols: kptr_restrict; futex wait/wake for the lock variants)"
    if "pthread_rwlock" in leaf or "pthread_mutex" in leaf or "lll_lock" in leaf:
        return "lock functions (libc)"
    if "VersionedStore::put" in s or "commit_locked" in s or "KVStore::put" in s:
        return "writer: store put"
    if "vdso" in leaf or "steady_clock" in leaf or "clock_gettime" in leaf:
        return "clock reads (the harness times every get)"
    if "VersionedStore::get" in s or "KVStore::get" in s or "VersionedStore::read" in s:
        return "reader: store get"
    if "_State_impl" in leaf:
        return "thread loop body (harness)"
    return "other"


print("\n2. Flame-graph data: share of sampled on-CPU cycles by role (perf record over the whole process)\n")
fgdir = f"{OUT}/{PROF}/flamegraphs"
for f in sorted(os.listdir(fgdir)):
    if not f.endswith(".folded"):
        continue
    c = {}
    for line in open(os.path.join(fgdir, f)):
        stack, n = line.rsplit(" ", 1)
        k = role(stack.split(";"))
        c[k] = c.get(k, 0) + int(n)
    total = sum(c.values())
    print(f"   {f[:-7]}")
    for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
        if 100.0 * v / total >= 0.5:
            print(f"      {100.0 * v / total:5.1f}%  {k}")

print("\n3. perf c2c (AMD IBS load/store sampling)\n")
c2dir = f"{OUT}/{PROF}/c2c"
reports = sorted(os.path.join(c2dir, f) for f in os.listdir(c2dir) if f.endswith(".c2c-report.txt"))
for rep in reports:
    text = open(rep, errors="replace").read()

    def g(pat):
        m = re.search(pat + r"\s*:\s*(\d+)", text)
        return int(m.group(1)) if m else 0

    loads, nomap = g("Load Operations"), g("Loads - no mapping")
    mapped = loads - nomap
    parts = [("L1", g("Load L1D hit")), ("L2", g("Load L2D hit")), ("L3", g("Load LLC hit")),
             ("modified in another core (HITM)", g("Load Local HITM") + g("Load Remote HITM")), ("DRAM", g("Load Local DRAM"))]
    print(f"   {os.path.basename(rep)[:-15]}: {loads} load samples, {mapped} with a data source: " +
          ", ".join(f"{name} {100.0 * n / mapped:.2f}%" for name, n in parts) + f"; shared cache lines {g('Total Shared Cache Lines')}")
print()
sys.stdout.flush()
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "c2c_by_symbol.py")] + reports)
