#!/usr/bin/env bash
# verify_all.sh -- runs every verification gate that applies to the milestones
# implemented so far and exits nonzero unless every MANDATORY gate is PASS.
#
# Evidence goes to results/verification/<git-sha>[-dirty]/:
#   summary.md             gate table (PASS / FAIL / NOT RUN, exit codes, logs)
#   environment.txt        machine, kernel, compiler, tool versions
#   source_manifest.sha256 SHA-256 of every tracked file
#   logs/<gate>.log        full stdout+stderr of each gate, with its command
#
# A gate is NOT RUN when a prerequisite is missing or failed (e.g. tests after a
# failed build, or a sanitizer runtime that cannot start here). NOT RUN on a
# mandatory gate blocks overall PASS exactly like FAIL does.
#
# Gates are added here as milestones land; expectations are never loosened to
# hide a failure. Currently covered: A0, A1, A2, B1, B2.
set -u -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$NOVA_ROOT"

JOBS="${JOBS:-$(nproc)}"
SHA="$(git rev-parse --short=12 HEAD 2>/dev/null || echo nogit)"
[ "$(nova_git_dirty)" = yes ] && SHA="${SHA}-dirty"
OUT="results/verification/${SHA}"
LOGS="$OUT/logs"
BUILD_ROOT="build/verify"
rm -rf "$OUT"
mkdir -p "$LOGS"

SUMMARY_ROWS=()
OVERALL=0

# record <gate> <mandatory:yes|no> <PASS|FAIL|NOT RUN> <exit|-> <note>
record() {
    local gate="$1" mandatory="$2" result="$3" rc="$4" note="$5"
    SUMMARY_ROWS+=("| $gate | $mandatory | $result | $rc | $note |")
    printf '%-28s %-8s (mandatory: %s)\n' "$gate" "$result" "$mandatory"
    if [ "$mandatory" = yes ] && [ "$result" != PASS ]; then OVERALL=1; fi
}

# run_gate <gate> <mandatory> <command...>   -> returns the command's status
run_gate() {
    local gate="$1" mandatory="$2"; shift 2
    local log="$LOGS/$gate.log" rc
    {
        echo "# gate: $gate"
        echo "# started_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# cwd: $PWD"
        echo "# command: $*"
        echo
    } > "$log"
    "$@" >> "$log" 2>&1
    rc=$?
    printf '\n# exit_status: %d\n' "$rc" >> "$log"
    if [ $rc -eq 0 ]; then record "$gate" "$mandatory" PASS 0 "logs/$gate.log"
    else record "$gate" "$mandatory" FAIL "$rc" "logs/$gate.log"; fi
    return $rc
}

skip_gate() { record "$1" "$2" "NOT RUN" - "$3"; }

# ---- Environment and provenance ---------------------------------------------
scripts/env_info.sh > "$OUT/environment.txt" 2>&1
scripts/source_manifest.sh > "$OUT/source_manifest.sha256" 2>&1

run_gate supplied_files_unchanged yes sh -c 'cd supplied && sha256sum -c SHA256SUMS'
run_gate no_external_deps yes scripts/check_no_external_deps.sh

# Whitespace errors in project-authored tracked files and in uncommitted edits.
# The imported originals are excluded: they are preserved as received.
EMPTY_TREE="$(git hash-object -t tree /dev/null)"
run_gate whitespace_check yes sh -c "
    git diff --check $EMPTY_TREE HEAD -- . ':!NOVA_KV_*.md' ':!assignment2.pdf' ':!supplied/workload_gen.cpp' ':!results' &&
    git diff --check HEAD -- . ':!results'"

# ---- Build + test matrix -----------------------------------------------------
if ! nova_resolve_cmake 2> "$LOGS/cmake_resolve.log"; then
    for mode in debug release asan_ubsan tsan; do
        skip_gate "${mode}_configure" yes "cmake not found (run scripts/bootstrap_tools.sh)"
        skip_gate "${mode}_build" yes "cmake not found"
        skip_gate "${mode}_ctest" yes "cmake not found"
    done
