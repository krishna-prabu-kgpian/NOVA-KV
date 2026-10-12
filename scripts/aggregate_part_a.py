#!/usr/bin/env python3
"""Aggregates the raw Part A measurements into summary tables and figures.

Reads only files written by scripts/run_part_a.sh under results/part_a/ and
writes results/part_a/summary/*.csv, *.md and results/part_a/figures/*.

Rules (blueprint 9.3 / 16.4): every run is used, none is dropped as an outlier;
a configuration is summarised by the median of its repetitions together with
the minimum and maximum, so the spread is always visible; the worst-case
latency of a configuration is the largest maximum over its repetitions.

Usage: aggregate_part_a.py [results_dir]        (default: results/part_a)
Needs matplotlib and cairosvg for the figures (scripts/bootstrap_tools.sh
--profiling); the tables are produced even if they are missing.
"""
import csv
import glob
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "results/part_a")
SUMMARY = os.path.join(OUT, "summary")
FIGURES = os.path.join(OUT, "figures")
os.makedirs(SUMMARY, exist_ok=True)
os.makedirs(FIGURES, exist_ok=True)


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def med(values):
    return statistics.median(values)


def spread_pct(values):
    """(max - min) / median, in percent: the full observed range, not a model."""
    m = med(values)
    return 0.0 if m == 0 else 100.0 * (max(values) - min(values)) / m


def write_csv(name, header, rows):
    path = os.path.join(SUMMARY, name)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    return path


def write_md(name, header, rows, align=None):
    path = os.path.join(SUMMARY, name)
    align = align or ["l"] + ["r"] * (len(header) - 1)
    with open(path, "w") as f:
        f.write("| " + " | ".join(header) + " |\n")
        f.write("|" + "|".join("---" if a == "l" else "---:" for a in align) + "|\n")
        for r in rows:
            f.write("| " + " | ".join(str(c) for c in r) + " |\n")
    return path


def fmt_int(x):
    return f"{int(round(x)):,}"


def fmt_m(x):
    """Operations per second in millions."""
    return f"{x / 1e6:.2f}"


def fmt_us(ns):
    return f"{ns / 1000.0:.2f}"


# --------------------------------------------------------------------------- perf stat
PERF_LINE = re.compile(r"^\s*([\d,\.]+)\s+(?:msec\s+)?([A-Za-z][\w\-]*)")


def parse_perf_stat(path):
    """Returns {event: value} from a human-readable `perf stat -o` file."""
    out = {}
    with open(path) as f:
        for line in f:
            if "<not supported>" in line or "<not counted>" in line:
                ev = line.split()[-1] if line.split() else "?"
                out[ev] = None
                continue
            m = PERF_LINE.match(line)
            if m:
                out[m.group(2)] = float(m.group(1).replace(",", ""))
            m2 = re.match(r"^\s*([\d\.]+)\s+seconds time elapsed", line)
            if m2:
                out["elapsed_s"] = float(m2.group(1))
    return out


