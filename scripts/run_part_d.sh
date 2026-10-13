#!/usr/bin/env bash
# run_part_d.sh -- the Part D measurement campaign. Every number in the Part D
# report is derived from files this script writes under results/part_d/.
#
#   scripts/run_part_d.sh check       prerequisites only (changes nothing)
#   scripts/run_part_d.sh pilot       a small run of every stage into results/part_d_pilot/
#   scripts/run_part_d.sh all         the full campaign into results/part_d/ (about 40 minutes)
#   scripts/run_part_d.sh <stage>     one stage of an existing campaign:
#                                     d1_concurrent d1_reference d2_steady d2_backlog
#                                     d3_txn d3_join d3_cow d3_generator flamegraphs c2c
#
# TWO TRACKS, never mixed in one table:
#   track R  the UNMODIFIED supplied generator (drivers/): its writer finishes
#            before any reader starts, so these are PRELOAD-ONLY numbers, and
#            most of its random reads miss;
#   track C  benchmarks/concurrent_bench.cpp: one writer publishing during the
#            measured window, every read validated.
#
# VALIDITY RULES enforced here (docs/part_d_plan.md section 6):
#   - Release build with -O3 and frame pointers only;
#   - mains power and the `performance` governor at the start, after every
#     repetition and at the end: any change marks the campaign INVALID;
#   - the machine must be idle when the campaign starts;
#   - every run records its command line and exit status; a run that fails its
#     own validation fails the campaign;
#   - `all` and `pilot` start from an empty output directory and stamp every
#     stage with the campaign id; scripts/aggregate_part_d.py refuses a
#     directory whose stages carry different ids.
#
# CPU placement (AMD Ryzen AI 7 350: logical CPUs N and N+8 are SMT siblings;
# cores 0,2,4,6 are the fast cores, 1,3,5,7 the slower ones): readers and
# joiners take one logical CPU per physical core, fast cores first; the writer
# runs on CPU 15 (sibling of core 7: a physical core to itself up to 4 readers,
# shared with the eighth); the store's cleaner thread is not pinned.
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

MODE="${1:-check}"
BUILD="${BUILD:-build/release}"
if [ "$MODE" = pilot ]; then
    OUT="${OUT:-results/part_d_pilot}"
    REPS="${REPS:-1}"
    DURATION="${DURATION:-1}"
    REF_DURATION="${REF_DURATION:-1}"
    SHORT="${SHORT:-1}"
    READER_COUNTS="${READER_COUNTS:-8}"
    BATCHES="${BATCHES:-64}"
    TXN_SIZES="${TXN_SIZES:-4}"
    JOIN_THREADS="${JOIN_THREADS:-8}"
else
    OUT="${OUT:-results/part_d}"
    REPS="${REPS:-5}"
    DURATION="${DURATION:-5}"          # D.1 measured window, seconds
    REF_DURATION="${REF_DURATION:-5}"  # the generator's default duration_sec
    SHORT="${SHORT:-3}"                # D.2 / D.3 measured window, seconds
    READER_COUNTS="${READER_COUNTS:-1 2 4 8}"
    BATCHES="${BATCHES:-1 8 64 256 1024}"
    TXN_SIZES="${TXN_SIZES:-1 4 16}"
    JOIN_THREADS="${JOIN_THREADS:-1 2 4 8}"
fi
WARMUP="${WARMUP:-0.5}"
READER_CPUS="${READER_CPUS:-0,2,4,6,1,3,5,7}"
WRITER_CPU="${WRITER_CPU:-15}"
PACED_RATE="${PACED_RATE:-10000}"
PERF_EVENTS="${PERF_EVENTS:-task-clock,cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations,branch-misses,L1-dcache-load-misses}"
PERF_FREQ="${PERF_FREQ:-1999}"
FG="${FLAMEGRAPH_DIR:-$NOVA_ROOT/.tools/FlameGraph}"
MIN_IDLE_PCT="${MIN_IDLE_PCT:-97}"
RETAIN_LIMIT_MB="${RETAIN_LIMIT_MB:-16000}"   # the writer stops (and the run is INVALID) above this
GIT_SHA="$(git rev-parse --short=12 HEAD)"
[ "$(nova_git_dirty)" = yes ] && GIT_SHA="${GIT_SHA}-dirty"
export DEBUGINFOD_URLS=     # or `perf script` spends minutes on network lookups

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { echo "run_part_d.sh: $*" >&2; exit 1; }
FAILURES=0
fail() { log "FAILED: $*"; FAILURES=$((FAILURES + 1)); }
reader_cpus_for() { echo "$READER_CPUS" | cut -d, -f1-"$1"; }

BINARIES="concurrent_bench_single concurrent_bench_global_lock concurrent_bench_rwlock concurrent_bench_rcu
          join_bench reclaim_bench single_thread global_lock rwlock rcu"

