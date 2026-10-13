#!/usr/bin/env python3
"""Renders the Part D documents that quote measured numbers from their templates
(reports/part_d_report.md, docs/part_d_compliance.md, reports/milestones/D_corrected.md).

    scripts/render_part_d_report.py [--check]

Every number in the Part D report is a `{{ expression }}` in the template, evaluated here
against the summary CSVs of the PRIMARY campaign (results/part_d/PRIMARY), which
scripts/aggregate_part_d.py derives from the raw runs. Nothing is typed in by hand, so a
figure in the report cannot outlive the data it came from. `--check` renders to memory and
fails if reports/part_d_report.md differs (used by scripts/verify_all.sh).

Helpers available in an expression (all values are medians of the repetitions unless the
column name says otherwise):

  d1(variant, scenario, readers, column)      summary/d1_concurrent.csv
  ref(driver, readers, column)                summary/d1_reference.csv
  recl(scenario, readers, column)             summary/d1_rcu_reclamation.csv
  d2a(setting, column), d2b(setting, column)  summary/d2_steady.csv, d2_backlog.csv
  txn(size, index, metric, stat="median")     summary/d3_txn.csv
  join(joiners, writer, metric, stat="median")  summary/d3_join.csv
  cow(case, metric, stat="median")            summary/d3_cow.csv
  gen(mode, column)                           summary/d3_generator.csv
  oh(experiment, cell, measure)               summary/profiling_overhead.csv -> profiled / timing
  c2c(run, column)                            summary/perf_c2c.csv
  prof(pattern)                               first line of summary/profile_breakdown.txt matching a regex
  count(relative glob), cond(regex)           files in the campaign; values from conditions.log
Formatting: M(x) millions with 2 decimals; n0(x) integer with separators; n1, n2, n3 decimals;
us(ns) microseconds; rng(variant, scenario, readers) -> "median [min–max]" in millions.
"""
import csv
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, "results", "part_d")
PRIMARY = open(os.path.join(P, "PRIMARY")).read().strip()
C = os.path.join(P, PRIMARY)
S = os.path.join(C, "summary")


def load(name):
    with open(os.path.join(S, name), newline="") as f:
        return list(csv.DictReader(f))


def index(rows, keys):
    return {tuple(r[k] for k in keys): r for r in rows}


_d1 = index(load("d1_concurrent.csv"), ("variant", "scenario", "readers"))
_ref = index(load("d1_reference.csv"), ("driver", "readers"))
_recl = index(load("d1_rcu_reclamation.csv"), ("scenario", "readers"))
_d2a = index(load("d2_steady.csv"), ("setting",))
_d2b = index(load("d2_backlog.csv"), ("setting",))
_txn = index(load("d3_txn.csv"), ("txn_size", "index", "metric"))
_join = index(load("d3_join.csv"), ("joiners", "writer", "metric"))
_cow = index(load("d3_cow.csv"), ("case", "metric"))
_gen = index(load("d3_generator.csv"), ("mode",))
_oh = index(load("profiling_overhead.csv"), ("experiment", "cell", "measure"))
_c2c = index(load("perf_c2c.csv"), ("run",))
_prof = open(os.path.join(S, "profile_breakdown.txt")).read().splitlines()
_cond = [line for line in open(os.path.join(C, "conditions.log")) if line.strip()]


def _get(table, key, col, what):
    if key not in table:
        raise KeyError(f"{what}: no row {key}")
    if col not in table[key] or table[key][col] == "":
        raise KeyError(f"{what}: row {key} has no column {col!r}")
    return float(table[key][col])