# --------------------------------------------------------------------------- concurrent
def aggregate_concurrent():
    path = os.path.join(OUT, "concurrent", "runs.csv")
    if not os.path.exists(path):
        print("no concurrent results; skipping")
        return None
    runs = read_csv(path)
    groups = {}
    for r in runs:
        key = (r["variant"], r["scenario"], int(r["readers"]))
        perf = parse_perf_stat(os.path.join(OUT, "concurrent", "perf_stat", r["run_id"] + ".txt"))
        r["_perf"] = perf
        groups.setdefault(key, []).append(r)

    order_variant = {"single": 0, "global_lock": 1, "rwlock": 2}
    order_scenario = {"readonly": 0, "inline_writer": 1, "writer_max": 2, "writer_paced": 3}
    keys = sorted(groups, key=lambda k: (order_scenario.get(k[1], 9), order_variant.get(k[0], 9), k[2]))

    header = ["variant", "scenario", "readers", "reps",
              "read_rps_median", "read_rps_min", "read_rps_max", "read_rps_range_pct",
              "write_rps_median", "write_rps_min", "write_rps_max",
              "p50_ns_median", "p99_ns_median", "p999_ns_median", "max_ns_worst", "max_ns_median",
              "mean_ns_median", "read_ops_total", "read_misses_total", "read_corrupt_total",
              "write_failed_total", "reader_min_over_max_rps_median",
              "task_clock_ms_median", "cpus_utilized_median", "cycles_median", "instructions_median",
              "ipc_median", "cache_references_median", "cache_misses_median", "cache_miss_pct_median",
              "cache_misses_per_cpu_ms_median", "cache_misses_per_1k_ops_median",
              "cycles_per_op_median", "context_switches_per_s_median", "cpu_migrations_median"]
    rows = []
    table = {}
    for key in keys:
        g = groups[key]
        rps = [float(r["read_rps"]) for r in g]
        wps = [float(r["write_rps"]) for r in g]
        perf_ok = [r for r in g if r["_perf"].get("cycles")]

        def pm(fn):
            vals = [fn(r) for r in perf_ok]
            return med(vals) if vals else float("nan")

        def ops(r):
            return float(r["read_ops"]) + float(r["write_ok"])

        row = dict(
            variant=key[0], scenario=key[1], readers=key[2], reps=len(g),
            read_rps_median=med(rps), read_rps_min=min(rps), read_rps_max=max(rps),
            read_rps_range_pct=spread_pct(rps),
            write_rps_median=med(wps), write_rps_min=min(wps), write_rps_max=max(wps),
            p50_ns_median=med([float(r["p50_ns"]) for r in g]),
            p99_ns_median=med([float(r["p99_ns"]) for r in g]),
            p999_ns_median=med([float(r["p999_ns"]) for r in g]),
            max_ns_worst=max(float(r["max_ns"]) for r in g),
            max_ns_median=med([float(r["max_ns"]) for r in g]),
            mean_ns_median=med([float(r["mean_ns"]) for r in g]),
            read_ops_total=sum(int(r["read_ops"]) for r in g),
            read_misses_total=sum(int(r["read_misses"]) for r in g),
            read_corrupt_total=sum(int(r["read_corrupt"]) for r in g),
            write_failed_total=sum(int(r["write_failed"]) for r in g),
            reader_min_over_max_rps_median=med(
                [float(r["reader_min_rps"]) / float(r["reader_max_rps"]) for r in g]),
            task_clock_ms_median=pm(lambda r: r["_perf"]["task-clock"]),
            cpus_utilized_median=pm(lambda r: r["_perf"]["task-clock"] / 1000.0 / r["_perf"]["elapsed_s"]),
            cycles_median=pm(lambda r: r["_perf"]["cycles"]),
            instructions_median=pm(lambda r: r["_perf"]["instructions"]),
            ipc_median=pm(lambda r: r["_perf"]["instructions"] / r["_perf"]["cycles"]),
            cache_references_median=pm(lambda r: r["_perf"]["cache-references"]),
            cache_misses_median=pm(lambda r: r["_perf"]["cache-misses"]),
            cache_miss_pct_median=pm(lambda r: 100.0 * r["_perf"]["cache-misses"] / r["_perf"]["cache-references"]),
            cache_misses_per_cpu_ms_median=pm(lambda r: r["_perf"]["cache-misses"] / r["_perf"]["task-clock"]),
            # Counters cover the whole process (load + warm-up + window); the
            # operation count is the measured window only, so this ratio is an
            # overestimate by the same factor (~warm-up/duration) in every row.
            cache_misses_per_1k_ops_median=pm(lambda r: 1000.0 * r["_perf"]["cache-misses"] / ops(r)),
            cycles_per_op_median=pm(lambda r: r["_perf"]["cycles"] / ops(r)),
            context_switches_per_s_median=pm(lambda r: r["_perf"]["context-switches"] / r["_perf"]["elapsed_s"]),
            cpu_migrations_median=pm(lambda r: r["_perf"]["cpu-migrations"]),
        )
        table[key] = row
        rows.append([row[h] if not isinstance(row[h], float) else round(row[h], 3) for h in header])
    write_csv("concurrent_summary.csv", header, rows)

    # Report tables.
    for scenario, fname in (("writer_max", "table_concurrent_writer_max.md"),
                            ("writer_paced", "table_concurrent_writer_paced.md")):
        md = []
        for key in keys:
            if key[1] != scenario:
                continue
            t = table[key]
            md.append([key[0].replace("_", "\\_"), key[2], fmt_m(t["read_rps_median"]),
                       f"{fmt_m(t['read_rps_min'])}–{fmt_m(t['read_rps_max'])}",
                       fmt_int(t["write_rps_median"]),
                       fmt_us(t["p50_ns_median"]), fmt_us(t["p99_ns_median"]),
                       fmt_us(t["p999_ns_median"]), fmt_us(t["max_ns_worst"])])
        write_md(fname, ["variant", "readers", "read M/s (median)", "min–max", "writes/s",
                         "p50 µs", "p99 µs", "p99.9 µs", "max µs"], md,
                 ["l"] + ["r"] * 8)
    md = []
    for key in keys:
        if key[1] != "writer_max":
            continue
        t = table[key]
        md.append([key[0].replace("_", "\\_"), key[2], f"{t['cpus_utilized_median']:.2f}",
                   f"{t['ipc_median']:.2f}", f"{t['cache_miss_pct_median']:.1f}",
                   fmt_int(t["cache_misses_per_cpu_ms_median"]),
                   f"{t['cache_misses_per_1k_ops_median']:.0f}",
                   fmt_int(t["cycles_per_op_median"]),
                   fmt_int(t["context_switches_per_s_median"])])
    write_md("table_perf_stat_writer_max.md",
             ["variant", "readers", "CPUs used", "IPC", "cache-miss %", "misses / CPU-ms",
              "misses / 1k ops", "cycles / op", "ctx-sw / s"], md, ["l"] + ["r"] * 8)
    md = []
    for key in keys:
        if key[0] != "single":
            continue
        t = table[key]
        md.append([key[1].replace("_", "\\_"), fmt_m(t["read_rps_median"]),
                   f"{fmt_m(t['read_rps_min'])}–{fmt_m(t['read_rps_max'])}",
                   fmt_int(t["write_rps_median"]), fmt_us(t["p50_ns_median"]),
                   fmt_us(t["p99_ns_median"]), fmt_us(t["p999_ns_median"]),
                   fmt_us(t["max_ns_worst"]), f"{t['ipc_median']:.2f}"])
    write_md("table_single.md", ["single-thread run", "read M/s (median)", "min–max", "writes/s",
                                 "p50 µs", "p99 µs", "p99.9 µs", "max µs", "IPC"], md,
             ["l"] + ["r"] * 8)
    return table


