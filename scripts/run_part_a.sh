#!/usr/bin/env bash
# run_part_a.sh -- runs the Part A measurement campaign and stores every raw
# output under results/part_a/. Nothing here is estimated or synthesized: each
# number in the Part A report is derived from files this script writes.
#
#   scripts/run_part_a.sh all            every stage below, in order
#   scripts/run_part_a.sh env            machine / toolchain record
#   scripts/run_part_a.sh concurrent     active-writer matrix under `perf stat`
#   scripts/run_part_a.sh reference      supplied generator (preload-only) runs
#   scripts/run_part_a.sh layout         NSM / DSM / PAX microbenchmark (+ perf stat)
#   scripts/run_part_a.sh flamegraphs    `perf record` + flame graphs, 8 configs
#   scripts/run_part_a.sh c2c            `perf c2c` cache-line contention reports, 8 configs
#
# Requirements: a Release build in build/release, perf usable without root,
# .tools/FlameGraph (scripts/bootstrap_tools.sh --profiling).
#
# CPU placement on the development machine (AMD Ryzen AI 7 350: logical CPUs
# N and N+8 are SMT siblings; cores 0,2,4,6 are the fast cores, 1,3,5,7 the
# slower ones). Readers take one logical CPU per physical core, fast cores
# first; the writer always runs on CPU 15, the SMT sibling of core 7, so it has
# a physical core to itself for 1/2/4 readers and shares core 7 with the eighth
# reader. Override with READER_CPUS / WRITER_CPU on other machines.
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

BUILD="${BUILD:-build/release}"
OUT="${OUT:-results/part_a}"
REPS="${REPS:-5}"
DURATION="${DURATION:-3}"
WARMUP="${WARMUP:-0.5}"
REF_DURATION="${REF_DURATION:-2}"
READER_COUNTS="${READER_COUNTS:-1 2 4 8}"
READER_CPUS="${READER_CPUS:-0,2,4,6,1,3,5,7}"
WRITER_CPU="${WRITER_CPU:-15}"
PACED_RATE="${PACED_RATE:-10000}"
PERF_EVENTS="${PERF_EVENTS:-task-clock,cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations}"
PERF_FREQ="${PERF_FREQ:-1999}"
FG="${FLAMEGRAPH_DIR:-$NOVA_ROOT/.tools/FlameGraph}"
GIT_SHA="$(git rev-parse --short=12 HEAD)"
[ "$(nova_git_dirty)" = yes ] && GIT_SHA="${GIT_SHA}-dirty"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { echo "run_part_a.sh: $*" >&2; exit 1; }

need_build() {
    for b in concurrent_bench_single concurrent_bench_global_lock concurrent_bench_rwlock \
             layout_bench single_thread global_lock rwlock; do
        [ -x "$BUILD/$b" ] || die "$BUILD/$b missing: build the Release configuration first"
    done
    grep -q 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD/CMakeCache.txt" || die "$BUILD is not a Release build"
}

reader_cpus_for() { echo "$READER_CPUS" | cut -d, -f1-"$1"; }

stage_env() {
    mkdir -p "$OUT"
    scripts/env_info.sh > "$OUT/environment.txt" 2>&1
    {
        echo "git_sha: $GIT_SHA"
        echo "build_dir: $BUILD"
        echo "compile flags (from $BUILD/compile_commands.json, kvstore.cpp of the rwlock variant):"
        grep -o '"command": "[^"]*' "$BUILD/compile_commands.json" | grep 'NOVA_KV_SYNC_RWLOCK' | grep 'kvstore.cpp' | head -1 | sed 's/"command": "/  /'
        echo "reps=$REPS duration_s=$DURATION warmup_s=$WARMUP reference_duration_s=$REF_DURATION"
        echo "reader_cpus=$READER_CPUS writer_cpu=$WRITER_CPU paced_writer_rate=$PACED_RATE"
        echo "perf_events=$PERF_EVENTS perf_record_freq_hz=$PERF_FREQ"
    } > "$OUT/campaign_config.txt"
    log "environment recorded in $OUT/environment.txt"
}