# ---- Conditions ------------------------------------------------------------------
ac_online() { cat /sys/class/power_supply/AC*/online 2>/dev/null | head -1; }
governors() { cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ' | sed 's/ $//'; }
# Percentage of CPU time spent idle over a sample of $1 seconds, all CPUs.
idle_pct() {
    local a b
    a=($(head -1 /proc/stat)); sleep "$1"; b=($(head -1 /proc/stat))
    local t1=0 t2=0 i
    for i in 1 2 3 4 5 6 7 8; do t1=$((t1 + ${a[$i]})); t2=$((t2 + ${b[$i]})); done
    echo $(( 100 * (${b[4]} - ${a[4]}) / (t2 - t1) ))
}
conditions() {
    printf '%s %s ac_online=%s governor=%s epp=%s loadavg=%s battery=%s/%s%% temp=%sC mem_available_mb=%s\n' \
        "$(date +%Y-%m-%dT%H:%M:%S)" "$1" "$(ac_online)" "$(governors)" \
        "$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null)" \
        "$(cut -d' ' -f1-3 /proc/loadavg)" \
        "$(cat /sys/class/power_supply/BAT*/status 2>/dev/null | head -1)" \
        "$(cat /sys/class/power_supply/BAT*/capacity 2>/dev/null | head -1)" \
        "$(( $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0) / 1000 ))" \
        "$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)" >> "$OUT/conditions.log"
    # Any change of power source or governor invalidates the campaign.
    if [ "$(ac_online)" != 1 ] || [ "$(governors)" != performance ]; then
        echo "$(date +%Y-%m-%dT%H:%M:%S) $1: power source or governor changed" >> "$OUT/INVALID"
        log "CONDITIONS CHANGED at $1: campaign marked INVALID"
    fi
}

# ---- Prerequisites: nothing is measured unless all of these hold ----------------
check_prerequisites() {
    local ok=1 report="${1:-/dev/stdout}"
    {
        echo "prerequisite check, $(date +%Y-%m-%dT%H:%M:%S), git $GIT_SHA"
        chk() { if [ "$2" = ok ]; then echo "  ok      $1: $3"; else echo "  FAILED  $1: $3"; ok=0; fi; }
        [ "$(ac_online)" = 1 ] && chk "mains power" ok "AC online" || chk "mains power" no "AC online=$(ac_online)"
        [ "$(governors)" = performance ] && chk "governor" ok "performance on all CPUs" || chk "governor" no "$(governors)"
        local par; par="$(cat /proc/sys/kernel/perf_event_paranoid)"
        [ "$par" -le 1 ] && chk "perf_event_paranoid" ok "$par" || chk "perf_event_paranoid" no "$par (needs <= 1)"
        # A short busy loop: `true` exits before every event has been scheduled.
        if perf stat -e "$PERF_EVENTS" -- sh -c 'i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done' > /dev/null 2> "$OUT/.perf_probe"; then
            if grep -q -E 'not supported|not counted' "$OUT/.perf_probe"; then
                chk "perf stat events" no "$(grep -E 'not supported|not counted' "$OUT/.perf_probe" | head -2 | tr -s ' ' | tr '\n' ';')"
            else chk "perf stat events" ok "$PERF_EVENTS"; fi
        else chk "perf stat" no "perf stat failed"; fi
        if perf record -F 99 --call-graph fp -o "$OUT/.perf_probe.data" -- true > /dev/null 2>&1; then
            chk "perf record" ok "works"; else chk "perf record" no "perf record failed"; fi
        if perf c2c record -o "$OUT/.perf_probe.data" -- true > /dev/null 2>&1; then
            chk "perf c2c" ok "works"; else chk "perf c2c" no "perf c2c record failed (cache-line evidence would be missing)"; fi
        rm -f "$OUT/.perf_probe" "$OUT/.perf_probe.data"
        [ -x "$FG/stackcollapse-perf.pl" ] && [ -x "$FG/flamegraph.pl" ] && chk "FlameGraph" ok "$FG" || chk "FlameGraph" no "not in $FG"
        [ -x /usr/bin/time ] && chk "/usr/bin/time" ok present || chk "/usr/bin/time" no missing
        local mem; mem="$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)"
        [ "$mem" -ge 12000 ] && chk "memory available" ok "${mem} MiB" || chk "memory available" no "${mem} MiB (needs 12000)"
        local disk; disk="$(df -Pm . | awk 'NR==2 {print $4}')"
        [ "$disk" -ge 5000 ] && chk "disk free" ok "${disk} MiB" || chk "disk free" no "${disk} MiB (needs 5000)"
        local ncpu; ncpu="$(nproc)"
        [ "$ncpu" -ge 16 ] && chk "logical CPUs" ok "$ncpu" || chk "logical CPUs" no "$ncpu (the CPU placement assumes 16)"
        # Idleness: actual CPU time, not the load average.
        local idle; idle="$(idle_pct 5)"
        [ "$idle" -ge "$MIN_IDLE_PCT" ] && chk "machine idle" ok "${idle}% idle over 5 s" || chk "machine idle" no "${idle}% idle over 5 s (needs ${MIN_IDLE_PCT}%)"
        local busy; busy="$(ps -eo pcpu,comm --sort=-pcpu --no-headers | awk '$1 >= 15.0 && $2 != "ps" {print $2 "=" $1 "%"}' | head -5 | tr '\n' ' ')"
        [ -z "$busy" ] && chk "no busy process" ok "none above 15% CPU" || chk "no busy process" no "$busy"
        local other; other="$(pgrep -fa 'cc1plus|ctest|verify_all|concurrent_bench|join_bench|reclaim_bench|run_part_' | grep -v -E "pgrep|run_part_d.sh" | head -3 | cut -c1-80 | tr '\n' ';')"
        [ -z "$other" ] && chk "no build / test / benchmark running" ok none || chk "no build / test / benchmark running" no "$other"
        echo "  load average: $(cut -d' ' -f1-3 /proc/loadavg)   top CPU users: $(ps -eo pcpu,comm --sort=-pcpu --no-headers | head -4 | awk '{printf "%s=%s%% ", $2, $1}')"
    } > "$report" 2>&1
    [ "$report" != /dev/stdout ] && cat "$report"
    return $((1 - ok))
}

build_release() {
    nova_resolve_cmake || die "cmake not found"
    log "building the Release configuration in $BUILD"
    "$NOVA_CMAKE" -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > "$OUT/env/build.log" 2>&1 &&
        "$NOVA_CMAKE" --build "$BUILD" -j >> "$OUT/env/build.log" 2>&1 || die "build failed, see $OUT/env/build.log"
    grep -q 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD/CMakeCache.txt" || die "$BUILD is not a Release build"
    local b
    for b in $BINARIES; do [ -x "$BUILD/$b" ] || die "$BUILD/$b missing"; done
    local flags; flags="$(grep -o '"command": "[^"]*' "$BUILD/compile_commands.json" | grep 'NOVA_KV_SYNC_RCU' | grep 'kvstore.cpp' | head -1)"
    echo "$flags" | grep -q -- '-O3' || die "the build is not -O3"
    echo "$flags" | grep -q -- '-fno-omit-frame-pointer' || die "the build has no frame pointers"
    echo "$flags" | grep -q -E 'fsanitize' && die "the build has a sanitizer enabled"
    {
        echo "campaign: $CAMPAIGN"
        echo "git_sha: $GIT_SHA"
        echo "mode: $MODE"
        echo "build_dir: $BUILD (CMAKE_BUILD_TYPE=Release)"
        echo "compile command (kvstore.cpp, rcu variant):"
        echo "$flags" | sed 's/"command": "/  /'
        echo "reps=$REPS d1_duration_s=$DURATION reference_duration_s=$REF_DURATION d2_d3_duration_s=$SHORT warmup_s=$WARMUP"
        echo "reader_counts=\"$READER_COUNTS\" reclaim_batches=\"$BATCHES\" txn_sizes=\"$TXN_SIZES\" join_threads=\"$JOIN_THREADS\""
        echo "reader_cpus=$READER_CPUS writer_cpu=$WRITER_CPU (cleaner thread not pinned) paced_writer_rate=$PACED_RATE retain_limit_mb=$RETAIN_LIMIT_MB"
        echo "perf_events=$PERF_EVENTS perf_record_freq_hz=$PERF_FREQ"
        echo "perf: $(perf --version)   perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid) kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict)"
        echo "sha256 of the measured executables:"
        for b in $BINARIES; do echo "  $(sha256sum "$BUILD/$b" | cut -c1-64)  $b"; done
    } > "$OUT/env/campaign_config.txt"
}

stamp() { mkdir -p "$1"; echo "$CAMPAIGN" > "$1/.campaign"; }

# run_perf <dir> <id> <command...>: one measured process under `perf stat`,
# with its peak resident set from /usr/bin/time. Command and exit status are kept.
run_perf() {
    local dir="$1" id="$2"; shift 2
    mkdir -p "$dir/perf_stat" "$dir/stdout" "$dir/rss"
    {
        echo "# command: perf stat -e $PERF_EVENTS -o $dir/perf_stat/$id.txt -- /usr/bin/time -f maxrss_kb=%M -o $dir/rss/$id.txt $*"
        perf stat -e "$PERF_EVENTS" -o "$dir/perf_stat/$id.txt" -- /usr/bin/time -f 'maxrss_kb=%M' -o "$dir/rss/$id.txt" "$@"
        echo "# exit_status: $?"
    } > "$dir/stdout/$id.txt" 2>&1
    grep -q '^# exit_status: 0$' "$dir/stdout/$id.txt" || fail "$id (see $dir/stdout/$id.txt)"
}

# ---- D.1, track C: point gets with a writer active during the measured window ------
# d1c <variant> <scenario> <readers> <rep> <extra args...>
d1c() {
    local variant="$1" scenario="$2" readers="$3" rep="$4"; shift 4
    local dir="$OUT/d1/concurrent" id="${variant}_${scenario}_r${readers}_rep${rep}"
    mkdir -p "$dir/hist"
    # RCU: copy-on-write and reclamation counters; and a memory guard well above
    # anything a 5 s run retains (if the writer outruns the cleaner the run must
    # show that, not be cut short at the harness's 6 GB default).
    local cow=()
    [ "$variant" = rcu ] && cow=(--cow-csv "$dir/cow.csv" --retain-limit-mb "$RETAIN_LIMIT_MB")
    run_perf "$dir" "$id" "$BUILD/concurrent_bench_${variant}" --readers "$readers" --duration "$DURATION" \
        --warmup "$WARMUP" --reader-cpus "$(reader_cpus_for "$readers")" --seed "$rep" --run-id "$id" \
        --git-sha "$GIT_SHA" --scenario "$scenario" --csv "$dir/runs.csv" --hist "$dir/hist/$id.csv" "${cow[@]}" "$@"
}
stage_d1_concurrent() {
    rm -rf "$OUT/d1/concurrent"; stamp "$OUT/d1/concurrent"
    log "D.1 track C: {global_lock,rwlock,rcu} x {$READER_COUNTS} readers x {paced,unthrottled} writer, $REPS reps of ${DURATION}s"
    local rep variant readers W="--writer on --writer-cpu $WRITER_CPU"
    for rep in $(seq 1 "$REPS"); do     # repetitions outermost: drift is spread over all cells
        for variant in global_lock rwlock rcu; do
            for readers in $READER_COUNTS; do
                d1c "$variant" writer_paced "$readers" "$rep" $W --writer-rate "$PACED_RATE"
                d1c "$variant" writer_max "$readers" "$rep" $W
            done
        done
        # The unsynchronized store: ONE thread only (a concurrent run would be invalid).
        d1c single readonly 1 "$rep" --writer off
        d1c single inline_writer 1 "$rep" --writer inline:100
        # Reads only, as an upper bound for the RCU store.
        for readers in 1 8; do
            echo " $READER_COUNTS " | grep -q " $readers " && d1c rcu readonly "$readers" "$rep" --writer off
        done
        # The generator's own key distribution (80 % hot, 20 % mostly-missing random) WITH a writer.
        for variant in global_lock rwlock rcu; do
            echo " $READER_COUNTS " | grep -q " 8 " && d1c "$variant" refkeys_paced 8 "$rep" $W --writer-rate "$PACED_RATE" --keys reference
        done
        conditions "d1_concurrent:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# ---- D.1, track R: the unmodified supplied generator (PRELOAD-ONLY) ----------------
stage_d1_reference() {
    local dir="$OUT/d1/reference"
    rm -rf "$dir"; stamp "$dir"; mkdir -p "$dir/stdout"
    echo "run_id,driver,readers,duration_s,rep,total_ops,rps,p50_us,p99_us,p999_us,random_hit_rate,expected_overall_hit_rate" > "$dir/runs.csv"
    log "D.1 track R: supplied generator, default parameters, PRELOAD-ONLY (no writer while readers run)"
    local rep driver readers counts id line hit
    for rep in $(seq 1 "$REPS"); do
        for driver in single_thread global_lock rwlock rcu; do
            counts="$READER_COUNTS"
            [ "$driver" = single_thread ] && counts=1
            for readers in $counts; do
                id="${driver}_r${readers}_rep${rep}"
                {
                    echo "# command: taskset -c $READER_CPUS $BUILD/$driver $readers $REF_DURATION"
                    taskset -c "$READER_CPUS" "$BUILD/$driver" "$readers" "$REF_DURATION"
                    echo "# exit_status: $?"
                } > "$dir/stdout/$id.txt" 2>&1
                line="$(grep -E 'ops=' "$dir/stdout/$id.txt" | tail -1)"
                hit="$(grep -E '^reference_reads:' "$dir/stdout/$id.txt" | tail -1)"
                if [ -z "$line" ] || [ -z "$hit" ] || ! grep -q '^# exit_status: 0$' "$dir/stdout/$id.txt"; then fail "$id"; continue; fi
                echo "$line" | sed -E "s/.*ops=([0-9]+) +rps=([0-9.]+) +p50=([0-9.]+) *us p99=([0-9.]+) *us p999=([0-9.]+) *us.*/$id,$driver,$readers,$REF_DURATION,$rep,\1,\2,\3,\4,\5/" | tr -d '\n' >> "$dir/runs.csv"
                echo "$hit" | sed -E 's/.*random_hit_rate=([0-9.]+) expected_overall_hit_rate=([0-9.]+).*/,\1,\2/' >> "$dir/runs.csv"
            done
        done
        conditions "d1_reference:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# ---- D.2, workload A: the steady point-get workload at each reclaim setting --------
stage_d2_steady() {
    local dir="$OUT/d2/steady"
    rm -rf "$dir"; stamp "$dir"; mkdir -p "$dir/hist"
    log "D.2 workload A (steady): rcu, 8 readers, unthrottled writer; batches {$BATCHES}, disabled, stop-the-world"
    local rep batch id common
    for rep in $(seq 1 "$REPS"); do
        common=(--readers 8 --reader-cpus "$(reader_cpus_for 8)" --writer on --writer-cpu "$WRITER_CPU" --warmup "$WARMUP"
                --seed "$rep" --git-sha "$GIT_SHA" --csv "$dir/runs.csv" --cow-csv "$dir/cow.csv" --retain-limit-mb "$RETAIN_LIMIT_MB")
        for batch in $BATCHES; do
            id="steady_batch${batch}_rep${rep}"
            run_perf "$dir" "$id" "$BUILD/concurrent_bench_rcu" "${common[@]}" --duration "$SHORT" --reclaim-batch "$batch" \
                --run-id "$id" --scenario "batch_${batch}" --hist "$dir/hist/$id.csv"
        done
        id="steady_stw_rep${rep}"
        run_perf "$dir" "$id" "$BUILD/concurrent_bench_rcu" "${common[@]}" --duration "$SHORT" --reclaim stw \
            --run-id "$id" --scenario reclaim_stw --hist "$dir/hist/$id.csv"
        # Reclamation disabled retains every version: a 1 s window (the memory guard is in `common`).
        id="steady_disabled_rep${rep}"
        run_perf "$dir" "$id" "$BUILD/concurrent_bench_rcu" "${common[@]}" --duration 1 --reclaim disabled \
            --run-id "$id" --scenario reclaim_disabled --hist "$dir/hist/$id.csv"
        conditions "d2_steady:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# ---- D.2, workload B: reclamation falls behind (4 KiB values, snapshot pinning) ----
stage_d2_backlog() {
    local dir="$OUT/d2/backlog"
    rm -rf "$dir"; stamp "$dir"; mkdir -p "$dir/hist"
    log "D.2 workload B (backlog-building): 4 KiB values, snapshot held 20 ms every 50 ms; same settings"
    local rep batch id common
    for rep in $(seq 1 "$REPS"); do
        common=(--readers 8 --reader-cpus "$(reader_cpus_for 8)" --writer-cpu "$WRITER_CPU" --warmup "$WARMUP" --seed "$rep"
                --git-sha "$GIT_SHA" --csv "$dir/runs.csv" --value-bytes 4096 --keys 2000 --pin-hold-ms 20 --pin-gap-ms 30)
        for batch in $BATCHES; do
            id="backlog_batch${batch}_rep${rep}"
            run_perf "$dir" "$id" "$BUILD/reclaim_bench" "${common[@]}" --duration "$SHORT" --reclaim-batch "$batch" \
                --run-id "$id" --scenario "batch_${batch}" --hist "$dir/hist/$id.csv"
        done
        id="backlog_stw_rep${rep}"
        run_perf "$dir" "$id" "$BUILD/reclaim_bench" "${common[@]}" --duration "$SHORT" --reclaim stw \
            --run-id "$id" --scenario reclaim_stw --hist "$dir/hist/$id.csv"
        id="backlog_disabled_rep${rep}"
        run_perf "$dir" "$id" "$BUILD/reclaim_bench" "${common[@]}" --duration 1 --reclaim disabled --retain-limit-mb 8000 \
            --run-id "$id" --scenario reclaim_disabled --hist "$dir/hist/$id.csv"
        conditions "d2_backlog:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# jb <dir> <id> <args...>: one join_bench run
jb() {
    local dir="$1" id="$2"; shift 2
    mkdir -p "$dir/hist"
    run_perf "$dir" "$id" "$BUILD/join_bench" --writer-cpu "$WRITER_CPU" --csv "$dir/runs.csv" --hist "$dir/hist/$id.csv" \
        --run-id "$id" --git-sha "$GIT_SHA" "$@"
}

# ---- D.3: transaction commit throughput by size --------------------------------------
stage_d3_txn() {
    local dir="$OUT/d3/txn"
    rm -rf "$dir"; stamp "$dir"
    log "D.3 transactions: sizes {$TXN_SIZES} keys, Trades table without and with a secondary index"
    local rep size
    for rep in $(seq 1 "$REPS"); do
        for size in $TXN_SIZES; do
            jb "$dir" "txn${size}_plain_rep${rep}" --experiment txn --txn-size "$size" --index off --duration "$SHORT"
            jb "$dir" "txn${size}_indexed_rep${rep}" --experiment txn --txn-size "$size" --index on --duration "$SHORT"
        done
        conditions "d3_txn:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# ---- D.3: the index-nested-loop join, writer active and paused -----------------------
stage_d3_join() {
    local dir="$OUT/d3/join"
    rm -rf "$dir"; stamp "$dir"
    log "D.3 join: {$JOIN_THREADS} joiner threads, writer paused and active (one Snapshot per store)"
    local rep t
    for rep in $(seq 1 "$REPS"); do
        for t in $JOIN_THREADS; do
            jb "$dir" "join_t${t}_paused_rep${rep}" --experiment join --threads "$t" --cpus "$(reader_cpus_for "$t")" --writer off --duration "$SHORT"
            jb "$dir" "join_t${t}_active_rep${rep}" --experiment join --threads "$t" --cpus "$(reader_cpus_for "$t")" --writer on --duration "$SHORT"
        done
        conditions "d3_join:rep$rep"
        log "  repetition $rep/$REPS done"
    done
}

# ---- D.3: where copy-on-write hurts ---------------------------------------------------
stage_d3_cow() {
    local dir="$OUT/d3/cow"
    rm -rf "$dir"; stamp "$dir"
    log "D.3 copy-on-write cost: the same rows as one transaction and as single puts"
    local rep
    for rep in $(seq 1 "$REPS"); do
        jb "$dir" "cow_rep${rep}" --experiment cow
    done
    conditions "d3_cow:end"
}

# ---- Track R with the generator's own transaction and index-probe paths --------------
stage_d3_generator() {
    local dir="$OUT/d3/generator"
    rm -rf "$dir"; stamp "$dir"; mkdir -p "$dir/stdout"
    echo "run_id,driver,mode,readers,duration_s,rep,total_ops,rps,p50_us,p99_us,p999_us,probe_hit_rate" > "$dir/runs.csv"
    log "track R, generator's use_txn and use_join paths on the rcu store (an index PROBE, not a join; PRELOAD-ONLY)"
    local rep mode id line probe
    for rep in $(seq 1 "$REPS"); do
        for mode in put txn:16 join; do
            id="rcu_$(echo "$mode" | tr ':' '_')_r8_rep${rep}"
            {
                echo "# command: taskset -c $READER_CPUS $BUILD/rcu 8 $REF_DURATION 1000 100 $mode"
                taskset -c "$READER_CPUS" "$BUILD/rcu" 8 "$REF_DURATION" 1000 100 "$mode"
                echo "# exit_status: $?"
            } > "$dir/stdout/$id.txt" 2>&1
            line="$(grep -E 'ops=' "$dir/stdout/$id.txt" | tail -1)"
            probe="$(grep -E '^generator_index_probe:' "$dir/stdout/$id.txt" | sed -E 's/.*probe_hit_rate=([0-9.]+).*/\1/')"
            if [ -z "$line" ] || ! grep -q '^# exit_status: 0$' "$dir/stdout/$id.txt"; then fail "$id"; continue; fi
            if [ "$mode" = join ] && [ -z "$probe" ]; then fail "$id (no probe hit rate)"; continue; fi
            echo "$line" | sed -E "s/.*ops=([0-9]+) +rps=([0-9.]+) +p50=([0-9.]+) *us p99=([0-9.]+) *us p999=([0-9.]+) *us.*/$id,rcu,$mode,8,$REF_DURATION,$rep,\1,\2,\3,\4,\5,${probe:-}/" >> "$dir/runs.csv"
        done
    done
    conditions "d3_generator:end"
}

# ---- perf record + flame graphs ---------------------------------------------------------
# fg <id> <title> <command...>
fg() {
    local dir="$OUT/perf/flamegraphs" tmp="build/perf_data" id="$1" title="$2"; shift 2
    mkdir -p "$tmp"
    log "perf record: $id"
    {
        echo "# command: perf record -F $PERF_FREQ --call-graph fp -o $tmp/$id.data -- $*"
        perf record -F "$PERF_FREQ" --call-graph fp -o "$tmp/$id.data" -- "$@"
        echo "# exit_status: $?"
    } > "$dir/$id.record.log" 2>&1
    grep -q '^# exit_status: 0$' "$dir/$id.record.log" || { fail "perf record $id"; return; }
    perf script --no-inline -i "$tmp/$id.data" > "$tmp/$id.perf-script.txt" 2> "$dir/$id.script.log" || { fail "perf script $id"; return; }
    sed -E 's/^(\s+)(ffffffff[0-9a-f]+) \[unknown\] \(\[unknown\]\)/\1\2 [kernel] ([kernel])/' "$tmp/$id.perf-script.txt" \
        | "$FG/stackcollapse-perf.pl" 2>> "$dir/$id.script.log" \
        | perl -pe 's/(\[kernel\];)+/[kernel];/g; s/(;\[kernel\])+ /;[kernel] /' \
        | sort | awk '{c=$NF; $NF=""; sub(/ $/,""); a[$0]+=c} END{for(k in a) print k, a[k]}' | sort > "$dir/$id.folded"
    "$FG/flamegraph.pl" --width 1400 --countname cycles --title "$title" "$dir/$id.folded" > "$dir/$id.svg" 2>> "$dir/$id.script.log"
    [ -s "$dir/$id.svg" ] || { fail "flame graph $id"; return; }
    xz -9 -T0 -c "$tmp/$id.perf-script.txt" > "$dir/$id.perf-script.txt.xz"
    echo "$id: $(grep -c 'cycles' "$tmp/$id.perf-script.txt") samples, $(awk '{s+=$NF} END{print s}' "$dir/$id.folded") cycles" >> "$dir/sample_counts.txt"
}
stage_flamegraphs() {
    local dir="$OUT/perf/flamegraphs"
    rm -rf "$dir"; stamp "$dir"
    {
        echo "FlameGraph: https://github.com/brendangregg/FlameGraph @ $(git -C "$FG" rev-parse HEAD 2>/dev/null)"
        echo "perf: $(perf --version)   kptr_restrict: $(cat /proc/sys/kernel/kptr_restrict)"
        echo "Each <id>.record.log starts with the exact perf record command. Then:"
        echo "  DEBUGINFOD_URLS= perf script --no-inline -i <id>.data   (kept as <id>.perf-script.txt.xz)"
        echo "  sed: label unresolved kernel frames [kernel]; stackcollapse-perf.pl; merge consecutive [kernel] frames; flamegraph.pl"
        echo "Samples are taken on the cycles event: box widths are shares of on-CPU cycles. A thread blocked"
        echo "in the kernel consumes no cycles and is not sampled. kptr_restrict is nonzero, so kernel frames"
        echo "have no symbol names; no sample is added, removed or reattributed."
    } > "$dir/COMMANDS.txt"
    local c="--warmup $WARMUP --duration 5 --git-sha $GIT_SHA --csv $dir/profiled_runs.csv"
    local W="--writer on --writer-cpu $WRITER_CPU" readers
    for readers in $READER_COUNTS; do
        fg "rcu_r${readers}_writer_max" "NOVA-KV Part D: rcu, $readers reader(s) + unthrottled writer (on-CPU cycles)" \
            "$BUILD/concurrent_bench_rcu" --readers "$readers" --reader-cpus "$(reader_cpus_for "$readers")" $W $c \
            --run-id "fg_rcu_r${readers}_writer_max" --scenario writer_max_profiled
    done
    if echo " $READER_COUNTS " | grep -q " 8 "; then
        fg rcu_r8_writer_paced "NOVA-KV Part D: rcu, 8 readers + writer paced at $PACED_RATE/s (on-CPU cycles)" \
            "$BUILD/concurrent_bench_rcu" --readers 8 --reader-cpus "$(reader_cpus_for 8)" $W --writer-rate "$PACED_RATE" $c \
            --run-id fg_rcu_r8_writer_paced --scenario writer_paced_profiled
        fg rcu_r8_readonly "NOVA-KV Part D: rcu, 8 readers, no writer (on-CPU cycles)" \
            "$BUILD/concurrent_bench_rcu" --readers 8 --reader-cpus "$(reader_cpus_for 8)" --writer off $c \
            --run-id fg_rcu_r8_readonly --scenario readonly_profiled
        local variant
        for variant in global_lock rwlock; do
            fg "${variant}_r8_writer_max" "NOVA-KV Part D: $variant, 8 readers + unthrottled writer (on-CPU cycles)" \
                "$BUILD/concurrent_bench_${variant}" --readers 8 --reader-cpus "$(reader_cpus_for 8)" $W $c \
                --run-id "fg_${variant}_r8_writer_max" --scenario writer_max_profiled
        done
        fg join_t8_writer_active "NOVA-KV Part D: index-nested-loop join, 8 joiners + writer (on-CPU cycles)" \
            "$BUILD/join_bench" --experiment join --threads 8 --cpus "$(reader_cpus_for 8)" --writer on --writer-cpu "$WRITER_CPU" \
            --duration 3 --run-id fg_join_t8_active --csv "$dir/profiled_join_runs.csv"
    fi
    conditions "flamegraphs:end"
}

# ---- perf c2c: which cache lines are written by one core and read by others ------------
# The evidence a flame graph cannot give for the "owner counts share cache lines
# with reader data" hypothesis (audit item R8).
c2c() {
    local dir="$OUT/perf/c2c" tmp="build/perf_data" id="$1"; shift
    mkdir -p "$tmp"
    log "perf c2c: $id"
    {
        echo "# command: perf c2c record -o $tmp/$id.c2c.data -- $*"
        perf c2c record -o "$tmp/$id.c2c.data" -- "$@"
        echo "# exit_status: $?"
    } > "$dir/$id.record.log" 2>&1
    grep -q '^# exit_status: 0$' "$dir/$id.record.log" || { fail "perf c2c record $id"; return; }
    perf c2c report -i "$tmp/$id.c2c.data" --stdio > "$dir/$id.c2c-report.txt" 2> "$dir/$id.report.log" || fail "perf c2c report $id"
}
stage_c2c() {
    local dir="$OUT/perf/c2c"
    rm -rf "$dir"; stamp "$dir"
    local W="--writer on --writer-cpu $WRITER_CPU" c="--warmup $WARMUP --duration 5 --git-sha $GIT_SHA"
    c2c rcu_r8_writer_max "$BUILD/concurrent_bench_rcu" --readers 8 --reader-cpus "$(reader_cpus_for 8)" $W $c --run-id c2c_rcu_r8_writer_max --scenario writer_max_c2c
    c2c rcu_r8_readonly "$BUILD/concurrent_bench_rcu" --readers 8 --reader-cpus "$(reader_cpus_for 8)" --writer off $c --run-id c2c_rcu_r8_readonly --scenario readonly_c2c
    c2c rcu_r1_writer_max "$BUILD/concurrent_bench_rcu" --readers 1 --reader-cpus "$(reader_cpus_for 1)" $W $c --run-id c2c_rcu_r1_writer_max --scenario writer_max_c2c
    c2c rwlock_r8_writer_max "$BUILD/concurrent_bench_rwlock" --readers 8 --reader-cpus "$(reader_cpus_for 8)" $W $c --run-id c2c_rwlock_r8_writer_max --scenario writer_max_c2c
    conditions "c2c:end"
}

STAGES="d1_concurrent d1_reference d2_steady d2_backlog d3_txn d3_join d3_cow d3_generator flamegraphs c2c"

case "$MODE" in
    check)
        OUT="$(mktemp -d)"; check_prerequisites; rc=$?; rm -rf "$OUT"
        [ $rc -eq 0 ] && echo "prerequisites: OK" || echo "prerequisites: NOT MET"
        exit $rc ;;
    all|pilot)
        [ "$(nova_git_dirty)" = no ] || [ "$MODE" = pilot ] || die "the working tree is not clean: commit first, so that results name an exact revision"
        rm -rf "$OUT"; mkdir -p "$OUT/env"
        CAMPAIGN="$(date +%Y%m%dT%H%M%S)-$GIT_SHA-$MODE"
        echo "$CAMPAIGN" > "$OUT/CAMPAIGN"
        check_prerequisites "$OUT/env/prerequisites.txt" || die "prerequisites not met (see above): nothing was measured"
        scripts/env_info.sh > "$OUT/env/environment.txt" 2>&1
        { lscpu; echo; lscpu -e=CPU,CORE,SOCKET,MAXMHZ,MINMHZ; echo; free -m; echo; uname -a; } > "$OUT/env/hardware.txt" 2>&1
        build_release
        conditions "campaign:start"
        for st in $STAGES; do
            conditions "$st:start"
            "stage_$st"
            conditions "$st:end"
        done
        conditions "campaign:end"
        ;;
    d1_concurrent|d1_reference|d2_steady|d2_backlog|d3_txn|d3_join|d3_cow|d3_generator|flamegraphs|c2c)
        [ -f "$OUT/CAMPAIGN" ] || die "no campaign in $OUT: run 'all' first"
        CAMPAIGN="$(cat "$OUT/CAMPAIGN")"
        [ "${CAMPAIGN#*-}" = "$GIT_SHA-all" ] || [ "${CAMPAIGN#*-}" = "$GIT_SHA-pilot" ] || die "the campaign in $OUT was measured at another revision (${CAMPAIGN}); a stage rerun must use the same one"
        check_prerequisites "$OUT/env/prerequisites_$MODE.txt" || die "prerequisites not met"
        conditions "$MODE:start"; "stage_$MODE"; conditions "$MODE:end"
        ;;
    *) die "unknown mode '$MODE'" ;;
esac

log "'$MODE' finished with $FAILURES failure(s)$([ -f "$OUT/INVALID" ] && echo '; CONDITIONS CHANGED: campaign INVALID')"
[ "$FAILURES" -eq 0 ] && [ ! -f "$OUT/INVALID" ]
