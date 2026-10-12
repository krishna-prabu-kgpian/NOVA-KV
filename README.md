# NOVA-KV

A single-node, in-memory key-value store with RCU reads, built for
*Design Optimization of Computing Systems — Assignment 2* (`assignment2.pdf`).

## Implementation status

| Milestone | Scope | Status |
|---|---|---|
| A0 | Build system, public API, reference workload import, verification scripts | **Done** (`reports/milestones/A0.md`) |
| A1 | Baseline `KVStore` (own hash table + value arena) with single / global-lock / rwlock variants, tests, drivers | **Done** (`reports/milestones/A1.md`) |
| A2 | Active-writer benchmark, lock profiling (perf stat, flame graphs, perf c2c), NSM/DSM/PAX microbenchmark, Part A report | **Done** (`reports/milestones/A2.md`) — Part A complete |
| B1 | Immutable versions, copy-on-write index, atomic root, lock-free reads; replaced versions retained (no reclamation) | **Done** (`reports/milestones/B1.md`) |
| B2 | Epoch-based reclamation, background cleaner, `set_reclaim_batch`; Part B README, compliance checklist, A→B diff | **Done** (`reports/milestones/B2.md`) — Part B complete |
| C1–C2 | Transactions, snapshots, secondary index, join | Not started |
| D | Benchmark campaign | Not started |
| E | Write-ahead log (optional bonus) | Not started |

**What works:** `put`, `get`, `erase` and `set_reclaim_batch` under four synchronization
policies: the three Part A lock policies and `rcu` (Part B), where reads take no lock, every
write publishes an immutable version with one atomic pointer swap, and replaced versions are
freed by epoch-based reclamation on a background thread.

**What does not yet:** everything from Part C is an inert declaration (`Transaction::commit`
and `create_index` return `false`, `index_get` returns 0, snapshots pin nothing). There is no
durability. Part D's benchmark campaign has not been run.

## Design of the Part A store

```text
key --hash--> partition (256) --> HashSegment: flat array, linear probing
                                    slot = { key, ValueRef, state }        24 bytes
                                    ValueRef = { chunk_id, offset, length }
                                         |
            chunk directory (2-level, chunk_id -> Chunk*) --> Chunk: 64 KiB, append-only bytes
```

- **Index:** 256 partitions chosen by the low hash bits; each is one open-addressed table
  (initially 1024 slots, allocated on first use) probed linearly from the next hash bits.
  Erase leaves a tombstone. A segment is replaced by a privately built array when occupancy
  (live + tombstones) would exceed 70 % (double, or same size if tombstones ≥ 20 %) and when
  tombstones reach 20 % after an erase.
- **Values:** the index never holds value bytes. They are appended to 64 KiB chunks; a value
  larger than a chunk gets a dedicated chunk; a zero-length value uses a sentinel reference
  and no arena space. Stored bytes are never overwritten: an overwrite appends and repoints.
- **Chunk lifetime (Part A only):** each chunk counts the index entries pointing into it; a
  sealed chunk with no entries is freed and its ID reused. There is no compaction.
- **Failure atomicity:** `put` performs every allocation before its first index change, so a
  failed `put` returns `false` and leaves the store exactly as it was.
- **Synchronization:** `single` (none), `global_lock` (`std::mutex` on every operation),
  `rwlock` (`std::shared_mutex`, shared for `get`). Same sources; only `src/sync_policy.h`
  differs in effect.

API contract choices where the assignment is silent (`docs/decisions.md` D-012, D-018):

| Case | Behaviour |
|---|---|
| `get` buffer smaller than the value | `false`, buffer not modified |
| `get` buffer larger than the value | copies exactly the value; the length is not reported |
| zero-length value | stored; `get` returns `true` (even with `nullptr`, 0); distinct from a missing key |
| `nullptr` with length > 0 | `false` (`put` and `get`) |
| value longer than 1 GiB | `put` returns `false` |
| allocation failure in `put` | `false`, nothing changed |