# --------------------------------------------------------------------------- reference
def aggregate_reference():
    path = os.path.join(OUT, "reference", "runs.csv")
    if not os.path.exists(path):
        print("no reference results; skipping")
        return None
    groups = {}
    for r in read_csv(path):
        groups.setdefault((r["driver"], int(r["readers"])), []).append(r)
    order = {"single_thread": 0, "global_lock": 1, "rwlock": 2}
    header = ["driver", "readers", "reps", "rps_median", "rps_min", "rps_max", "rps_range_pct",
              "p50_us_median", "p99_us_median", "p999_us_median"]
    rows, md, table = [], [], {}
    for key in sorted(groups, key=lambda k: (order.get(k[0], 9), k[1])):
        g = groups[key]
        rps = [float(r["rps"]) for r in g]
        row = [key[0], key[1], len(g), round(med(rps)), round(min(rps)), round(max(rps)),
               round(spread_pct(rps), 2), med([float(r["p50_us"]) for r in g]),
               med([float(r["p99_us"]) for r in g]), med([float(r["p999_us"]) for r in g])]
        rows.append(row)
        table[key] = dict(zip(header, row))
        md.append([key[0].replace("_", "\\_"), key[1], fmt_m(med(rps)),
                   f"{fmt_m(min(rps))}–{fmt_m(max(rps))}", row[7], row[8], row[9]])
    write_csv("reference_summary.csv", header, rows)
    write_md("table_reference.md", ["driver", "readers", "read M/s (median)", "min–max",
                                    "p50 µs", "p99 µs", "p99.9 µs"], md, ["l"] + ["r"] * 6)
    return table


