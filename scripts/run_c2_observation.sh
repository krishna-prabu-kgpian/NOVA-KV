#!/usr/bin/env bash
# run_c2_observation.sh -- preliminary measurements for milestone C2 (secondary
# index and index-nested-loop join on the rcu build). An observation set, NOT
# the Part D campaign: a few repetitions, one session, conditions recorded.
#
#   scripts/run_c2_observation.sh        writes results/c2/   (about 4 minutes)
#
# Every run validates its own results (benchmarks/join_bench.cpp) and a failed
# validation fails the script. Numbers from different sessions (other power
# source, governor or load) must not be compared with these.
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

BUILD="${BUILD:-build/release}"
OUT="${OUT:-results/c2}"
REPS="${REPS:-3}"
TRADES="${TRADES:-100000}"
INSTRUMENTS="${INSTRUMENTS:-50}"
THREAD_COUNTS="${THREAD_COUNTS:-1 2 4 8}"
READER_CPUS="${READER_CPUS:-0,2,4,6,1,3,5,7}"
WRITER_CPU="${WRITER_CPU:-15}"
DURATION="${DURATION:-2}"
GIT_SHA="$(git rev-parse --short=12 HEAD)"
[ "$(nova_git_dirty)" = yes ] && GIT_SHA="${GIT_SHA}-dirty"

BIN="$BUILD/join_bench"
[ -x "$BIN" ] || { echo "$BIN missing: build the Release configuration first" >&2; exit 1; }
grep -q 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD/CMakeCache.txt" || { echo "$BUILD is not a Release build" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/stdout"
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
    echo "reps=$REPS trades=$TRADES instruments=$INSTRUMENTS thread_counts=\"$THREAD_COUNTS\" duration=${DURATION}s"
    echo "thread affinity: reader/joiner thread i on CPU i-th of $READER_CPUS; writer on CPU $WRITER_CPU;"
    echo "                 the store's cleaner thread is not pinned"
    echo "tables: Trades = $TRADES records of 32 bytes, keys 0..N-1, symbol = key % $INSTRUMENTS;"
    echo "        Instruments = $INSTRUMENTS records of 20 bytes, one per symbol, indexed on symbol (8 bytes)"
    echo "join: benchmarks/join.h, one snapshot per store, exact_row_sizes = true (two lookups per record)"
    echo "writer during 'join --writer on': single Trade puts that change the symbol, and after every 64 of"
    echo "        them one transaction rewriting all Instruments"
    echo "latency sampling: index_get 1 call in 64; every join; every commit"
    echo "compile command (kvstore.cpp, rcu variant):"
    grep -o '"command": "[^"]*' "$BUILD/compile_commands.json" | grep 'NOVA_KV_SYNC_RCU' | grep 'kvstore.cpp' | head -1 | sed 's/"command": "/  /'
} > "$OUT/config.txt"

FAILURES=0
# run <id> <args...>
run() {
    local id="$1"; shift
    local cmd=("$BIN" --trades "$TRADES" --instruments "$INSTRUMENTS" --writer-cpu "$WRITER_CPU"
               --csv "$OUT/runs.csv" --run-id "$id" "$@")
    { echo "# command: ${cmd[*]}"; "${cmd[@]}"; echo "# exit_status: $?"; } > "$OUT/stdout/$id.txt" 2>&1
    grep -q '^# exit_status: 0' "$OUT/stdout/$id.txt" || { echo "FAILED: $id"; FAILURES=$((FAILURES + 1)); }
}
cpus() { echo "$READER_CPUS" | cut -d, -f1-"$1"; }

conditions start
for rep in $(seq 1 "$REPS"); do
    run "create_rep${rep}" --experiment create
    run "maintain_rep${rep}" --experiment maintain --duration "$DURATION"
    for size in 1 4 16; do
        run "txn${size}_indexed_rep${rep}" --experiment txn --txn-size "$size" --index on --duration "$DURATION"
        run "txn${size}_plain_rep${rep}" --experiment txn --txn-size "$size" --index off --duration "$DURATION"
    done
    for t in $THREAD_COUNTS; do
        run "lookup_t${t}_paused_rep${rep}" --experiment lookup --threads "$t" --cpus "$(cpus "$t")" --writer off --duration "$DURATION"
        run "lookup_t${t}_active_rep${rep}" --experiment lookup --threads "$t" --cpus "$(cpus "$t")" --writer on --duration "$DURATION"
        run "join_t${t}_paused_rep${rep}" --experiment join --threads "$t" --cpus "$(cpus "$t")" --writer off --duration "$DURATION"
        run "join_t${t}_active_rep${rep}" --experiment join --threads "$t" --cpus "$(cpus "$t")" --writer on --duration "$DURATION"
    done
    conditions "rep$rep"
    echo "[$(date +%H:%M:%S)] repetition $rep/$REPS done"
done

{ echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
  if perf stat -e task-clock -- true >/dev/null 2>&1; then echo "perf: usable"; else echo "perf: NOT usable by this user (no profiling was done)"; fi; } > "$OUT/perf_availability.txt"

PY="$NOVA_ROOT/.tools/venv/bin/python"; [ -x "$PY" ] || PY=python3
"$PY" scripts/summarize_c2.py "$OUT" || FAILURES=$((FAILURES + 1))
echo "finished with $FAILURES failure(s)"
[ "$FAILURES" -eq 0 ]
