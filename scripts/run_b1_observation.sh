#!/usr/bin/env bash
# run_b1_observation.sh -- a SMALL set of measurements of the Part B store at
# milestone B1 (blueprint 8.5: "record root-copy/segment-copy bytes per write
# and index lookup latency"). This is not the Part D campaign.
#
# Same harness, dataset, read mix, thread pinning and build flags as the Part A
# campaign (scripts/run_part_a.sh), so the numbers can be put next to
# results/part_a/. Differences forced by B1, which does not reclaim replaced
# versions: the unthrottled-writer window is 1 s instead of 3 s (it retains
# roughly 2-3 GiB per second), and the harness invalidates a run whose retained
# memory passes the limit.
#
#   scripts/run_b1_observation.sh        writes results/b1/
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

BUILD="${BUILD:-build/release}"
OUT="${OUT:-results/b1}"
REPS="${REPS:-3}"
READER_COUNTS="${READER_COUNTS:-1 2 4 8}"
READER_CPUS="${READER_CPUS:-0,2,4,6,1,3,5,7}"
WRITER_CPU="${WRITER_CPU:-15}"
PACED_RATE="${PACED_RATE:-10000}"
GIT_SHA="$(git rev-parse --short=12 HEAD)"
[ "$(nova_git_dirty)" = yes ] && GIT_SHA="${GIT_SHA}-dirty"
BIN="$BUILD/concurrent_bench_rcu"

[ -x "$BIN" ] || { echo "$BIN missing: build the Release configuration first" >&2; exit 1; }
grep -q 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD/CMakeCache.txt" || { echo "$BUILD is not a Release build" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/stdout" "$OUT/hist"
scripts/env_info.sh > "$OUT/environment.txt" 2>&1
conditions() {
    printf '%s %s ac_online=%s governor=%s epp=%s loadavg=%s\n' "$(date +%Y-%m-%dT%H:%M:%S)" "$1" \
        "$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)" \
        "$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/conditions.log"
}
{
    echo "git_sha: $GIT_SHA"
    echo "reps=$REPS reader_cpus=$READER_CPUS writer_cpu=$WRITER_CPU paced_writer_rate=$PACED_RATE"
    echo "writer_paced: --warmup 0.5 --duration 3    writer_max: --warmup 0.3 --duration 1"
    echo "compile command (kvstore.cpp, rcu variant):"
    grep -o '"command": "[^"]*' "$BUILD/compile_commands.json" | grep 'NOVA_KV_SYNC_RCU' | grep 'kvstore.cpp' | head -1 | sed 's/"command": "/  /'
} > "$OUT/config.txt"

FAILURES=0
run() { # <scenario> <readers> <rep> <args...>
    local scenario="$1" readers="$2" rep="$3"; shift 3
    local id="rcu_${scenario}_r${readers}_rep${rep}"
    local cmd=("$BIN" --readers "$readers" --reader-cpus "$(echo "$READER_CPUS" | cut -d, -f1-"$readers")"
               --seed "$rep" --run-id "$id" --git-sha "$GIT_SHA" --scenario "$scenario"
               --csv "$OUT/runs.csv" --cow-csv "$OUT/cow.csv" --hist "$OUT/hist/$id.csv" "$@")
    { echo "# command: ${cmd[*]}"; "${cmd[@]}"; echo "# exit_status: $?"; } > "$OUT/stdout/$id.txt" 2>&1
    grep -q '^# exit_status: 0' "$OUT/stdout/$id.txt" || { echo "FAILED: $id"; FAILURES=$((FAILURES + 1)); }
}

conditions start
for rep in $(seq 1 "$REPS"); do
    for readers in $READER_COUNTS; do
        run writer_paced "$readers" "$rep" --warmup 0.5 --duration 3 --writer on --writer-cpu "$WRITER_CPU" --writer-rate "$PACED_RATE"
        run writer_max "$readers" "$rep" --warmup 0.3 --duration 1 --writer on --writer-cpu "$WRITER_CPU"
    done
    # One thread, no concurrent writer: the cost of the read path alone, and
    # with the same thread writing (replaced versions released at once).
    run readonly 1 "$rep" --warmup 0.5 --duration 3 --writer off
    run inline_writer 1 "$rep" --warmup 0.5 --duration 3 --writer inline:100
    echo "[$(date +%H:%M:%S)] repetition $rep/$REPS done"
done
conditions end

PY="$NOVA_ROOT/.tools/venv/bin/python"; [ -x "$PY" ] || PY=python3
"$PY" scripts/summarize_b1.py "$OUT" || FAILURES=$((FAILURES + 1))
echo "finished with $FAILURES failure(s)"
[ "$FAILURES" -eq 0 ]