# --------------------------------------------------------------------------- layout
PATTERNS = ["row_chain", "row_uniform", "row_hot", "sym_price", "field_price", "scan_price"]
LAYOUTS = ["nsm", "dsm", "pax"]


def aggregate_layout():
    path = os.path.join(OUT, "layout", "runs.csv")
    if not os.path.exists(path):
        print("no layout results; skipping")
        return None
    groups = {}
    for r in read_csv(path):
        groups.setdefault((int(r["records"]), r["pattern"], r["layout"]), []).append(float(r["ns_per_op"]))
    sizes = sorted({k[0] for k in groups})
    header = ["records", "pattern", "layout", "reps", "ns_per_op_median", "ns_per_op_min",
              "ns_per_op_max", "range_pct", "relative_to_nsm"]
    rows, table = [], {}
    for size in sizes:
        for pattern in PATTERNS:
            base = med(groups[(size, pattern, "nsm")])
            for layout in LAYOUTS:
                v = groups[(size, pattern, layout)]
                table[(size, pattern, layout)] = dict(median=med(v), min=min(v), max=max(v))
                rows.append([size, pattern, layout, len(v), round(med(v), 4), round(min(v), 4),
                             round(max(v), 4), round(spread_pct(v), 2), round(med(v) / base, 3)])
    write_csv("layout_summary.csv", header, rows)

    md = []
    for size in sizes:
        for pattern in PATTERNS:
            cells = [f"{size:,}", pattern.replace("_", "\\_")]
            base = table[(size, pattern, "nsm")]["median"]
            for layout in LAYOUTS:
                t = table[(size, pattern, layout)]
                cells.append(f"{t['median']:.2f}")
            for layout in ("dsm", "pax"):
                cells.append(f"{table[(size, pattern, layout)]['median'] / base:.2f}×")
            md.append(cells)
    write_md("table_layout.md", ["records", "pattern", "NSM ns", "DSM ns", "PAX ns",
                                 "DSM/NSM", "PAX/NSM"], md, ["r", "l"] + ["r"] * 5)

    # Hardware counters: (pattern run) - (setup-only run), per operation.
    perf_rows, perf_md = [], []
    stat_dir = os.path.join(OUT, "layout", "perf_stat")
    for size in sizes:
        for pattern in ("row_uniform", "row_chain"):
            for layout in LAYOUTS:
                p = os.path.join(stat_dir, f"{layout}_{pattern}_{size}.txt")
                b = os.path.join(stat_dir, f"{layout}_none_{size}.txt")
                if not (os.path.exists(p) and os.path.exists(b)):
                    continue
                run, base = parse_perf_stat(p), parse_perf_stat(b)
                ops = 4000000 * 20  # --ops 4000000 --reps 20 in run_part_a.sh (warm-up pass excluded)
                ops_with_warmup = 4000000 * 21

                def per_op(ev):
                    return (run[ev] - base[ev]) / ops_with_warmup

                perf_rows.append([size, pattern, layout, ops, round(per_op("cycles"), 3),
                                  round(per_op("instructions"), 3),
                                  round(per_op("cache-references"), 4),
                                  round(per_op("cache-misses"), 4),
                                  int(run["cache-misses"]), int(base["cache-misses"])])
                perf_md.append([f"{size:,}", pattern.replace("_", "\\_"), layout.upper(),
                                f"{per_op('cycles'):.1f}", f"{per_op('instructions'):.1f}",
                                f"{per_op('cache-misses'):.3f}"])
    write_csv("layout_perf_summary.csv",
              ["records", "pattern", "layout", "measured_ops", "cycles_per_get",
               "instructions_per_get", "cache_references_per_get", "cache_misses_per_get",
               "cache_misses_run_raw", "cache_misses_setup_only_raw"], perf_rows)
    write_md("table_layout_perf.md", ["records", "pattern", "layout", "cycles / get",
                                      "instr / get", "cache misses / get"], perf_md,
             ["r", "l", "l", "r", "r", "r"])
    return table, sizes


