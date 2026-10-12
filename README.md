# NOVA-KV

A single-node, in-memory key-value store with RCU reads, built for
*Design Optimization of Computing Systems — Assignment 2* (`assignment2.pdf`).

## Implementation status

| Milestone | Scope | Status |
|---|---|---|
| A0 | Build system, public API, reference workload import, verification scripts | **Done** (`reports/milestones/A0.md`) |
| A1 | Baseline `KVStore` (own hash table + value arena) with single / global-lock / rwlock variants, tests, drivers | **Done** (`reports/milestones/A1.md`) |
| A2 | Storage-layout microbenchmark, lock profiling, Part A report | Not started |
| B1–B2 | RCU version roots, epoch-based reclamation | Not started |
| C1–C2 | Transactions, snapshots, secondary index, join | Not started |
| D | Benchmark campaign | Not started |
| E | Write-ahead log (optional bonus) | Not started |

**What works:** `put`, `get`, `erase` (Part A), under three synchronization policies.

**What does not:** everything from Parts B and C. Those methods are declared in
`include/kvstore.h` only so the supplied workload generator compiles unchanged; they are inert
and never report success (`Transaction::commit` and `create_index` return `false`, `index_get`
returns 0, snapshots pin nothing, `set_reclaim_batch` does nothing). There is no RCU, no
reclamation protocol and no durability. No benchmark or profiling results exist yet.

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
| `nova_kv_single`, `nova_kv_global_lock`, `nova_kv_rwlock` | The store, compiled from the same sources once per synchronization policy |
| `nova_kv_single_testhooks` | Test-only build with allocation-failure injection compiled in |
| `nova_workload` | The instructor's `workload_gen.cpp`, compiled unmodified |
| `single_thread`, `global_lock`, `rwlock` | Part A drivers: `<driver> [num_readers] [duration_sec]` (defaults 8 and 5; `single_thread` always uses 1 reader) |

Tests (`ctest`, 18 per build mode):

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

The drivers and the reference workload print throughput figures. With the supplied generator
those are **preload-only** (no writer runs while readers are measured) and the ctest runs last
one second in whatever build mode is being tested; they are functional checks, not results.

## Repository layout

```text
include/kvstore.h      public API (assignment Appendix A.1)
src/                   store implementation
supplied/              instructor workload generator (see supplied/PROVENANCE.md)
tests/                 tests
drivers/               single_thread / global_lock / rwlock drivers (rcu from Part B)
benchmarks/            benchmark harnesses (from A2)
scripts/               verification and environment scripts
docs/                  assignment_contract.md, decisions.md
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