# run_concurrent <variant> <scenario> <readers> <rep> <extra bench args...>
# One measured process, wrapped in perf stat; appends one CSV row.
run_concurrent() {
    local variant="$1" scenario="$2" readers="$3" rep="$4"; shift 4
    local id="${variant}_${scenario}_r${readers}_rep${rep}"
    local dir="$OUT/concurrent"
    mkdir -p "$dir/perf_stat" "$dir/hist" "$dir/stdout"
    local cmd=("$BUILD/concurrent_bench_${variant}" --readers "$readers" --duration "$DURATION"
               --warmup "$WARMUP" --reader-cpus "$(reader_cpus_for "$readers")" --seed "$rep"
               --run-id "$id" --git-sha "$GIT_SHA" --scenario "$scenario"
               --csv "$dir/runs.csv" --hist "$dir/hist/$id.csv" "$@")
    {
        echo "# command: perf stat -e $PERF_EVENTS -o $dir/perf_stat/$id.txt -- ${cmd[*]}"
        perf stat -e "$PERF_EVENTS" -o "$dir/perf_stat/$id.txt" -- "${cmd[@]}"
        echo "# exit_status: $?"
    } > "$dir/stdout/$id.txt" 2>&1
    local rc; rc="$(tail -1 "$dir/stdout/$id.txt" | sed 's/.*: //')"
    [ "$rc" = 0 ] || { log "FAILED: $id (exit $rc), see $dir/stdout/$id.txt"; FAILURES=$((FAILURES + 1)); }
}

stage_concurrent() {
    need_build
    rm -rf "$OUT/concurrent"
    log "concurrent matrix: 2 lock variants x {$READER_COUNTS} readers x 2 writer modes x $REPS reps"
    # Repetitions are the outer loop so that drift over the campaign is spread
    # over all configurations instead of biasing the later ones.
    for rep in $(seq 1 "$REPS"); do
        for variant in global_lock rwlock; do
            for readers in $READER_COUNTS; do
                run_concurrent "$variant" writer_max "$readers" "$rep" --writer on --writer-cpu "$WRITER_CPU"
                run_concurrent "$variant" writer_paced "$readers" "$rep" --writer on --writer-cpu "$WRITER_CPU" --writer-rate "$PACED_RATE"
            done
        done
        # Unsynchronized store: one thread only (never run concurrently).
        run_concurrent single readonly 1 "$rep" --writer off
        run_concurrent single inline_writer 1 "$rep" --writer inline:100
        log "  repetition $rep/$REPS done"
    done
}

stage_reference() {
    need_build
    local dir="$OUT/reference"
    rm -rf "$dir"; mkdir -p "$dir"
    echo "run_id,driver,readers,duration_s,rep,total_ops,rps,p50_us,p99_us,p999_us" > "$dir/runs.csv"
    log "supplied generator (PRELOAD-ONLY: no writer runs while readers are measured)"
    for rep in $(seq 1 "$REPS"); do
        for driver in single_thread global_lock rwlock; do
            local counts="$READER_COUNTS"
            [ "$driver" = single_thread ] && counts=1
            for readers in $counts; do
                local id="${driver}_r${readers}_rep${rep}" line
                {
                    echo "# command: taskset -c $READER_CPUS $BUILD/$driver $readers $REF_DURATION"
                    taskset -c "$READER_CPUS" "$BUILD/$driver" "$readers" "$REF_DURATION"
                    echo "# exit_status: $?"
                } > "$dir/$id.txt" 2>&1
                line="$(grep -E 'ops=' "$dir/$id.txt" | tail -1)"
                [ -n "$line" ] || { log "FAILED: $id"; FAILURES=$((FAILURES + 1)); continue; }
                echo "$line" | sed -E "s/.*ops=([0-9]+) +rps=([0-9.]+) +p50=([0-9.]+) *us p99=([0-9.]+) *us p999=([0-9.]+) *us.*/$id,$driver,$readers,$REF_DURATION,$rep,\1,\2,\3,\4,\5/" >> "$dir/runs.csv"
            done
        done
        log "  repetition $rep/$REPS done"
    done
}

