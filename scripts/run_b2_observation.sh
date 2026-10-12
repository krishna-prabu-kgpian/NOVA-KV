#!/usr/bin/env bash
# run_b2_observation.sh -- measurements of the complete Part B store (milestone
# B2: epoch-based reclamation enabled). An observation set, not the Part D
# campaign.
#
# Because the machine's power source and governor have differed between
# milestones, this script also re-measures reference points IN THE SAME SESSION:
#   * the B1 checkpoint (tag part-b-publication: same store, no reclamation and
#     no epoch announcement on reads), built from the tag into build/b1_reference;
#   * the Part A lock variants (unchanged code) from the current build.
# Only numbers from one session of this script are compared with each other.
#
#   scripts/run_b2_observation.sh        writes results/b2/   (about 10 minutes)
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

BUILD="${BUILD:-build/release}"
OUT="${OUT:-results/b2}"
REPS="${REPS:-3}"
READER_COUNTS="${READER_COUNTS:-1 2 4 8}"
READER_CPUS="${READER_CPUS:-0,2,4,6,1,3,5,7}"
WRITER_CPU="${WRITER_CPU:-15}"
PACED_RATE="${PACED_RATE:-10000}"
BATCHES="${BATCHES:-1 8 64 256 1024}"
B1_TAG="${B1_TAG:-part-b-publication}"
B1_DIR="build/b1_reference"
GIT_SHA="$(git rev-parse --short=12 HEAD)"
[ "$(nova_git_dirty)" = yes ] && GIT_SHA="${GIT_SHA}-dirty"

for b in concurrent_bench_rcu concurrent_bench_global_lock concurrent_bench_rwlock; do
    [ -x "$BUILD/$b" ] || { echo "$BUILD/$b missing: build the Release configuration first" >&2; exit 1; }
done
grep -q 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD/CMakeCache.txt" || { echo "$BUILD is not a Release build" >&2; exit 1; }
nova_resolve_cmake || exit 1

rm -rf "$OUT"; mkdir -p "$OUT/stdout" "$OUT/hist"
echo "building the B1 reference ($B1_TAG) in $B1_DIR ..."
rm -rf "$B1_DIR"; mkdir -p "$B1_DIR/src"
git archive "$B1_TAG" | tar -x -C "$B1_DIR/src" || exit 1
"$NOVA_CMAKE" -S "$B1_DIR/src" -B "$B1_DIR/build" -DCMAKE_BUILD_TYPE=Release > "$OUT/b1_reference_build.log" 2>&1 &&
    "$NOVA_CMAKE" --build "$B1_DIR/build" -j --target concurrent_bench_rcu >> "$OUT/b1_reference_build.log" 2>&1 ||
    { echo "B1 reference build failed, see $OUT/b1_reference_build.log" >&2; exit 1; }

scripts/env_info.sh > "$OUT/environment.txt" 2>&1
conditions() {
    printf '%s %s ac_online=%s governor=%s epp=%s loadavg=%s\n' "$(date +%Y-%m-%dT%H:%M:%S)" "$1" \
        "$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)" \
        "$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/conditions.log"
}
{
    echo "git_sha: $GIT_SHA   b1_reference: $B1_TAG ($(git rev-parse --short "$B1_TAG^{commit}"))"
    echo "reps=$REPS reader_cpus=$READER_CPUS writer_cpu=$WRITER_CPU paced_writer_rate=$PACED_RATE batches=$BATCHES"
    echo "windows: --warmup 0.5 --duration 3, except reclamation disabled and the B1 reference with an"
    echo "         unthrottled writer: --warmup 0.3 --duration 1 (they retain every version)"
    echo "cleaner thread: not pinned"
    echo "compile command (kvstore.cpp, rcu variant):"
    grep -o '"command": "[^"]*' "$BUILD/compile_commands.json" | grep 'NOVA_KV_SYNC_RCU' | grep 'kvstore.cpp' | head -1 | sed 's/"command": "/  /'
} > "$OUT/config.txt"