# --------------------------------------------------------------------------- flame graphs
# Classification of a folded stack by the frames it contains. Order matters: the
# first matching rule wins. "kernel" is appended when the stack ends in the
# kernel, which with these binaries means a futex system call or a wake-up.
RULES = [
    ("lock acquire", re.compile(
        r"mutex::lock|pthread_mutex_lock|__lll_lock_wait|lll_mutex_lock|lock_guard<[^;]*>::lock_guard"
        r"|shared_mutex::lock|pthread_rwlock_(rd|wr)lock|shared_lock<[^;]*>::shared_lock"
        r"|unique_lock<[^;]*>::unique_lock|unique_lock<[^;]*>::lock|shared_lock<[^;]*>::lock"
        r"|__pthread_rwlock|futex_wait|__futex_abstimed|__shared_mutex_pthread::lock")),
    ("lock release", re.compile(
        r"unlock|__lll_lock_wake|~lock_guard|~shared_lock|~unique_lock|futex_wake")),
    ("store: index + copy", re.compile(r"KVStore::|nova::|memcpy|memmove")),
    ("harness: clock", re.compile(r"clock_gettime|steady_clock|\[vdso\]")),
    ("harness: record build", re.compile(r"make_trade|snprintf|printf")),
    # The reader/writer loops are inlined into libstdc++'s thread trampoline
    # (std::thread::_State_impl<...>::_M_run); cycles in that frame itself are
    # the harness loop (key choice, counters, histogram update).
    ("harness: loop", re.compile(r"reader_main|writer_main|LatencyHistogram|bench::|_State_impl")),
]


def classify(stack):
    for name, rx in RULES:
        if rx.search(stack):
            return name
    return "other"


def aggregate_flamegraphs():
    files = sorted(glob.glob(os.path.join(OUT, "flamegraphs", "*.folded")))
    if not files:
        print("no folded stacks; skipping")
        return None
    cats = [r[0] for r in RULES] + ["other"]
    header = ["config", "total_cycles"] + [c + " %" for c in cats] + \
             ["lock acquire in kernel %", "lock release in kernel %", "all kernel %"]
    rows, table = [], {}
    for path in files:
        config = os.path.basename(path)[:-len(".folded")]
        total = 0
        by = {c: 0 for c in cats}
        kern = {"lock acquire": 0, "lock release": 0}
        all_kernel = 0
        with open(path) as f:
            for line in f:
                stack, _, count = line.rstrip("\n").rpartition(" ")
                count = int(count)
                total += count
                c = classify(stack)
                by[c] += count
                in_kernel = stack.endswith("[kernel]")
                if in_kernel:
                    all_kernel += count
                    if c in kern:
                        kern[c] += count
        pct = lambda x: round(100.0 * x / total, 2) if total else 0.0
        row = [config, total] + [pct(by[c]) for c in cats] + \
              [pct(kern["lock acquire"]), pct(kern["lock release"]), pct(all_kernel)]
        rows.append(row)
        table[config] = dict(zip(header, row))
    order = {"global_lock": 0, "rwlock": 1}
    rows.sort(key=lambda r: (order.get(r[0].rsplit("_r", 1)[0], 9), int(r[0].rsplit("_r", 1)[1])))
    write_csv("flamegraph_breakdown.csv", header, rows)
    md = []
    for r in rows:
        t = table[r[0]]
        variant, readers = r[0].rsplit("_r", 1)
        md.append([variant.replace("_", "\\_"), readers,
                   f"{t['lock acquire %']:.1f}", f"{t['lock release %']:.1f}",
                   f"{t['lock acquire in kernel %'] + t['lock release in kernel %']:.1f}",
                   f"{t['store: index + copy %']:.1f}",
                   f"{t['harness: clock %'] + t['harness: record build %'] + t['harness: loop %']:.1f}",
                   f"{t['other %']:.1f}"])
    write_md("table_flamegraph_breakdown.md",
             ["variant", "readers", "lock acquire %", "lock release %", "of which kernel %",
              "store work %", "harness %", "other %"], md, ["l"] + ["r"] * 7)
    return table