## Requirements

- Linux, GCC with C++17 (developed with GCC 15.2), `make`, POSIX threads
- CMake ≥ 3.16. If it is not installed and you have no root access:
  `scripts/bootstrap_tools.sh` installs a project-local copy into `.tools/venv`
  (needs `python3 -m venv` and network access to PyPI).
- No third-party libraries.

## Build and test

```bash
scripts/bootstrap_tools.sh                 # only if cmake is missing
export PATH="$PWD/.tools/venv/bin:$PATH"   # only if you used the line above

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

Other configurations:

| Build dir | Configure flags | Purpose |
|---|---|---|
| `build/debug` | `-DCMAKE_BUILD_TYPE=Debug` | `-O0 -g` |
| `build/release` | `-DCMAKE_BUILD_TYPE=Release` | `-O3 -g -DNDEBUG`, performance builds |
| `build/asan` | `-DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_ASAN_UBSAN=ON` | AddressSanitizer + UBSan |
| `build/tsan` | `-DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_TSAN=ON` | ThreadSanitizer (separate from ASan) |

Run every gate in all four configurations from clean build directories:

```bash
scripts/verify_all.sh
```

It exits nonzero unless every mandatory gate passes and writes a gate table, full logs, the
environment description and a source manifest to `results/verification/<git-sha>/`.

## Targets

| Target | What it is |
|---|---|
| `nova_kv_single`, `nova_kv_global_lock`, `nova_kv_rwlock` | The Part A store, compiled from the same sources once per lock policy |
| `nova_kv_rcu` | The Part B store (same `KVStore` class and files, `rcu` policy) |
| `nova_kv_rcu_testhooks` | Test-only build with allocation-failure injection and pause points |
| `nova_kv_single_testhooks` | Test-only build with allocation-failure injection compiled in |
| `nova_workload` | The instructor's `workload_gen.cpp`, compiled unmodified |
| `single_thread`, `global_lock`, `rwlock`, `rcu` | Drivers for the supplied generator: `<driver> [num_readers] [duration_sec] [m] [n]` (defaults 8, 5, 1000, 100; `single_thread` always uses 1 reader) |
| `concurrent_bench_{single,global_lock,rwlock,rcu}` | Project benchmark with a writer active during the measured window (`benchmarks/concurrent_bench.cpp`) |
| `layout_bench` | NSM / DSM / PAX microbenchmark (`benchmarks/layout_bench.cpp`) |

Tests (`ctest`, 47 per build mode):

| Test | What it checks |
|---|---|
| `api_conformance_test` | Compile-time pin of every Appendix A signature |
| `hash_segment_test` | One index partition in isolation with an identity hash: exact collision chains, erase in the middle of a chain, no duplicate past a tombstone, wraparound, growth, tombstone rebuild, 200k random operations against `std::unordered_map` |
| `arena_test` | Zero/one-byte values, exact chunk fit, oversized values, stable addresses, shared-chunk lifetime, chunk free and ID reuse, writable-chunk reset, > 3000 chunks across directory blocks, directory exhaustion |
| `alloc_failure_test` | Every allocation of a scripted workload is made to fail in turn; a failed `put` must change nothing, a failed rebuild must not fail an `erase` |
| `kvstore_test_<variant>` | The Part A sanity test through the public API: insert/lookup/overwrite/erase, key extremes, zero-length values, invalid pointers, undersized buffers, binary and variable-length values up to 1 MiB, real hash collisions, 300k keys with growth and mass erase, overwrite churn with bounded memory, 570k randomized operations against `std::unordered_map` |
| `concurrent_lock_test_{global_lock,rwlock}` | One writer and four readers on self-validating records: no torn record, final state equals the writer's log; the data-race check when run under TSan |
| `smoke_test_<variant>` | Empty-store behaviour, handle lifecycles, variant wiring |
| `reference_workload_link_test_<variant>` | Unmodified generator links and runs; every trade it loaded with plain puts is read back intact |
| `driver_<name>` | Each driver runs end to end for 1 second |
| `pinned_arena_test` | Part B arena: one reference count per chunk, last reference frees and gates ID reuse, the commit reference that closes audit finding F2, rollback, lock-free ID pool and directory under a concurrent releasing thread |
| `epoch_test` | Gated epoch advance, reader slots without a fixed limit, slot reuse after thread exit, a thread outliving its store, registration racing with scans |
| `reclamation_test` | Deterministic schedules for reclamation: reader parked at each step of its entry, writer parked around retirement, cleaner parked mid-scan while a reader registers, both audited chunk schedules, batch limits 1–1024, resumable destruction, a long-lived pin moved between threads, comparator modes |
| `reclamation_stress_test` | Background cleaner at batch 1/8/64/1024 with readers, a writer, pins and thread churn: no torn or stale read; memory stops growing; 200 construct/destroy cycles |
| `cow_segment_test` | Immutable segments: the base is byte-for-byte untouched by a builder, exact pin sets, growth, empty result, abandoning a build |
| `versioned_store_test` | One publication per logical write, one clone per touched segment, sharing of the rest, every retained version unchanged (logically and byte-for-byte), ownership counts, reclamation after every write against a reference model |
| `versioned_alloc_failure_test` | Every allocation of each write fails in turn: nothing published, arena rolled back, content equals the model, counts consistent |
| `versioned_interleaving_test` | Deterministic schedules with pause points: reader parked across 3051 publications, writer parked before/after the root swap, plus a bounded atomic-visibility stress |
| `kvstore_test_rcu`, `concurrent_lock_test_rcu`, `smoke_test_rcu`, `reference_workload_link_test_rcu`, `driver_rcu` | The variant-generic tests against the `rcu` build |
| `bench_validate_concurrent_rcu*` (6), `bench_rcu_retain_limit_invalidates_run` | Harness against the `rcu` build: default, paced, batch 8, single-thread, reclamation disabled, stop-the-world; the memory guard must invalidate a run |
| `bench_histogram_test` | The benchmark's latency histogram: bucket error bound, percentiles against a sorted vector, exact maximum, merging |
| `bench_validate_*` (7), `bench_rejects_concurrent_single` | Each harness runs briefly and must pass its own validity checks; the harness must refuse to run the unsynchronized variant concurrently |

The drivers and the reference workload print throughput figures. With the supplied generator
those are **preload-only** (no writer runs while readers are measured) and the ctest runs last
one second in whatever build mode is being tested; they are functional checks, not results.

## Part B store

- **Part B README (assignment deliverable):** `reports/part_b_readme.md`
- **Compliance checklist:** `docs/part_b_compliance.md`
- **Design and safety argument:** `docs/reclamation.md` (reclamation), `docs/versioning.md`
  (publication, ownership)
- **Part A → Part B source diff:** `results/diffs/part_a_to_part_b.diff`, or
  `git diff part-a-final part-b-final -- src include`
- **Measurements:** `results/b2/` (`scripts/run_b2_observation.sh`); earlier B1 set in
  `results/b1/`

In short: `current` is an atomic pointer to an immutable `Version` holding 256 segment
pointers; a write clones only the segments it touches, shares the rest, and swaps the pointer
once. A reader announces an epoch in its own slot, loads the pointer once, reads, and clears
the slot. A cleaner thread frees a replaced version only after the epoch has advanced twice
past its retirement tag, at most `set_reclaim_batch(n)` versions per pass.

```bash
build/release/rcu 8 5                         # supplied generator against the RCU store
build/release/concurrent_bench_rcu --readers 8 --duration 3 \
    --reader-cpus 0,2,4,6,1,3,5,7 --writer on --writer-cpu 15 --reclaim-batch 64