stage_layout() {
    need_build
    local dir="$OUT/layout"
    rm -rf "$dir"; mkdir -p "$dir/perf_stat"
    local cpu="${READER_CPUS%%,*}"
    for records in 100000 8000000; do
        log "layout microbenchmark: $records records, all layouts and patterns, 7 reps"
        "$BUILD/layout_bench" --records "$records" --ops 4000000 --reps 7 --cpu "$cpu" \
            --run-id "layout_$records" --csv "$dir/runs.csv" > "$dir/stdout_$records.txt" 2>&1 \
            || { log "FAILED: layout $records"; FAILURES=$((FAILURES + 1)); }
        # Hardware counters per layout. The process also builds the tables and
        # the access sequences, so a "none" run (setup only) is recorded as the
        # baseline to subtract.
        for layout in nsm dsm pax; do
            for pattern in none row_uniform row_chain; do
                local id="${layout}_${pattern}_${records}"
                perf stat -e "$PERF_EVENTS" -o "$dir/perf_stat/$id.txt" -- \
                    "$BUILD/layout_bench" --records "$records" --ops 4000000 --reps 20 --cpu "$cpu" \
                    --layout "$layout" --pattern "$pattern" > /dev/null 2>&1 \
                    || { log "FAILED: perf stat $id"; FAILURES=$((FAILURES + 1)); }
            done
        done
    done
}

stage_flamegraphs() {
    need_build
    [ -x "$FG/stackcollapse-perf.pl" ] && [ -x "$FG/flamegraph.pl" ] \
        || die "FlameGraph scripts not found in $FG (scripts/bootstrap_tools.sh --profiling)"
    local dir="$OUT/flamegraphs" tmp="build/perf_data"
    rm -rf "$dir"; mkdir -p "$dir" "$tmp"
    {
        echo "FlameGraph: https://github.com/brendangregg/FlameGraph @ $(git -C "$FG" rev-parse HEAD 2>/dev/null)"
        echo "perf: $(perf --version)"
        echo "kptr_restrict: $(cat /proc/sys/kernel/kptr_restrict)"
        echo
        echo "Per configuration <id> = <variant>_r<readers>:"
        echo "  perf record -F $PERF_FREQ --call-graph fp -o $tmp/<id>.data -- $BUILD/concurrent_bench_<variant> --readers <R> --duration 5 --warmup $WARMUP --reader-cpus <cpus> --writer on --writer-cpu $WRITER_CPU"
        echo "  DEBUGINFOD_URLS= perf script --no-inline -i $tmp/<id>.data > <id>.perf-script.txt   (kept as <id>.perf-script.txt.xz)"
        echo "  sed -E 's/^(\\s+)(ffffffff[0-9a-f]+) \\[unknown\\] \\(\\[unknown\\]\\)/\\1\\2 [kernel] ([kernel])/'   (label unresolved kernel frames)"
        echo "  stackcollapse-perf.pl | perl -pe 's/(\\[kernel\\];)+/[kernel];/g; s/(;\\[kernel\\])+ /;[kernel] /'  > <id>.folded"
        echo "  flamegraph.pl --width 1400 --countname cycles --title ... <id>.folded > <id>.svg"
        echo
        echo "Samples are taken on the cycles event, and stackcollapse-perf.pl weights each stack by"
        echo "its sample period, so box widths are shares of CPU cycles (on-CPU time only: a thread"
        echo "blocked in the kernel waiting for a lock consumes no cycles and is not sampled)."
        echo
        echo "kptr_restrict is nonzero, so kernel symbols are not available to an unprivileged"
        echo "perf: every kernel frame appears as an address with symbol [unknown]. The sed/perl"
        echo "steps only rename those frames to [kernel] and merge consecutive ones into a single"
        echo "frame; no sample is added, removed or reattributed. The raw perf script output is kept."
    } > "$dir/COMMANDS.txt"
    for variant in global_lock rwlock; do
        for readers in $READER_COUNTS; do
            local id="${variant}_r${readers}"
            log "perf record: $id"
            perf record -F "$PERF_FREQ" --call-graph fp -o "$tmp/$id.data" -- \
                "$BUILD/concurrent_bench_${variant}" --readers "$readers" --duration 5 --warmup "$WARMUP" \
                --reader-cpus "$(reader_cpus_for "$readers")" --writer on --writer-cpu "$WRITER_CPU" \
                --run-id "perf_record_$id" --git-sha "$GIT_SHA" --scenario writer_max_profiled \
                --csv "$dir/profiled_runs.csv" > "$dir/$id.record.log" 2>&1 \
                || { log "FAILED: perf record $id"; FAILURES=$((FAILURES + 1)); continue; }
            perf script --no-inline -i "$tmp/$id.data" > "$tmp/$id.perf-script.txt" 2> "$dir/$id.script.log" \
                || { log "FAILED: perf script $id"; FAILURES=$((FAILURES + 1)); continue; }
            sed -E 's/^(\s+)(ffffffff[0-9a-f]+) \[unknown\] \(\[unknown\]\)/\1\2 [kernel] ([kernel])/' "$tmp/$id.perf-script.txt" \
                | "$FG/stackcollapse-perf.pl" 2>> "$dir/$id.script.log" \
                | perl -pe 's/(\[kernel\];)+/[kernel];/g; s/(;\[kernel\])+ /;[kernel] /' \
                | sort | awk '{c=$NF; $NF=""; sub(/ $/,""); a[$0]+=c} END{for(k in a) print k, a[k]}' | sort \
                > "$dir/$id.folded"
            "$FG/flamegraph.pl" --width 1400 --countname cycles --title "NOVA-KV Part A: $variant, $readers reader(s) + 1 writer (on-CPU samples)" \
                "$dir/$id.folded" > "$dir/$id.svg" 2>> "$dir/$id.script.log"
            xz -9 -T0 -c "$tmp/$id.perf-script.txt" > "$dir/$id.perf-script.txt.xz"
            echo "$id: $(grep -c 'cycles' "$tmp/$id.perf-script.txt") samples, $(awk '{s+=$NF} END{print s}' "$dir/$id.folded") cycles" >> "$dir/sample_counts.txt"
        done
    done
}

