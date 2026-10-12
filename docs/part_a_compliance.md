# Part A compliance checklist

Requirement by requirement, from `assignment2.pdf` pages 2–3 (Part A), page 7 (marks) and
pages 8–11 (Appendix A). Each row names the artifact that answers it and how that artifact is
checked. Status values: PASS (artifact exists and its check ran and passed), PARTIAL (see
note), FAIL, NOT RUN.

"Check" columns refer to things that actually run: a CTest test, a gate of
`scripts/verify_all.sh`, or a script named in the row. Statuses were filled in from the
verification run recorded in `reports/milestones/A2.md`.

## A. Store and sanity test

| # | Requirement (PDF) | Artifact | Check | Status |
|---|---|---|---|---|
| A1 | Core store in `kvstore.cpp` / `kvstore.h` (p. 2) | `include/kvstore.h`, `src/kvstore.cpp` (+ `src/hash_segment.*`, `src/arena.*`, `src/alloc.*`) | builds in 4 modes | PASS |
| A2 | "exposing exactly the KVStore interface from Appendix A (put, get, erase)" (p. 2, p. 8) | `include/kvstore.h` | `api_conformance_test` (compile-time) | PASS |
| A3 | Copy constructor/assignment deleted, move left undeclared (p. 8, p. 10) | `include/kvstore.h` | `api_conformance_test` | PASS |
| A4 | "Internally it's a hash table (open addressing or chaining)" (p. 2) | `src/hash_segment.*`: open addressing, linear probing | `hash_segment_test` | PASS |
| A5 | No existing concurrent hash-map library (p. 2) | — | gate `no_external_deps` | PASS |
| A6 | `get()` returns true if found, value copied into `value_out` (p. 8, p. 10) | `src/kvstore.cpp` | `kvstore_test_*` | PASS |
| A7 | `erase()` returns true if the key was present and is now removed (p. 8) | `src/kvstore.cpp` | `kvstore_test_*` | PASS |
| A8 | `kvstore_test.cpp` checking basic insert, lookup, overwrite, erase (p. 2) | `tests/kvstore_test.cpp` | `kvstore_test_{single,global_lock,rwlock}` | PASS |
| A9 | Build with `-std=c++17` or later (p. 2, p. 11) | `CMakeLists.txt` | all compile commands carry `-std=c++17` | PASS |

## B. Three drivers

| # | Requirement (PDF) | Artifact | Check | Status |
|---|---|---|---|---|
| B1 | `single_thread.cpp`: workload generator, no concurrency (p. 2) | `drivers/single_thread.cpp` (store variant without a lock; one reader thread) | `driver_single_thread` | PASS |
| B2 | `global_lock.cpp`: "a single std::mutex held for every put/get" (p. 2) | `drivers/global_lock.cpp`, `src/sync_policy.h` | `driver_global_lock`, `concurrent_lock_test_global_lock` | PASS |
| B3 | `rwlock.cpp`: `std::shared_mutex`, readers shared, writer unique (p. 2) | `drivers/rwlock.cpp`, `src/sync_policy.h` | `driver_rwlock`, `concurrent_lock_test_rwlock` | PASS |
| B4 | "Use the same underlying data structure across all three drivers. Only the synchronization should differ." (p. 2) | one source set compiled three times; `src/sync_policy.h` is the only difference | `kvstore_test` passes identically on all three | PASS |
| B5 | Drivers "run the workload generator against your store" (p. 2); generator used unchanged (p. 11) | `supplied/workload_gen.cpp` linked unmodified | gate `supplied_files_unchanged`; `reference_workload_link_test_*` | PASS |

## C. Profiling of `global_lock` and `rwlock`