scripts/run_b2_observation.sh                 # the B2 measurement set -> results/b2/
```

**Reading Part A against RCU numbers.** The `rcu` build differs from the Part A variants in
storage and update strategy, not only in synchronization: a Part A write changes one slot in
place, an RCU write copies the segment it touches and the directory. A difference between,
for example, `rwlock` and `rcu` compares two whole designs and is not the cost of the lock
alone (`docs/decisions.md` D-038). Machine conditions also differed between milestones; only
numbers taken in one session are comparable (D-046).

## Part A results and how to reproduce them

- **Report (4 pages):** `reports/part_a_report.pdf` (source `reports/part_a_report.src.md`).
- **Requirement-by-requirement checklist:** `docs/part_a_compliance.md`.
- **Raw data and profiles:** `results/part_a/`

| Path under `results/part_a/` | Content |
|---|---|
| `environment.txt`, `campaign_config.txt`, `conditions.log` | Machine, toolchain, exact compile command, run parameters, power source and governor at each stage |
| `concurrent/runs.csv`, `concurrent/hist/`, `concurrent/perf_stat/`, `concurrent/stdout/` | Active-writer benchmark: one row, one latency histogram and one `perf stat` output per run (90 runs) |
| `reference/` | Unmodified supplied generator, **preload-only** (45 runs) |
| `layout/runs.csv`, `layout/perf_stat/` | NSM / DSM / PAX microbenchmark and its hardware counters |
| `flamegraphs/` | 8 flame graphs (`.svg`), folded stacks, compressed `perf script` output, exact commands |
| `c2c/` | `perf c2c` cache-line contention reports, 8 configurations |
| `summary/`, `figures/` | Tables and figures generated from the above by `scripts/aggregate_part_a.py` |

```bash
scripts/bootstrap_tools.sh --profiling          # FlameGraph (pinned commit), matplotlib, cairosvg
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j
scripts/run_part_a.sh all                       # about 11 minutes; rewrites results/part_a/
scripts/build_part_a_report.sh                  # aggregate, render the PDF, check page limits
scripts/check_part_a_artifacts.sh               # every Part A deliverable present?
```

`run_part_a.sh` needs `perf` usable without root (`kernel.perf_event_paranoid <= 1`) and pins
threads to the CPU numbering of the development machine; set `READER_CPUS` / `WRITER_CPU` for
another machine. The report needs `pandoc` and `xelatex`. Re-running produces new measurements;
the committed files are the ones the report was written from.

## Repository layout

```text
include/kvstore.h      public API (assignment Appendix A.1)
src/                   store implementation (Part A engine + Part B versioned engine)
supplied/              instructor workload generator (see supplied/PROVENANCE.md)
tests/                 tests
drivers/               single_thread / global_lock / rwlock drivers (rcu from Part B)
benchmarks/            benchmark harnesses: concurrent_bench, layout_bench
scripts/               verification and environment scripts
docs/                  assignment_contract.md, decisions.md, versioning.md, reclamation.md, part_a_compliance.md, part_b_compliance.md
reports/milestones/    one report per milestone
results/               evidence (verification logs, raw benchmark data, figures)
NOVA_KV_*.md           project specification documents (authoritative, unmodified)
assignment2.pdf        the assignment
CLAUDE.md              standing instructions for the coding agent
```

## Supplied files

`supplied/workload_gen.cpp` is the instructor's file, byte-for-byte
(SHA-256 `58c3c2c8f5203bffc20198515c9065d33dd46fd8cfc89fd83e610eda3ad08508`).
`supplied/workload_gen.h` was not provided as a file and is transcribed from the listing in
the PDF (Appendix A.2, page 10). Neither may be edited; `supplied/SHA256SUMS` is checked on
every verification run.

Note that the supplied generator loads all data before it starts any reader thread, so its
numbers are *preload-only* and are not, on their own, the active-writer measurements Parts A
and D ask for. See `docs/assignment_contract.md` §4 for the full list of discrepancies.