def d1(v, scen, n, col): return _get(_d1, (v, scen, str(n)), col, "d1")
def ref(d, n, col): return _get(_ref, (d, str(n)), col, "ref")
def recl(scen, n, col): return _get(_recl, (scen, str(n)), col, "recl")
def d2a(s, col): return _get(_d2a, (s,), col, "d2a")
def d2b(s, col): return _get(_d2b, (s,), col, "d2b")
def txn(size, idx, metric, stat="median"): return _get(_txn, (str(size), idx, metric), stat, "txn")
def join(t, wr, metric, stat="median"): return _get(_join, (str(t), wr, metric), stat, "join")
def cow(case, metric, stat="median"): return _get(_cow, (case, metric), stat, "cow")
def gen(mode, col): return _get(_gen, (mode,), col, "gen")
def oh(exp, cell, measure): return _get(_oh, (exp, cell, measure), "ratio_profiled_over_timing", "oh")
def c2c(run, col): return _get(_c2c, (run,), col, "c2c")


def prof(pattern):
    for line in _prof:
        if re.search(pattern, line):
            return line.strip()
    raise KeyError(f"profile_breakdown.txt: nothing matches {pattern!r}")


def profnum(pattern, group=1):
    """A number captured by a regex from the first matching line of profile_breakdown.txt."""
    for line in _prof:
        m = re.search(pattern, line)
        if m:
            return float(m.group(group))
    raise KeyError(f"profile_breakdown.txt: nothing matches {pattern!r}")


def fgshare(run, role):
    """Share (%) of sampled cycles of one role in one flame graph, from profile_breakdown.txt."""
    inside = False
    for line in _prof:
        if line.strip() == run:
            inside = True
            continue
        if inside:
            m = re.match(r"\s+([\d.]+)%\s+(.*)", line)
            if not m:
                break
            if role in m.group(2):
                return float(m.group(1))
    return 0.0


def c2csym(run, symbol, field="samples"):
    """HITM load samples attributed to functions whose name contains `symbol` in one perf c2c report
    (from profile_breakdown.txt): field = samples | pct | lines | total (all HITM samples of the report)."""
    inside, total, acc = False, 0.0, {"samples": 0.0, "pct": 0.0, "lines": 0.0}
    for line in _prof:
        if line.startswith("== "):
            inside = line.rstrip().endswith(f"/{run}.c2c-report.txt")
            continue
        if not inside:
            continue
        m = re.match(r"\s+HITM load samples in the Pareto section: (\d+)", line)
        if m:
            total = float(m.group(1))
            continue
        m = re.match(r"\s+([\d.]+)\s+([\d.]+)%\s+(\d+) lines\s+(.*)", line)
        if m and symbol in m.group(4):
            acc["samples"] += float(m.group(1))
            acc["pct"] += float(m.group(2))
            acc["lines"] += float(m.group(3))
    if total == 0:
        raise KeyError(f"profile_breakdown.txt: no c2c section for {run}")
    return total if field == "total" else acc[field]


def c2csrc(run, what):
    """Share (%) of data-sourced load samples served from `what` (L1, L2, L3, HITM, DRAM) in one c2c report."""
    for line in _prof:
        if line.strip().startswith(run + ":"):
            m = re.search((r"modified in another core \(HITM\) " if what == "HITM" else what + " ") + r"([\d.]+)%", line)
            return float(m.group(1))
    raise KeyError(f"profile_breakdown.txt: no c2c summary line for {run}")


def perfcell(variant, scen, readers, field):
    """One field of the perf stat line of a D.1 cell in profile_breakdown.txt (IPC, cycles/read, instr/read,
    cache-miss/read, L1d-miss/read, ctx-switches, or 'slowest' / 'fastest' reader in M/s)."""
    for line in _prof:
        if re.match(rf"\s+{variant}\s+{scen}\s+r={readers}:", line):
            if field in ("slowest", "fastest"):
                m = re.search(r"slowest/fastest reader=\s*([\d.]+)/\s*([\d.]+)M", line)
                return float(m.group(1 if field == "slowest" else 2))
            return float(re.search(re.escape(field) + r"=\s*([\d.]+)", line).group(1))
    raise KeyError(f"profile_breakdown.txt: no perf stat line for {variant} {scen} r={readers}")