else
    rm -f "$LOGS/cmake_resolve.log"

    # sanitizer_usable <flags>: can a trivial threaded program built with these
    # flags actually start on this machine? (Distinguishes "runtime unusable
    # here" = NOT RUN from "our code has a defect" = FAIL.)
    sanitizer_usable() {
        local dir="$BUILD_ROOT/probe" ok=0
        mkdir -p "$dir"
        printf '#include <thread>\nint main(){ std::thread t([]{}); t.join(); return 0; }\n' > "$dir/probe.cpp"
        g++ -std=c++17 -pthread "$@" "$dir/probe.cpp" -o "$dir/probe" && "$dir/probe" || ok=1
        return $ok
    }

    # verify_mode <name> <sanitizer probe flags or ''> <cmake args...>
    verify_mode() {
        local mode="$1" probe_flags="$2"; shift 2
        local bdir="$BUILD_ROOT/$mode"
        if [ -n "$probe_flags" ] && ! sanitizer_usable $probe_flags > "$LOGS/${mode}_probe.log" 2>&1; then
            skip_gate "${mode}_configure" yes "sanitizer runtime unusable here, see logs/${mode}_probe.log"
            skip_gate "${mode}_build" yes "sanitizer runtime unusable here"
            skip_gate "${mode}_ctest" yes "sanitizer runtime unusable here"
            return
        fi
        rm -rf "$bdir"  # every run is a clean configure + build
        if ! run_gate "${mode}_configure" yes "$NOVA_CMAKE" -S . -B "$bdir" "$@"; then
            skip_gate "${mode}_build" yes "configure failed"
            skip_gate "${mode}_ctest" yes "configure failed"
            return
        fi
        if ! run_gate "${mode}_build" yes "$NOVA_CMAKE" --build "$bdir" -j "$JOBS"; then
            skip_gate "${mode}_ctest" yes "build failed"
            return
        fi
        run_gate "${mode}_ctest" yes "$NOVA_CTEST" --test-dir "$bdir" --output-on-failure
    }

    verify_mode debug      ""                           -DCMAKE_BUILD_TYPE=Debug
    verify_mode release    ""                           -DCMAKE_BUILD_TYPE=Release
    verify_mode asan_ubsan "-fsanitize=address,undefined" -DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_ASAN_UBSAN=ON
    verify_mode tsan       "-fsanitize=thread"          -DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_TSAN=ON
fi

# ---- Part A deliverables (milestone A2) ----------------------------------------
# Presence of every Part A artifact, and the page limits of the rendered report
# (<= 5 pages, design note <= 3), both read from the committed files.
run_gate part_a_artifacts yes scripts/check_part_a_artifacts.sh
run_gate part_a_report_pages yes scripts/check_part_a_report.sh
# The accepted Part A checkpoint: tag present, and its measurements, report and
# compliance checklist byte-identical in this tree (Part B must not touch them).
run_gate part_a_checkpoint_unchanged yes sh -c '
    git rev-parse --verify --quiet part-a-final^{commit} >/dev/null || { echo "tag part-a-final missing"; exit 1; }
    git diff --stat --exit-code part-a-final HEAD -- results/part_a reports/part_a_report.pdf \
        reports/part_a_report.md reports/part_a_report.src.md docs/part_a_compliance.md \
        reports/milestones/A0.md reports/milestones/A1.md reports/milestones/A2.md &&
    echo "Part A results, report and milestone reports identical to part-a-final"'

# ---- Part B deliverables (milestone B2) ----------------------------------------
run_gate part_b_artifacts yes sh -c '
    status=0
    for f in include/kvstore.h src/kvstore.cpp src/versioned_store.cpp src/epoch.cpp src/pinned_arena.cpp \
             src/cow_segment.cpp tests/concurrent_lock_test.cpp tests/reclamation_test.cpp \
             tests/reclamation_stress_test.cpp reports/part_b_readme.md docs/reclamation.md \
             docs/part_b_compliance.md results/diffs/part_a_to_part_b.diff; do
        if [ -s "$f" ]; then echo "ok       $f"; else echo "MISSING  $f"; status=1; fi
    done
    grep -q "void set_reclaim_batch(int n);" include/kvstore.h || { echo "MISSING  set_reclaim_batch declaration"; status=1; }
    grep -q "std::atomic<int> reclaim_batch_;" include/kvstore.h || { echo "MISSING  private reclaim-batch member in kvstore.h"; status=1; }
    git rev-parse --verify --quiet part-b-audited^{commit} >/dev/null || { echo "MISSING  tag part-b-audited"; status=1; }
    if git diff part-a-final part-b-audited -- src include | cmp -s - results/diffs/part_a_to_part_b.diff; then
        echo "ok       Part A -> Part B diff matches: git diff part-a-final part-b-audited -- src include"
    else
        echo "STALE    results/diffs/part_a_to_part_b.diff differs from: git diff part-a-final part-b-audited -- src include"; status=1
    fi
    git rev-parse --verify --quiet part-b-publication^{commit} >/dev/null || { echo "MISSING  tag part-b-publication"; status=1; }
    exit $status'

