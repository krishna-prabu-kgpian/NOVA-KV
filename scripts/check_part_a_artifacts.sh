#!/usr/bin/env bash
# Presence check for every Part A deliverable the assignment lists (PDF pp. 2-3)
# and for the raw data behind the report. It checks that artifacts exist and are
# non-empty; it does not judge their content.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"
R=results/part_a
status=0
need() { if [ -s "$1" ]; then echo "ok       $1"; else echo "MISSING  $1"; status=1; fi; }
need_count() { # <glob> <expected count> <description>
    local n; n="$(ls $1 2>/dev/null | wc -l)"
    if [ "$n" -ge "$2" ]; then echo "ok       $n x $3"; else echo "MISSING  $3: found $n, need $2"; status=1; fi
}

echo "-- store, sanity test, drivers"
for f in include/kvstore.h src/kvstore.cpp tests/kvstore_test.cpp drivers/single_thread.cpp \
         drivers/global_lock.cpp drivers/rwlock.cpp supplied/workload_gen.cpp supplied/workload_gen.h; do need "$f"; done
echo "-- benchmark harness and layout microbenchmark"
need benchmarks/concurrent_bench.cpp; need benchmarks/layout_bench.cpp
echo "-- raw measurements"
need $R/environment.txt; need $R/campaign_config.txt; need $R/conditions.log
need $R/concurrent/runs.csv; need $R/reference/runs.csv; need $R/layout/runs.csv
echo "-- perf stat: global_lock and rwlock at 1/2/4/8 readers, writer active"
for v in global_lock rwlock; do for r in 1 2 4 8; do
    need_count "$R/concurrent/perf_stat/${v}_writer_max_r${r}_rep*.txt" 5 "perf stat ${v} r${r} (writer_max)"
done; done
echo "-- perf record / perf script / flame graphs: global_lock and rwlock at 1/2/4/8 readers"
for v in global_lock rwlock; do for r in 1 2 4 8; do
    need "$R/flamegraphs/${v}_r${r}.svg"; need "$R/flamegraphs/${v}_r${r}.folded"
    need "$R/flamegraphs/${v}_r${r}.perf-script.txt.xz"
done; done
need $R/flamegraphs/COMMANDS.txt
echo "-- perf c2c cache-line contention reports"
for v in global_lock rwlock; do for r in 1 2 4 8; do need "$R/c2c/${v}_r${r}.c2c-report.txt"; done; done
echo "-- layout hardware counters"
need_count "$R/layout/perf_stat/*.txt" 18 "layout perf stat files"
echo "-- summaries, figures, report"
for f in concurrent_summary.csv reference_summary.csv layout_summary.csv layout_perf_summary.csv flamegraph_breakdown.csv c2c_summary.csv; do need "$R/summary/$f"; done
need $R/figures/fig_scaling.pdf; need $R/figures/fig_layout.pdf
need reports/part_a_report.pdf; need reports/part_a_report.md; need docs/part_a_compliance.md
[ $status -eq 0 ] && echo "all Part A artifacts present"
exit $status