_cmp = index(load("original_vs_corrected.csv") if os.path.isfile(os.path.join(S, "original_vs_corrected.csv")) else
             list(csv.DictReader(open(os.path.join(P, "summary", "original_vs_corrected.csv"), newline=""))), ("cell", "measure"))


def cmp3(cell, measure, col):
    """results/part_d/summary/original_vs_corrected.csv: col = original_wrapped | original_lock_offset |
    middle_unwrapped | middle_lock_offset | final | final_lock_offset_0."""
    v = _cmp[(cell, measure)][col]
    return float(v) if re.fullmatch(r"[\d.e+\-]+", v) and "offset" not in col.replace("final_lock_offset_0", "") else v


def yardstick(which):
    """Final / original ratio over the supplied-generator cells (never wrapped, fixed arguments):
    which = 'min' | 'max'."""
    r = [float(v["final"]) / float(v["original_wrapped"]) for (cell, measure), v in _cmp.items()
         if measure == "lookups/s" and v["original_wrapped"] and v["final"]]
    return min(r) if which == "min" else max(r)


def count(pattern): return len(glob.glob(os.path.join(C, pattern)))
def cond(regex, which=0): return re.search(regex, _cond[which]).group(1)
def condall(regex): return [float(m.group(1)) for m in (re.search(regex, line) for line in _cond) if m]
def ncond(): return len(_cond)


def M(x): return f"{x / 1e6:.2f}"
def n0(x): return f"{x:,.0f}"
def n1(x): return f"{x:,.1f}"
def n2(x): return f"{x:,.2f}"
def n3(x): return f"{x:,.3f}"
def us(ns, d=3): return f"{ns / 1e3:,.{d}f}"
def ms(ns, d=1): return f"{ns / 1e6:,.{d}f}"
def pct(x, d=0): return f"{x * 100:.{d}f}"
def times(x, d=1): return f"{x:.{d}f}×"


def rng(v, scen, n):
    return f"{M(d1(v, scen, n, 'read_rps_median'))} [{M(d1(v, scen, n, 'read_rps_min'))}–{M(d1(v, scen, n, 'read_rps_max'))}]"


def wr(v, scen, n):
    """Writer updates/s: the median, with the range only when it is not flat."""
    a, lo, hi = (d1(v, scen, n, c) for c in ("write_rps_median", "write_rps_min", "write_rps_max"))
    return n0(a) if hi - lo < 0.01 * max(a, 1) else f"{n0(a)} [{n0(lo)}–{n0(hi)}]"


def stop_after(v, scen, readers=(1, 2, 4, 8)):
    """The aggregator's rule: the largest reader count whose median is more than 5 % above the best
    median at any smaller count."""
    stop, best = readers[0], d1(v, scen, readers[0], "read_rps_median")
    for n in readers[1:]:
        m = d1(v, scen, n, "read_rps_median")
        if m > best * 1.05:
            stop, best = n, m
    return stop


def worst_batch(table, col="max_stall_ns_median"):
    """The largest median of the longest stall over the batch settings: (setting, value)."""
    t = _d2a if table == "A" else _d2b
    return max(((k[0], float(r[col])) for k, r in t.items() if k[0].startswith("batch_")), key=lambda kv: kv[1])


def batches(table, col):
    t = _d2a if table == "A" else _d2b
    return [float(t[(f"batch_{b}",)][col]) for b in (1, 8, 64, 256, 1024)]


_D2_SETTINGS = [("batch_1", "batch 1"), ("batch_8", "batch 8"), ("batch_64", "batch 64"), ("batch_256", "batch 256"),
                ("batch_1024", "batch 1024"), ("reclaim_disabled", "disabled (1 s)"), ("reclaim_stw", "stop-the-world")]