# ---- Part C.1 deliverables (milestone C1) ---------------------------------------
run_gate part_c1_artifacts yes sh -c '
    status=0
    for f in src/staged_writes.cpp src/staged_writes.h tests/transaction_test.cpp tests/snapshot_test.cpp \
             tests/txn_failure_test.cpp tests/txn_concurrency_test.cpp docs/transactions.md \
             docs/part_c_compliance.md results/diffs/part_b_to_part_c1.diff; do
        if [ -s "$f" ]; then echo "ok       $f"; else echo "MISSING  $f"; status=1; fi
    done
    grep -q "write skew" docs/transactions.md || grep -q "Write skew" docs/transactions.md || { echo "MISSING  write-skew explanation"; status=1; }
    grep -q "Durability" docs/transactions.md || { echo "MISSING  durability statement"; status=1; }
    # Every tag that is a checkpoint so far still names the commit it was created on.
    for pair in a0-bootstrap:2ef5f19 part-a-core:80bc092 part-a-final:3444f23 part-b-publication:2d9c02c \
                part-b-final:304a8ac part-b-audited:87d38aa part-c-transactions:1e6af10 part-c-final:27d3161; do
        tag=${pair%%:*}; want=${pair##*:}
        have=$(git rev-parse --short=7 "$tag^{commit}" 2>/dev/null)
        if [ "$have" = "$want" ]; then echo "ok       tag $tag = $want"; else echo "MOVED    tag $tag: $have, expected $want"; status=1; fi
    done
    # The interim Part B -> Part C diff is current. It is frozen at the tag
    # part-c-transactions once that exists; before that it must match HEAD.
    base=HEAD
    git rev-parse --verify --quiet part-c-transactions^{commit} >/dev/null && base=part-c-transactions
    if git diff part-b-audited "$base" -- src include | cmp -s - results/diffs/part_b_to_part_c1.diff; then
        echo "ok       Part B -> Part C.1 diff matches: git diff part-b-audited $base -- src include"
    else
        echo "STALE    results/diffs/part_b_to_part_c1.diff differs from: git diff part-b-audited $base -- src include"; status=1
    fi
    exit $status'

# ---- Part C deliverables (milestone C2: Part C complete) --------------------------
run_gate part_c_artifacts yes sh -c '
    status=0
    for f in src/secondary_index.cpp src/secondary_index.h benchmarks/join.h benchmarks/join_bench.cpp \
             tests/index_test.cpp tests/index_failure_test.cpp tests/index_concurrency_test.cpp tests/join_test.cpp \
             docs/secondary_index.md docs/transactions.md docs/part_c_compliance.md reports/part_c_readme.md \
             results/diffs/part_b_to_part_c.diff; do
        if [ -s "$f" ]; then echo "ok       $f"; else echo "MISSING  $f"; status=1; fi
    done
    grep -q "bool create_index(size_t field_offset, size_t field_len);" include/kvstore.h || { echo "MISSING  create_index declaration"; status=1; }
    # The cross-store deviation must be stated wherever the join is described.
    for f in reports/part_c_readme.md docs/secondary_index.md docs/part_c_compliance.md benchmarks/join.h; do
        grep -qi "two handles" "$f" || { echo "MISSING  one-snapshot-per-store limitation in $f"; status=1; }
    done
    grep -qi "write skew" reports/part_c_readme.md || { echo "MISSING  write-skew discussion in the Part C README"; status=1; }
    # The Part B -> Part C diff is current: against the tag part-c-final once
    # that exists, against HEAD before.
    base=HEAD
    git rev-parse --verify --quiet part-c-final^{commit} >/dev/null && base=part-c-final
    if git diff part-b-audited "$base" -- src include | cmp -s - results/diffs/part_b_to_part_c.diff; then
        echo "ok       Part B -> Part C diff matches: git diff part-b-audited $base -- src include"
    else
        echo "STALE    results/diffs/part_b_to_part_c.diff differs from: git diff part-b-audited $base -- src include"; status=1
    fi
    exit $status'

# ---- Part D deliverables (milestone D) --------------------------------------------
run_gate part_d_artifacts yes sh -c '
    status=0
    for f in scripts/run_part_d.sh scripts/aggregate_part_d.py scripts/part_d_profile_breakdown.py \
             benchmarks/concurrent_bench.cpp benchmarks/join_bench.cpp benchmarks/reclaim_bench.cpp \
             reports/part_d_report.md reports/part_d_report.pdf docs/part_d_compliance.md docs/part_d_plan.md \
             results/part_d/CAMPAIGN results/part_d/conditions.log results/part_d/env/campaign_config.txt \
             results/part_d/env/prerequisites.txt results/part_d/summary/summary.md \
             results/part_d/summary/profile_breakdown.txt results/part_d/d1/concurrent/runs.csv \
             results/part_d/d1/reference/runs.csv results/part_d/d2/steady/runs.csv results/part_d/d2/backlog/runs.csv \
             results/part_d/d3/txn/runs.csv results/part_d/d3/join/runs.csv results/part_d/d3/cow/runs.csv; do
        if [ -s "$f" ]; then echo "ok       $f"; else echo "MISSING  $f"; status=1; fi
    done
    [ -e results/part_d/INVALID ] && { echo "INVALID  the campaign is marked invalid"; status=1; }
    [ -e results/part_d/summary/problems.txt ] && [ ! -s results/part_d/summary/problems.txt ] \
        && echo "ok       aggregator found no problem" || { echo "PROBLEM  results/part_d/summary/problems.txt is missing or not empty"; status=1; }
    if grep -v -E "ac_online=1 .*governor=performance " results/part_d/conditions.log | grep -q .; then
        echo "CHANGED  a condition record is not mains + performance"; status=1
    else echo "ok       $(grep -c . results/part_d/conditions.log) condition records: mains power, performance governor"; fi
    campaign=$(cat results/part_d/CAMPAIGN)
    for d in d1/concurrent d1/reference d2/steady d2/backlog d3/txn d3/join d3/cow d3/generator perf/flamegraphs perf/c2c; do
        [ "$(cat results/part_d/$d/.campaign 2>/dev/null)" = "$campaign" ] || { echo "MIXED    results/part_d/$d is not from campaign $campaign"; status=1; }
    done
    n() { ls "$1" 2>/dev/null | grep -c "$2"; }
    [ "$(n results/part_d/figures .png)" -ge 12 ] && echo "ok       figures: $(n results/part_d/figures .png)" || { echo "MISSING  figures"; status=1; }
    [ "$(n results/part_d/perf/flamegraphs "\.svg$")" -ge 9 ] && echo "ok       flame graphs: $(n results/part_d/perf/flamegraphs "\.svg$")" || { echo "MISSING  flame graphs"; status=1; }
    [ "$(n results/part_d/perf/c2c c2c-report.txt)" -ge 4 ] && echo "ok       perf c2c reports: $(n results/part_d/perf/c2c c2c-report.txt)" || { echo "MISSING  perf c2c reports"; status=1; }
    runs=$(($(wc -l < results/part_d/d1/concurrent/runs.csv) - 1))
    [ "$runs" -eq "$(n results/part_d/d1/concurrent/hist .csv)" ] && [ "$runs" -eq "$(n results/part_d/d1/concurrent/perf_stat .txt)" ] \
        && echo "ok       D.1 track C: $runs runs, each with a histogram and a perf stat file" || { echo "MISMATCH D.1 runs / histograms / perf stat files"; status=1; }
    # Statements the report must not lose.
    grep -qi "preload-only" reports/part_d_report.md || { echo "MISSING  preload-only label in the report"; status=1; }
    grep -qi "two handles" reports/part_d_report.md || { echo "MISSING  one-snapshot-per-store limitation in the report"; status=1; }
    grep -q "not what was measured" reports/part_d_report.md || { echo "MISSING  the batch trade-off finding"; status=1; }
    exit $status'

# ---- Informational (do not affect the verdict) ------------------------------
# Whether profiling can be re-run on this machine (the committed Part A profiles
# do not depend on it).
run_gate perf_stat_usable no perf stat -e task-clock -- true
run_gate flamegraph_scripts_present no sh -c 'test -x .tools/FlameGraph/flamegraph.pl -a -x .tools/FlameGraph/stackcollapse-perf.pl || { command -v stackcollapse-perf.pl && command -v flamegraph.pl; }'

# ---- Summary ------------------------------------------------------------------
{
    echo "# Verification summary"
    echo
    echo "- git: \`$SHA\`"
    echo "- finished_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "- compiler: $(g++ --version | head -1)"
    echo "- cmake: ${NOVA_CMAKE:-not found}"
    echo "- milestones covered: A0, A1, A2, B1, B2, C1, C2, D"
    echo "- overall: $([ $OVERALL -eq 0 ] && echo PASS || echo FAIL)  (every mandatory gate must be PASS)"
    echo
    echo "| Gate | Mandatory | Result | Exit | Evidence / note |"
    echo "|---|---|---|---|---|"
    printf '%s\n' "${SUMMARY_ROWS[@]}"
} > "$OUT/summary.md"

echo
echo "evidence: $OUT/summary.md"
echo "overall: $([ $OVERALL -eq 0 ] && echo PASS || echo FAIL)"
exit $OVERALL