# --------------------------------------------------------------------------- perf c2c
def aggregate_c2c():
    files = sorted(glob.glob(os.path.join(OUT, "c2c", "*.c2c-report.txt")))
    if not files:
        print("no perf c2c reports; skipping")
        return None
    header = ["config", "lock_cachelines", "sampled_loads", "load_hitm_total",
              "shared_cache_lines", "top_line_address", "top_line_hitm", "top_line_hitm_pct",
              "top_line_is_store_lock", "locked_ops_on_shared_lines"]
    rows = []

    def field(text, label):
        m = re.search(r"^\s*" + re.escape(label) + r"\s*:\s*(\d+)", text, re.M)
        return int(m.group(1)) if m else 0

    for path in files:
        config = os.path.basename(path)[:-len(".c2c-report.txt")]
        text = open(path, errors="replace").read()
        log = open(os.path.join(OUT, "c2c", config + ".record.log"), errors="replace").read()
        m = re.search(r"store lock object at 0x[0-9a-f]+, \d+ bytes \(cache lines (0x[0-9a-f]+)\.\.(0x[0-9a-f]+)\)", log)
        lock_lo, lock_hi = (int(m.group(1), 16), int(m.group(2), 16)) if m else (0, -1)
        lock_line = f"{m.group(1)}..{m.group(2)}" if m else ""
        hitm = field(text, "Load Local HITM") + field(text, "Load Remote HITM")
        top_addr, top_hitm, top_pct = "", 0, 0.0
        # First data row of the "Shared Data Cache Line Table".
        m = re.search(r"^\s+0\s+(0x[0-9a-f]+)\s+\d+\s+\d+\s+([\d\.]+)%\s+(\d+)", text, re.M)
        if m:
            top_addr, top_pct, top_hitm = m.group(1), float(m.group(2)), int(m.group(3))
        rows.append([config, lock_line, field(text, "Load Operations"), hitm,
                     field(text, "Total Shared Cache Lines"), top_addr, top_hitm, top_pct,
                     "yes" if (top_addr and lock_lo <= int(top_addr, 16) <= lock_hi) else "no",
                     field(text, "Locked Access on shared lines")])
    order = {"global_lock": 0, "rwlock": 1}
    rows.sort(key=lambda r: (order.get(r[0].rsplit("_r", 1)[0], 9), int(r[0].rsplit("_r", 1)[1])))
    write_csv("c2c_summary.csv", header, rows)
    md = []
    for r in rows:
        variant, readers = r[0].rsplit("_r", 1)
        md.append([variant.replace("_", "\\_"), readers, fmt_int(r[2]), fmt_int(r[3]), r[4],
                   f"{r[7]:.1f}", r[8]])
    write_md("table_c2c.md", ["variant", "readers", "sampled loads", "HITM loads",
                              "contended lines", "top line: % of HITM", "top line is the store lock"],
             md, ["l"] + ["r"] * 6)
    return rows


