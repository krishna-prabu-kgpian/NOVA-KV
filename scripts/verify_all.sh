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
    if git diff part-a-final HEAD -- src include | cmp -s - results/diffs/part_a_to_part_b.diff; then
        echo "ok       Part A -> Part B diff matches the sources at HEAD"
    else
        echo "STALE    results/diffs/part_a_to_part_b.diff differs from: git diff part-a-final HEAD -- src include"; status=1
    fi
    git rev-parse --verify --quiet part-b-publication^{commit} >/dev/null || { echo "MISSING  tag part-b-publication"; status=1; }
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
    echo "- milestones covered: A0, A1, A2, B1, B2"
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