FAILURES=0
# run <binary> <csv-prefix> <variant-label> <scenario> <readers> <rep> <args...>
run() {
    local bin="$1" prefix="$2" label="$3" scenario="$4" readers="$5" rep="$6"; shift 6
    local id="${label}_${scenario}_r${readers}_rep${rep}"
    local cmd=("$bin" --readers "$readers" --reader-cpus "$(echo "$READER_CPUS" | cut -d, -f1-"$readers")"
               --seed "$rep" --run-id "$id" --git-sha "$GIT_SHA" --scenario "$scenario"
               --csv "$OUT/${prefix}runs.csv" --hist "$OUT/hist/$id.csv" "$@")
    { echo "# command: ${cmd[*]}"; "${cmd[@]}"; echo "# exit_status: $?"; } > "$OUT/stdout/$id.txt" 2>&1
    grep -q '^# exit_status: 0' "$OUT/stdout/$id.txt" || { echo "FAILED: $id"; FAILURES=$((FAILURES + 1)); }
}
RCU="$BUILD/concurrent_bench_rcu"
B1="$B1_DIR/build/concurrent_bench_rcu"
W="--writer on --writer-cpu $WRITER_CPU"

conditions start
for rep in $(seq 1 "$REPS"); do
    # (1) The Part B store, default batch (64): scaling with readers.
    for readers in $READER_COUNTS; do
        run "$RCU" "" rcu writer_paced "$readers" "$rep" --warmup 0.5 --duration 3 $W --writer-rate "$PACED_RATE" --cow-csv "$OUT/cow.csv"
        run "$RCU" "" rcu writer_max "$readers" "$rep" --warmup 0.5 --duration 3 $W --cow-csv "$OUT/cow.csv"
    done
    run "$RCU" "" rcu readonly 1 "$rep" --warmup 0.5 --duration 3 --writer off --cow-csv "$OUT/cow.csv"
    run "$RCU" "" rcu inline_writer 1 "$rep" --warmup 0.5 --duration 3 --writer inline:100 --cow-csv "$OUT/cow.csv"
    # (2) Reclaim-batch sweep at 8 readers, unthrottled writer; plus the two comparators.
    for batch in $BATCHES; do
        run "$RCU" "" rcu "batch_${batch}" 8 "$rep" --warmup 0.5 --duration 3 $W --reclaim-batch "$batch" --cow-csv "$OUT/cow.csv"
    done
    run "$RCU" "" rcu reclaim_disabled 8 "$rep" --warmup 0.3 --duration 1 $W --reclaim disabled --retain-limit-mb 12000 --cow-csv "$OUT/cow.csv"
    run "$RCU" "" rcu reclaim_stw 8 "$rep" --warmup 0.5 --duration 3 $W --reclaim stw --cow-csv "$OUT/cow.csv"
    # (3) Same-session references.
    for readers in 1 8; do
        run "$B1" "reference_b1_" b1 writer_paced "$readers" "$rep" --warmup 0.5 --duration 3 $W --writer-rate "$PACED_RATE"
        run "$B1" "reference_b1_" b1 writer_max "$readers" "$rep" --warmup 0.3 --duration 1 $W --retain-limit-mb 12000
    done
    run "$B1" "reference_b1_" b1 readonly 1 "$rep" --warmup 0.5 --duration 3 --writer off
    for variant in global_lock rwlock; do
        for readers in 1 8; do
            run "$BUILD/concurrent_bench_$variant" "reference_part_a_" "$variant" writer_paced "$readers" "$rep" --warmup 0.5 --duration 3 $W --writer-rate "$PACED_RATE"
            run "$BUILD/concurrent_bench_$variant" "reference_part_a_" "$variant" writer_max "$readers" "$rep" --warmup 0.5 --duration 3 $W
        done
    done
    run "$BUILD/concurrent_bench_single" "reference_part_a_" single readonly 1 "$rep" --warmup 0.5 --duration 3 --writer off
    conditions "rep$rep"
    echo "[$(date +%H:%M:%S)] repetition $rep/$REPS done"
done

{ echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
  if perf stat -e task-clock -- true >/dev/null 2>&1; then echo "perf: usable"; else echo "perf: NOT usable by this user (no profiling was done)"; fi; } > "$OUT/perf_availability.txt"

PY="$NOVA_ROOT/.tools/venv/bin/python"; [ -x "$PY" ] || PY=python3
"$PY" scripts/summarize_b2.py "$OUT" || FAILURES=$((FAILURES + 1))
echo "finished with $FAILURES failure(s)"
[ "$FAILURES" -eq 0 ]
