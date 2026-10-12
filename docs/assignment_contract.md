# Assignment contract (extracted from `assignment2.pdf`)

This file records what the assignment itself requires, with page references, so that
requirements are never confused with this project's design choices. It is an index into the
PDF, not a replacement for it. Design choices live in `docs/decisions.md` and in the
blueprint.

Assignment: Design Optimization of Computing Systems, Assignment 2 — NOVA-KV. Deadline
25 October 2026, 11:59 PM. 200 marks + 20 bonus. Team size 2–4. Build with `-std=c++17` or
later. No existing concurrent hash-map library. Single process, exactly one writer thread.

## 1. Public API (Appendix A.1, PDF pp. 8–10)

Declared in `include/kvstore.h`, pinned at compile time by `tests/api_conformance_test.cpp`.

| Part | Signature | PDF note |
|---|---|---|
| A | `KVStore(); ~KVStore();` | |
| A | `KVStore(const KVStore&) = delete; KVStore& operator=(const KVStore&) = delete;` | Move operations left undeclared; the user-declared destructor suppresses them. |
| A | `bool put(long key, const void* value, size_t value_len);` | |
| A | `bool get(long key, void* value_out, size_t value_out_len) const;` | "returns true if found (value copied into value_out)" |
| A | `bool erase(long key);` | "returns true if the key was present and is now removed" |
| B | `void set_reclaim_batch(int n);` | "sets how many retired versions one reclamation pass may free"; declared alongside put/get/erase; backed by a private member. |
| C | `bool create_index(size_t field_offset, size_t field_len);` | Secondary index over a byte range within each stored value. |
| C | `int index_get(const void* field_value, size_t field_len, long* keys_out, int max_keys) const;` | Returns the number of matches (up to `max_keys`). |
| C | `class Snapshot` with `Snapshot(Snapshot&&) noexcept; Snapshot& operator=(Snapshot&&) noexcept; ~Snapshot();` and deleted copies | Listed under "added to KVStore". Opaque, move-only; pins its version for its whole lifetime. |
| C | `Snapshot acquire_snapshot() const;` | Pinned to the current version. |
| C | `bool get(const Snapshot& snap, long key, void* value_out, size_t value_out_len) const;` | |
| C | `int index_get(const Snapshot& snap, const void* field_value, size_t field_len, long* keys_out, int max_keys) const;` | |
| C | `size_t keys(const Snapshot& snap, long* keys_out, size_t max_keys) const;` | Enumerates up to `max_keys` primary keys; returns the number written. |
| C | `class Transaction { explicit Transaction(KVStore& store); ~Transaction(); /* deleted copies */ void put(long key, const void* value, size_t value_len); bool commit(); };` | Destructor discards uncommitted writes; `commit()` is true if committed, all staged writes visible in one publish. |

Semantics fixed by the PDF text:

