# NOVA-KV

A single-node, in-memory key-value store with RCU reads, built for
*Design Optimization of Computing Systems — Assignment 2* (`assignment2.pdf`).

## Implementation status

| Milestone | Scope | Status |
|---|---|---|
| A0 | Build system, public API, reference workload import, verification scripts | **Done** (`reports/milestones/A0.md`) |
| A1 | Baseline `KVStore` (own hash table) + single / global-lock / rwlock variants | Not started |
| A2 | Storage-layout microbenchmark, lock profiling, Part A report | Not started |
| B1–B2 | RCU version roots, epoch-based reclamation | Not started |
| C1–C2 | Transactions, snapshots, secondary index, join | Not started |
| D | Benchmark campaign | Not started |
| E | Write-ahead log (optional bonus) | Not started |

**The store currently stores nothing.** `include/kvstore.h` declares the complete Appendix A
API so the supplied workload generator compiles unchanged, but `src/kvstore.cpp` is a
placeholder: `put`, `erase`, `create_index` and `Transaction::commit` return `false`, lookups
miss, snapshots are inert. No assignment part is implemented yet and no performance numbers
exist.

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
| `nova_workload` | The instructor's `workload_gen.cpp`, compiled unmodified |
| `api_conformance_test` | Compile-time pin of every Appendix A signature |
| `smoke_test_<variant>` | Empty-store behaviour and handle lifecycles, per variant |
| `reference_workload_link_test_<variant>` | Links and runs the unmodified workload generator (tiny config; put, transaction and join paths) against each variant. A compatibility check, not a benchmark. |

## Repository layout

```text
include/kvstore.h      public API (assignment Appendix A.1)
src/                   store implementation
supplied/              instructor workload generator (see supplied/PROVENANCE.md)
tests/                 tests
drivers/               single_thread / global_lock / rwlock / rcu drivers (from A1)
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