# --------------------------------------------------------------------------- figures
def make_figures(conc, layout):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available: figures skipped")
        return
    plt.rcParams.update({"font.size": 8, "axes.titlesize": 8.5, "legend.fontsize": 7,
                         "axes.spines.top": False, "axes.spines.right": False})
    colors = {"global_lock": "#b5492c", "rwlock": "#2c6fb5", "single": "#555555"}

    if conc:
        fig, axes = plt.subplots(1, 3, figsize=(7.2, 2.25))
        readers = sorted({k[2] for k in conc if k[1] == "writer_max"})
        for ax, scenario, title in ((axes[0], "writer_max", "Read throughput, writer unthrottled"),
                                    (axes[1], "writer_paced", "Read throughput, writer 10k updates/s")):
            for variant in ("global_lock", "rwlock"):
                pts = [conc[(variant, scenario, r)] for r in readers if (variant, scenario, r) in conc]
                y = [p["read_rps_median"] / 1e6 for p in pts]
                lo = [(p["read_rps_median"] - p["read_rps_min"]) / 1e6 for p in pts]
                hi = [(p["read_rps_max"] - p["read_rps_median"]) / 1e6 for p in pts]
                ax.errorbar(readers[:len(y)], y, yerr=[lo, hi], marker="o", ms=3, lw=1.2, capsize=2,
                            color=colors[variant], label=variant)
            if ("single", "readonly", 1) in conc:
                ax.axhline(conc[("single", "readonly", 1)]["read_rps_median"] / 1e6, color=colors["single"],
                           ls="--", lw=0.9, label="single thread, no lock, no writer")
            ax.set_xscale("log", base=2)
            ax.set_xticks(readers)
            ax.set_xticklabels([str(r) for r in readers])
            ax.set_xlabel("reader threads")
            ax.set_ylabel("million gets / s (all readers)")
            ax.set_ylim(bottom=0)
            ax.set_title(title)
            ax.legend(frameon=False)
        ax = axes[2]
        for variant in ("global_lock", "rwlock"):
            pts = [conc[(variant, "writer_max", r)] for r in readers if (variant, "writer_max", r) in conc]
            ax.plot(readers[:len(pts)], [p["write_rps_median"] / 1e6 for p in pts], marker="s", ms=3,
                    lw=1.2, color=colors[variant], label=variant)
        ax.set_xscale("log", base=2)
        ax.set_xticks(readers)
        ax.set_xticklabels([str(r) for r in readers])
        ax.set_xlabel("reader threads")
        ax.set_ylabel("million puts / s (writer)")
        ax.set_ylim(bottom=0)
        ax.set_title("Writer throughput, writer unthrottled")
        ax.legend(frameon=False)
        fig.tight_layout()
        for ext in ("pdf", "png"):
            fig.savefig(os.path.join(FIGURES, f"fig_scaling.{ext}"), dpi=200)
        plt.close(fig)

    if layout:
        table, sizes = layout
        fig, axes = plt.subplots(1, len(sizes), figsize=(7.2, 2.2))
        if len(sizes) == 1:
            axes = [axes]
        lc = {"nsm": "#2c6fb5", "dsm": "#b5492c", "pax": "#4c9a52"}
        for ax, size in zip(axes, sizes):
            width = 0.26
            for li, lay in enumerate(LAYOUTS):
                med_v = [table[(size, p, lay)]["median"] for p in PATTERNS]
                lo = [table[(size, p, lay)]["median"] - table[(size, p, lay)]["min"] for p in PATTERNS]
                hi = [table[(size, p, lay)]["max"] - table[(size, p, lay)]["median"] for p in PATTERNS]
                x = [i + (li - 1) * width for i in range(len(PATTERNS))]
                ax.bar(x, med_v, width, yerr=[lo, hi], capsize=1.5, color=lc[lay], label=lay.upper(),
                       error_kw={"lw": 0.6})
            ax.set_yscale("log")
            ax.set_xticks(range(len(PATTERNS)))
            ax.set_xticklabels([p.replace("_", "\n") for p in PATTERNS])
            ax.set_ylabel("ns per access (log scale)")
            ax.set_title(f"{size:,} records ({size * 32 / 2**20:.0f} MiB of rows)")
            ax.legend(frameon=False, ncol=3)
        fig.tight_layout()
        for ext in ("pdf", "png"):
            fig.savefig(os.path.join(FIGURES, f"fig_layout.{ext}"), dpi=200)
        plt.close(fig)

    # Flame graphs: the SVGs are the deliverable; PDF copies are for the report.
    try:
        import cairosvg
    except ImportError:
        print("cairosvg not available: flame-graph PDFs skipped")
        return
    for svg in sorted(glob.glob(os.path.join(OUT, "flamegraphs", "*.svg"))):
        name = os.path.basename(svg)[:-4]
        cairosvg.svg2pdf(url=svg, write_to=os.path.join(FIGURES, f"flame_{name}.pdf"))


def main():
    conc = aggregate_concurrent()
    aggregate_reference()
    layout = aggregate_layout()
    aggregate_flamegraphs()
    aggregate_c2c()
    make_figures(conc, layout)
    print("summary written to", os.path.relpath(SUMMARY, ROOT), "and", os.path.relpath(FIGURES, ROOT))


if __name__ == "__main__":
    main()