- `get()` copies the value out before returning, while the read-side critical section (or the
  Snapshot's pin) is still open; the same copy-out convention applies to `index_get` (p. 10).
- A single `get()`, `erase()` or `index_get()` acquires a snapshot internally and releases it
  on return (pp. 4, 9).
- Neither `KVStore` nor `Transaction` is copyable or movable (p. 10).

Not specified by the PDF (policies chosen in blueprint §2.2, to be implemented and tested in
the milestone that introduces each): buffer-too-small behaviour, zero-length values,
`value == nullptr`, repeated or empty `commit()`, `set_reclaim_batch(n <= 0)`,
`acquire_snapshot()` failure, using a Snapshot with a different store.

## 2. Deliverables by part

| Part | Marks | Required artifacts | PDF |
|---|---:|---|---|
| A | 50 | `kvstore.h`/`kvstore.cpp` (own hash table); `kvstore_test.cpp` (insert, lookup, overwrite, erase); `single_thread.cpp`, `global_lock.cpp`, `rwlock.cpp` sharing one data structure; `perf stat` and `perf record`/`perf script` + flame graphs for global_lock and rwlock at 1/2/4/8 readers with one background writer; layout design note (≤ 3 pages) with a microbenchmark comparing two of NSM/DSM/PAX; report ≤ 5 pages | pp. 2–3 |
| B | 40 | RCU inside the same `KVStore` class (no forked implementation): off-side version build, one atomic pointer swap, exactly one atomic load of the current pointer per read; EBR or hazard pointers; `set_reclaim_batch`; bounded reclamation pass that never blocks a reader or the writer; diff against Part A; `README.md`; stress test showing no torn rows | p. 4 |
| C.1 | 30 | `Transaction` with one-publish commit and discard-on-destroy; atomicity and isolation requirements; design note: isolation level, write skew, what leaving durability out costs | pp. 4–5 |
| C.2 | 30 | Secondary index on a non-key column of a second table; index-nested-loop join that acquires **exactly one Snapshot** and passes it to every `keys()`, `index_get()` and `get()`; outer loop over `keys()`; diff against Part B; design note | p. 5 |
| D.1 | 17 | Point-get RPS and p50/p99/p999 for single_thread, global_lock, rwlock and RCU at 1/2/4/8 readers with one background writer; where each lock config stops scaling; 100k+ RPS check at 8 readers (or the achieved number plus a flame-graph-backed explanation) | pp. 5–6 |
| D.2 | 17 | ≥ 4 reclaim-batch settings: sustained read RPS and longest single reader stall; plus reclamation-disabled and naive stop-the-world reference points | p. 6 |
| D.3 | 16 | Join throughput/latency at 8 readers with writer active vs paused; commit throughput at 1/4/16 keys per transaction; bounded vs stop-the-world worst-case pause; a workload where COW hurts | p. 6 |
| E | +20 | WAL appended before/as part of publish; replay on restart; no per-read overhead; fsync/group-commit note; D.1 write-side numbers with and without WAL; half a page on production gaps | pp. 6–7 |

Submission: one ZIP named `Asgn2_<RollNumber1>_<RollNumber2>[_<RollNumber3>][_<RollNumber4>].zip`,
uploaded to Moodle, one per team (p. 7). Roll numbers have not been provided yet.

## 3. Supplied workload generator (Appendix A.2, PDF pp. 10–11)

- `workload_gen.cpp` was supplied as a file and is preserved unchanged in `supplied/`.
- `workload_gen.h` is printed in the PDF (p. 10) and was **not** supplied as a file; it is
  transcribed in `supplied/workload_gen.h` (see `supplied/PROVENANCE.md`).
- Defaults: `m=1000, n=100, hot_every=100, num_readers=8, duration_sec=5, use_txn=false,
  txn_size=1, use_join=false`.

## 4. Discrepancies between the PDF text and the supplied source

These were confirmed by reading `supplied/workload_gen.cpp` in A0. None is resolved by editing
the supplied file.

| # | Discrepancy | Consequence / handling |
|---|---|---|
| 1 | `workload_run()` runs `run_writer()` to completion **before** starting any reader thread (lines 106–116). Parts A and D require one background writer active throughout. | Supplied-generator results are labelled *reference / preload-only*. Active-writer measurements need our own harness (`benchmarks/concurrent_bench.cpp`, from A2). |
| 2 | Inserted keys are `i*100000 + j` for `i < m`, `j < n` (line 30), but the 20 % random branch draws from `rng() % (m*n)` (line 77). With the defaults only keys `0..99` of `0..99999` exist, so about 99.9 % of random-branch lookups miss. | Preserve; record hit/miss ratio in supplementary runs; add a uniform-hit workload in our harness. |
| 3 | `std::optional<Transaction>` is instantiated unconditionally (line 40), so a complete `Transaction` type is needed even for a Part A build with `use_txn=false`. | `include/kvstore.h` declares the full A+B+C API from A0 (decision D-003). |
| 4 | With `use_join=true` a reader only calls `instruments->index_get()` (single-call overload) after a `get()` (lines 81–84). That is not the snapshot-scoped index-nested-loop join of C.2. | A separate compliant join benchmark is required (C2/D). |
| 5 | The join uses two `KVStore` instances (`trades`, `instruments`), while C.2 demands exactly one `Snapshot`, and a `Snapshot` is acquired from one store. | **OPEN / INSTRUCTOR.** Not resolved. Two per-store snapshots give per-store consistency only and must never be reported as one cross-store snapshot. |
| 6 | D.2 asks for the longest reader stall; `WorkloadResults` has no max field. | Our harness keeps full latency samples and the maximum. |
| 7 | `run_writer()` calls `txn->commit()` once more after the loop (line 60), which is a commit of an empty transaction whenever `m*n` is a multiple of `txn_size`. | The first commit of an empty transaction must succeed without publishing (blueprint §2.2); implemented in C1. |
| 8 | `load_instruments()` runs after `run_writer()` and calls `create_index()` before inserting any instrument (line 91). | `create_index` must work on an empty store and index later inserts. |
| 9 | The supplied `.cpp` has 150 newline-terminated lines (`wc -l`); the blueprint says 151. | Cosmetic. The file's SHA-256 is the reference, not the line count. |
| 10 | Latencies are stored per operation in unbounded `std::vector<double>`s. With a very fast store this is large (≈430 MB peak RSS for two 1-second, 2-reader runs against the A0 placeholder). | Not a defect to fix; note memory use when running the reference workload at 8 readers × 5 s. |