# perf consults debuginfod (a network service) for every binary it meets when
# DEBUGINFOD_URLS is set, which makes `perf script` take minutes instead of a
# fraction of a second. Symbols for our own binaries come from their -g build.
export DEBUGINFOD_URLS=

# perf c2c (cache-to-cache): samples memory accesses (AMD IBS / Intel PEBS) and
# reports cache lines on which loads hit a line held Modified by another core
# (HITM). This is the coherence-level evidence for cache-line bouncing that a
# flame graph cannot give.
stage_c2c() {
    need_build
    local dir="$OUT/c2c" tmp="build/perf_data"
    rm -rf "$dir"; mkdir -p "$dir" "$tmp"
    for variant in global_lock rwlock; do
        for readers in $READER_COUNTS; do
            local id="${variant}_r${readers}"
            log "perf c2c: $id"
            {
                echo "# command: perf c2c record -o $tmp/$id.c2c.data -- $BUILD/concurrent_bench_${variant} --readers $readers --duration 5 --warmup $WARMUP --reader-cpus $(reader_cpus_for "$readers") --writer on --writer-cpu $WRITER_CPU"
                perf c2c record -o "$tmp/$id.c2c.data" -- \
                    "$BUILD/concurrent_bench_${variant}" --readers "$readers" --duration 5 --warmup "$WARMUP" \
                    --reader-cpus "$(reader_cpus_for "$readers")" --writer on --writer-cpu "$WRITER_CPU" \
                    --run-id "perf_c2c_$id" --git-sha "$GIT_SHA" --scenario writer_max_c2c
                echo "# exit_status: $?"
            } > "$dir/$id.record.log" 2>&1
            grep -q '^# exit_status: 0' "$dir/$id.record.log" \
                || { log "FAILED: perf c2c record $id"; FAILURES=$((FAILURES + 1)); continue; }
            perf c2c report -i "$tmp/$id.c2c.data" --stdio > "$dir/$id.c2c-report.txt" 2> "$dir/$id.report.log" \
                || { log "FAILED: perf c2c report $id"; FAILURES=$((FAILURES + 1)); }
        done
    done
}

# Power source, load and CPU frequency at a point in the campaign, so a change
# of conditions between stages is visible in the record.
conditions() {
    {
        printf '%s stage=%s ' "$(date +%Y-%m-%dT%H:%M:%S)" "$1"
        printf 'ac_online=%s ' "$(cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1)"
        printf 'governor=%s epp=%s ' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" \
            "$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)"
        printf 'loadavg=%s\n' "$(cut -d' ' -f1-3 /proc/loadavg)"
    } >> "$OUT/conditions.log"
}

FAILURES=0
stage="${1:-all}"
mkdir -p "$OUT"
run_stage() { conditions "$1:start"; "stage_$1"; conditions "$1:end"; }
case "$stage" in
    env) stage_env ;;
    concurrent|reference|layout|flamegraphs|c2c) run_stage "$stage" ;;
    all)
        rm -f "$OUT/conditions.log"
        stage_env
        for st in concurrent reference layout flamegraphs c2c; do run_stage "$st"; done ;;
    *) die "unknown stage '$stage'" ;;
esac
log "stage '$stage' finished with $FAILURES failure(s)"
[ "$FAILURES" -eq 0 ]