| # | Requirement (PDF) | Artifact | Check | Status |
|---|---|---|---|---|
| C1 | Profile at 1, 2, 4 and 8 reader threads, "one background writer throughout" (p. 2) | `benchmarks/concurrent_bench.cpp`; `results/part_a/concurrent/runs.csv` (`write_ok` > 0 in every writer row) | harness exits nonzero if the writer made no update in the window; `bench_validate_*` | PASS |
| C2 | `perf stat` against both `global_lock` and `rwlock` (p. 3) | `results/part_a/concurrent/perf_stat/{global_lock,rwlock}_writer_max_r{1,2,4,8}_rep{1..5}.txt` (40 files), plus the paced-writer set | `scripts/check_part_a_artifacts.sh` | PASS |
| C3 | `perf record` + `perf script` for both, at 1/2/4/8 readers (p. 3) | `results/part_a/flamegraphs/<variant>_r<N>.perf-script.txt.xz` (8 files) | `scripts/check_part_a_artifacts.sh` | PASS |
| C4 | Flame graphs with Brendan Gregg's `stackcollapse-perf.pl` and `flamegraph.pl`, both variants × 1/2/4/8 (p. 3) | `results/part_a/flamegraphs/<variant>_r<N>.svg` and `.folded` (8 each); commands in `COMMANDS.txt` | `scripts/check_part_a_artifacts.sh` | PASS |
| C5 | Identify where the 8-core runs lose time: lock acquisition vs cache-line bouncing, pointing to the flame graph and the cache-miss counters "instead of asserting" (p. 3) | `reports/part_a_report.pdf` §3–§4; `results/part_a/summary/flamegraph_breakdown.csv`, `concurrent_summary.csv` | read by a human | PASS. Lock waiting: flame-graph cycle shares and context-switch counts. Cache-line bouncing: IPC / cache-miss counters plus `perf c2c` HITM reports (`results/part_a/c2c/`, `summary/c2c_summary.csv`) that locate the contended line at the lock's address |

## D. Storage-layout design note

| # | Requirement (PDF) | Artifact | Check | Status |
|---|---|---|---|---|
| D1 | Design note arguing for one of NSM / DSM / PAX for this workload: whole-row point gets plus the Part C multi-field join (p. 3) | `reports/part_a_report.pdf` §5 | read by a human | PASS |
| D2 | "at least one microbenchmark ... that actually measures point-get latency (or throughput) under two of these layouts side by side" (p. 3) | `benchmarks/layout_bench.cpp` (all three layouts); `results/part_a/layout/runs.csv` | `bench_validate_layout` (checksums equal across layouts) | PASS |
| D3 | Claim backed by a measured number, not asserted (p. 3) | `results/part_a/summary/layout_summary.csv`, `layout_perf_summary.csv` | — | PASS |
| D4 | Design note ≤ 3 of the 5 pages (p. 3) | `reports/part_a_report.pdf` | `scripts/check_part_a_report.sh` | PASS |

## E. Deliverable list (p. 3)

| # | Deliverable | Artifact | Status |
|---|---|---|---|
| E1 | `kvstore.cpp` / `kvstore.h` | `src/kvstore.cpp`, `include/kvstore.h` | PASS (flat top-level copies are produced by the final packaging milestone) |
| E2 | `kvstore_test.cpp` | `tests/kvstore_test.cpp` | PASS |
| E3 | The three driver files | `drivers/*.cpp` | PASS |
| E4 | perf outputs | `results/part_a/concurrent/perf_stat/`, `results/part_a/flamegraphs/*.perf-script.txt.xz`, `results/part_a/layout/perf_stat/` | PASS |
| E5 | Flame-graph images | `results/part_a/flamegraphs/*.svg` | PASS |
| E6 | Report of no more than 5 pages covering the profiling findings and the layout decision | `reports/part_a_report.pdf` | PASS (`scripts/check_part_a_report.sh`) |

## F. Marks table (p. 7)

| Item | Marks | Evidence |
|---|---:|---|
| Core KV store + 3 concurrency variants | 20 | sections A and B |
| NSM vs DSM vs PAX write-up, backed by data | 20 | section D |
| Baseline profiling / flame-graph report | 10 | section C |

Whether the marks are awarded is the grader's decision; this table only maps evidence to
items.

## Not required by Part A, done anyway

- All three layouts are measured (two were required), at two table sizes and six access
  patterns, with hardware counters.
- A second writer setting (paced at 10,000 updates/s) besides the unthrottled writer.
- `perf c2c` cache-line contention reports for all 8 profiled configurations.
- Preload-only runs of the unmodified generator, kept separate and labelled as such.

## Known gaps and caveats

- The supplied generator cannot produce active-writer measurements (it loads everything before
  starting readers). The profiled runs therefore use the project's own harness. The three
  required driver files exist and run the generator as the assignment prescribes, but their
  numbers are preload-only and are reported as such.
- Kernel frames in the flame graphs have no symbol names (`kptr_restrict = 1`); see
  `results/part_a/flamegraphs/COMMANDS.txt`.
- `perf c2c` sampled a few hundred to a few thousand HITM loads per configuration, from one 5-second run each; the shares are indicative, not tight estimates.
- Measurements were taken on a laptop (AC power, `performance` governor, see
  `results/part_a/conditions.log`) whose CPU has four fast and four slower cores, with a
  desktop session running; see `results/part_a/environment.txt`. A first campaign taken partly
  on battery was discarded (decision D-029).