def d2_table(workload, kind):
    """The D.2 tables, split so that they fit a page: kind 'readers' (what a reader sees) or
    'reclamation' (what the writer and the cleaner do). workload 'A' or 'B'."""
    f = d2a if workload == "A" else d2b
    out = []
    if kind == "readers":
        out += ["| Setting | Reads/s (M), median [min–max] | p50 µs | p99 µs | p99.9 µs | Longest single reader stall, µs: median run / worst run |",
                "|---|---:|---:|---:|---:|---:|"]
        for key, name in _D2_SETTINGS:
            stall = f"{us(f(key, 'max_stall_ns_median'), 0)} / {us(f(key, 'max_stall_ns_worst'), 0)}"
            if key == "reclaim_stw":
                stall = f"**{stall}**"
            out.append(f"| {name} | {M(f(key, 'read_rps_median'))} [{M(f(key, 'read_rps_min'))}–{M(f(key, 'read_rps_max'))}] | "
                       f"{us(f(key, 'p50_ns_median'))} | {us(f(key, 'p99_ns_median'))} | {us(f(key, 'p999_ns_median'))} | {stall} |")
    else:
        mem = "memory_at_end_mib_median" if workload == "A" else "memory_max_mib_median"
        head = ("| Setting | Writer updates/s | Passes | Most versions freed in one pass | Versions reclaimed/s | Backlog max | "
                + ("Memory at end, MiB" if workload == "A" else "Memory max, MiB") + " | Cleaner CPU s |")
        if workload == "B":
            head += " Final cleanup: backlog → ms to drain |"
        out += [head, "|---|" + "---:|" * (head.count("|") - 2)]
        for key, name in _D2_SETTINGS:
            epoch = key.startswith("batch_")
            row = (f"| {name} | {n0(f(key, 'write_rps_median'))} | {n0(f(key, 'passes_median')) if epoch else '—'} | "
                   f"{n0(f(key, 'max_versions_in_a_pass')) if epoch else '—'} | {n0(f(key, 'versions_reclaimed_per_s_median'))} | "
                   f"{n0(f(key, 'backlog_max_median'))} | {n1(f(key, mem))} | {n2(f(key, 'cleaner_cpu_s_median'))} |")
            if workload == "B":
                row += (" — |" if key == "reclaim_disabled" else
                        f" {n0(f(key, 'backlog_at_final_release_median'))} → {n1(f(key, 'drain_ms_median'))} |")
            out.append(row)
    return "\n".join(out)


ENV = {k: v for k, v in globals().items() if not k.startswith("_")}
ENV.update({"min": min, "max": max, "abs": abs, "round": round, "len": len, "PRIMARY": PRIMARY,
            "CAMPAIGN": open(os.path.join(C, "CAMPAIGN")).read().strip()})


def render(text):
    def repl(m):
        expr = m.group(1).strip()
        try:
            return str(eval(expr, ENV))        # the template is this repository's own file
        except Exception as e:                 # noqa: BLE001
            raise SystemExit(f"template expression failed: {{{{ {expr} }}}}: {e!r}")
    return re.sub(r"\{\{(.*?)\}\}", repl, text)


# (template, output): every Part D document that quotes measured numbers.
DOCS = [("reports/part_d_report.template.md", "reports/part_d_report.md"),
        ("docs/part_d_compliance.template.md", "docs/part_d_compliance.md"),
        ("reports/milestones/D_corrected.template.md", "reports/milestones/D_corrected.md")]

if __name__ == "__main__":
    bad = 0
    for tpl_rel, out_rel in DOCS:
        tpl, out = os.path.join(ROOT, tpl_rel), os.path.join(ROOT, out_rel)
        if not os.path.isfile(tpl):
            if out_rel.startswith("reports/milestones/"):     # written after the verification run
                continue
            sys.exit(f"missing template {tpl_rel}")
        text = render(open(tpl).read())
        if "--check" in sys.argv:
            if not os.path.isfile(out) or open(out).read() != text:
                print(f"STALE    {out_rel} is not what {tpl_rel} and the PRIMARY campaign's data give")
                bad = 1
            else:
                print(f"ok       {out_rel} matches its template and the data")
        else:
            open(out, "w").write(text)
            print(f"rendered {out_rel} from {PRIMARY}: {len(re.findall(r'{{', open(tpl).read()))} computed values")
    sys.exit(bad)
