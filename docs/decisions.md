# Decision log

Project decisions that are **not** dictated by the assignment PDF. Each entry states what was
chosen, why, and what it costs. Architecture decisions already fixed by the blueprint are not
repeated here; only choices the blueprint left to the implementer, and any departure from it.

Status values: `ACCEPTED` (in effect), `OPEN` (needs the user or the instructor).

---

## D-001 — Specification documents and PDF stay at the repository root (ACCEPTED, A0)

The five `NOVA_KV_*.md` documents and `assignment2.pdf` are kept where they were received and
are committed unchanged (commit `a3cccd8`). Blueprint §8.1 sketches `docs/BLUEPRINT.md`; the
documents refer to each other by their root filenames, so they are not moved or renamed.
`CLAUDE.md` indexes them.

## D-002 — Supplied generator lives in `supplied/` (ACCEPTED, A0)

`workload_gen.cpp` was moved from the root to `supplied/workload_gen.cpp` with `git mv`, per
blueprint §8.1. Content is unchanged (SHA-256 pinned in `supplied/SHA256SUMS`, checked by
`scripts/verify_all.sh`). `workload_gen.h` was not supplied as a file and is transcribed from
PDF Appendix A.2; see `supplied/PROVENANCE.md`. The assignment's flat top-level file names
(`kvstore.h`, `kvstore.cpp`, drivers) will be produced by the packaging step in the final
milestone.

## D-003 — Final-compatible public header from the first milestone (ACCEPTED, A0)

Blueprint §8.2 requires a decision on how the unmodified `workload_gen.cpp`, which needs a
complete `Transaction` type, compiles against a Part A store. Chosen: `include/kvstore.h`
declares the complete Appendix A surface (Parts A, B and C) from A0, and `kvstore.cpp` always
defines every declared symbol.

- Until a part is implemented its methods are inert and report failure/"not found"; they never
  report success for work that was not done. In particular a Part A build does **not** claim
  to implement Part C transactions (audit H02).
- Cost: the Part A→B and B→C header diffs will show only private-member changes, since the
  public declarations already exist. The source diffs still show each part's implementation.
- Alternative rejected: a separate "compat" header per part. It would mean the graded header
  differs from the one the tests pin.

## D-004 — `Snapshot` is the nested type `KVStore::Snapshot` (ACCEPTED, A0)

Appendix A.1 introduces `class Snapshot` under the comment "added to KVStore", and the
neighbouring declarations (`acquire_snapshot`, the snapshot overloads) are members. It is
therefore declared as a nested public class. No namespace-scope alias is provided because
nothing supplied needs one (audit H04: "publish type alias only if strictly needed").
`Transaction` is a separate namespace-scope class, as printed.

## D-005 — No `noexcept` on Appendix A functions (ACCEPTED, A0)

In C++17 `noexcept` is part of a function's type. To keep the declarations identical to the
printed contract, no public method carries `noexcept` except the two `Snapshot` move
operations, which the PDF prints as `noexcept`. Allocation failures will be caught at the
public boundary and translated to the documented return value (blueprint §2.2).

## D-006 — Pimpl for private state (ACCEPTED, A0; revisit with data)

`KVStore` holds `Impl* impl_`; `Transaction` holds `KVStore& store_` and `Impl* impl_`;
`Snapshot` holds `const KVStore* owner_` and an opaque `void* pin_`. This keeps the public
header free of implementation types and identical across sync variants.

- Cost: one extra pointer dereference per call. The pointer is immutable after construction,
  so it adds no synchronization. If profiling in Part B/D shows it matters on `get()`, state
  can be moved inline behind the same public API.
- Part B asks for "a private member controlling how many retired versions a reclamation pass
  frees". With pimpl that member would live in `Impl`; whether it should instead be visible
  as a private member in `kvstore.h` will be decided in B2.

## D-007 — One library per synchronization policy, selected at compile time (ACCEPTED, A0)

`src/kvstore.cpp` is compiled into `nova_kv_single`, `nova_kv_global_lock` and
`nova_kv_rwlock`, differing only in `NOVA_KV_SYNC_MODE` (`src/sync_mode.h`). An `rcu` variant
is added in Part B. This satisfies "same underlying data structure across all three drivers,
only the synchronization should differ" (PDF p. 2) and blueprint §11.8, with no runtime
dispatch on the read path. `nova_kv_sync_mode_name()` (internal, `src/build_info.h`) lets
tests and harnesses verify which variant a binary was linked with.

In A0 the three variants are behaviourally identical placeholders; the locking is added in A1.

## D-008 — Build flags (ACCEPTED, A0)

- Release: `-O3 -g -DNDEBUG`. Blueprint §9.3 specifies `-std=c++17 -O3 -g -pthread`;
  `-DNDEBUG` is added so `assert()` cannot put work on the measured hot path. Tests use
  `NOVA_CHECK`, which is independent of `NDEBUG`, so release-mode tests still check.
- Debug: `-O0 -g`.
- `-fno-omit-frame-pointer` in every mode, so `perf record -g` call graphs are usable.
- Project sources: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`. The supplied
  workload is compiled without the warning set because it cannot be edited.
- `NOVA_ENABLE_ASAN_UBSAN` and `NOVA_ENABLE_TSAN` are mutually exclusive CMake options
  (runbook §3 names).

## D-009 — Dependency-free test runner (ACCEPTED, A0)

Tests use a ~40-line header (`tests/test_util.h`) and CTest, not GoogleTest/Catch2. Reasons:
the build must work from a clean extraction with no network access (submission checklist), and
the assignment discourages external libraries in the store. Test-only scheduling hooks for the
EBR interleaving tests will be added in B2.

## D-010 — Project-local CMake (ACCEPTED, A0)

The development machine has no `cmake` and no passwordless `sudo`. `scripts/bootstrap_tools.sh`
installs `cmake==3.31.6` from PyPI into `.tools/venv` (git-ignored, removable by deleting the
directory). `CMakeLists.txt` requires only CMake ≥ 3.16, so a distribution cmake works too.
This is a tooling workaround, not a project dependency; the README states it.

## D-011 — Git workflow (ACCEPTED, A0)

`main` holds the import of the original materials. Milestone A0 was developed on the branch
`chore/bootstrap` (blueprint §8.2 names the checkpoint `chore/bootstrap`, tag `a0-bootstrap`).
Milestone evidence is committed in a second commit after the code commit, because a report
cannot contain the SHA of the commit that adds it; the tag points at the evidence commit.

## D-012 — Public error contracts confirmed by the user (ACCEPTED, 2026-10-12, before A1)

The user confirmed the blueprint §2.2 policies as the contract for A1 onward:

- If the output buffer is too small, `get()` returns `false` **without modifying the buffer**.
- Zero-length values are supported. An existing zero-length value is distinguishable from a
  missing key: `get()` returns `true` for it (also with `value_out == nullptr`,
  `value_out_len == 0`) and `false` for a missing key.
- `nullptr` with length > 0 is invalid and returns `false` (for both `put` and `get`).
- Exact compatibility with the assignment's public API is preserved.

## D-013 — Profiling environment deferred (ACCEPTED, 2026-10-12, user decision)

- perf permissions and the FlameGraph installation are deferred until the profiling milestone
  (A2).
- Mandatory profiling deliverables are never silently skipped: when profiling becomes
  necessary the limitations are documented and solutions proposed.
- **No `sudo` and no kernel-setting change without the user's explicit permission.**

## D-014 — Git continuation (ACCEPTED, 2026-10-12, user decision)

- The `a0-bootstrap` tag is preserved.
- Development continues from the verified A0 checkpoint: branch `part-a/core` starts at
  `a0-bootstrap`.
- Nothing is merged into `main` yet.
- The completed A1 milestone is committed and tagged `part-a-core`.

## D-015 — Cross-store snapshot ambiguity stays open (ACCEPTED, 2026-10-12, user decision)

The exactly-one-Snapshot-across-two-stores ambiguity (O-1) stays documented and is **not**
addressed in A1.

## D-016 — A1 storage engine: in-place mutation under the store lock (ACCEPTED, A1)

Blueprint §8.3: "Do not introduce RCU yet; show the simpler data structure deliberately."
A1 implements the agreed architecture (256 hash partitions, one open-addressed linear-probing
segment each, tombstones, private rebuilds, chunked append-only value arena, `ValueRef =
(chunk_id, offset, length)`, global two-level chunk directory) but **mutates segments in
place**, protected by the variant's lock. There is no Version root, no copy-on-write directory
and no reclamation protocol; those are Part B.

- Rebuilds and growth are still built privately and installed only after every fallible step
  succeeded, so a failed `put` changes nothing.
- Value bytes are never overwritten in place: an overwrite appends the new bytes and repoints
  the entry, exactly as Part B will.

## D-017 — A1 chunk lifetime: per-chunk entry count (ACCEPTED, A1; replaced in Part B)

Without any reclamation an update-heavy baseline would grow without bound. A1 therefore keeps
`live_refs` per chunk = number of index entries whose `ValueRef` points into it, maintained by
the writer under the exclusive lock:

- when the count of a sealed chunk reaches zero the chunk is freed, its directory slot is
  nulled and its ID is pushed on the free-ID stack (reused by the next chunk allocation);
- when the count of the *current writable* chunk reaches zero its write offset is reset to 0.

This is safe in A1 only because every reader holds the store lock (or there is one thread);
no reader can hold a `ValueRef` across a writer operation. It is **not** the Part B scheme:
blueprint §3.4 (one pin per distinct chunk per segment, released after an EBR grace period)
replaces it, and the reset-in-place of the writable chunk must be removed there. There is no
compaction: a chunk with one live value keeps its whole allocation.

## D-018 — A1 parameters and limits (ACCEPTED, A1; blueprint defaults unless noted)

| Parameter | Value | Note |
|---|---|---|
| Partitions | 256 | blueprint default |
| Initial segment capacity | 1024 slots, allocated lazily on the first insert into the partition | lazy allocation is an addition: an empty store costs no index memory |
| Max occupied load (live + tombstones) | 0.70 | blueprint default |
| Tombstone rebuild trigger | tombstones ≥ 20 % of capacity | blueprint default |
| Growth rule on an insert that would exceed the load | tombstones ≥ 20 % → rebuild at the same capacity; otherwise double | hysteresis so every rebuild buys at least 0.2 × capacity further operations |
| Shrinking | never | segments do not shrink after mass erase |
| Arena chunk | 64 KiB | blueprint default |
| Oversized value | dedicated chunk of exactly the value's size | blueprint §3.6 |
| Maximum value length | 1 GiB (2^30 bytes); longer `put` returns `false` | chosen; `ValueRef` fields are 32-bit |
| Chunk directory | 1024 blocks × 1024 slots = 1,048,576 chunk IDs; exhaustion → `put` returns `false` | fixed capacity per blueprint §3.5 |
| Hash | 64-bit murmur3 finalizer of the key; low 8 bits pick the partition, next bits the start slot | fixed and documented in `src/hash.h` |
| Slot | 24 bytes: key, `ValueRef`, state | |

`get()` with a buffer larger than the value copies exactly the value's length and leaves the
rest of the buffer untouched; the API cannot report the length (blueprint §2.2).

## D-019 — Test-only allocation failure injection (ACCEPTED, A1)

All store allocations go through `nova::alloc_bytes` (`src/alloc.h`). When compiled with
`NOVA_KV_TEST_HOOKS` it can fail the N-th allocation. Only the extra library
`nova_kv_single_testhooks` is built with that macro; the three production variants contain no
hook code. Internal state is inspected by tests through `NovaKvInspector` (`src/inspect.h`), a
private `friend` of `KVStore`; it is not part of the public API.

## D-020 — Drivers (ACCEPTED, A1)

`drivers/single_thread.cpp`, `global_lock.cpp`, `rwlock.cpp` have the shape printed in PDF
Appendix A.2 and accept optional arguments `[num_readers] [duration_sec]`. `single_thread`
always uses one reader thread: the store has no synchronization in that variant, so a
multi-reader run would not be a valid single-threaded baseline (audit H17). With the supplied
generator these are *preload-only* runs; the active-writer harness is A2.

## D-021 — Tools for A2 (ACCEPTED, 2026-10-12, user decision)

- Missing tools may be installed by a project-local or unprivileged method. No `sudo`, no
  kernel-setting change without explicit approval.
- FlameGraph comes from the official repository `https://github.com/brendangregg/FlameGraph`,
  pinned to commit `41fee1f99f9276008b7cd112fca19dc3ea84ac32` (2024-10-21) in
  `.tools/FlameGraph` (git-ignored). `matplotlib==3.10.9` and `cairosvg==2.9.1` are installed
  into `.tools/venv`. Reproduce with `scripts/bootstrap_tools.sh --profiling`.
- Found already installed by the user (system packages): cmake 4.x at `/usr/bin/cmake`, pandoc
  3.7.0.2, TeX Live (`xelatex`), poppler (`pdfinfo`, `pdftotext`), perf 7.0.12.
- If a required tool cannot be installed the problem is reported and alternatives are
  investigated; profiling results are never fabricated.

## D-022 — perf configuration (ACCEPTED, 2026-10-12, user decision)

`kernel.perf_event_paranoid = 1` is the accepted setting and is not changed by the agent.
`kernel.kptr_restrict = 1` (also not changed) hides kernel symbols from an unprivileged perf:
kernel frames are resolved only as far as "in the kernel". See D-025.

## D-023 — Part B chunk ownership must be redesigned (ACCEPTED, 2026-10-12, user decision)

The A1 per-chunk entry count (D-017) is accepted **only** for the mutable, lock-protected
Part A store. It is not sufficient for immutable RCU versions: when Part B begins, chunk
ownership must be redesigned so that every retained version and every snapshot keeps the
chunks it can reach alive (blueprint §3.4: one pin per distinct chunk per published segment,
released only after the segment's owning version has passed its EBR grace period), and the
reset-in-place of the writable chunk must be removed. Part B is not started in A2.

## D-024 — A2 measurement method (ACCEPTED, A2)

Two tracks, never mixed (blueprint §9.1):

- **Reference / preload-only:** the unmodified supplied generator through
  `drivers/{single_thread,global_lock,rwlock}`. No writer runs while readers are measured.
- **Active writer:** `benchmarks/concurrent_bench.cpp`. Same schema, key layout, hot set and
  80/20 read mix as the generator. Readers and one writer thread start on a barrier; 0.5 s
  warm-up, then a 3 s measured window; the writer overwrites existing trades for the whole
  window. Two writer settings are measured: `writer_max` (as fast as the lock lets it) and
  `writer_paced` (10,000 updates/s). The random 20 % of reads uses keys that exist; a
  `--keys reference` option reproduces the generator's mostly-missing formula.

Choices that affect the numbers:

- Each `get` is timed with two `steady_clock` reads (as the supplied generator does) and
  recorded in a fixed-memory histogram (1 ns buckets below 1024 ns, ≤ 1.6 % wide above), with
  the exact maximum kept separately. Percentiles use the generator's rank rule. The clock
  reads are part of every measured operation in every configuration.
- Throughput = completed `get` calls in the window ÷ measured wall-clock window. Loading and
  warm-up are excluded.
- The writer builds each record with `snprintf` like the generator's `make_trade`, i.e. it
  does work outside the lock between updates.
- Threads are pinned. Readers get one logical CPU per physical core, fast cores first
  (`0,2,4,6,1,3,5,7`); the writer is on CPU 15, the SMT sibling of core 7. So the writer has
  its own physical core for 1/2/4 readers and shares one with the eighth reader, and readers
  5–8 run on the slower cores of this heterogeneous CPU. Scaling from 4 to 8 readers is
  therefore not expected to be linear even without any lock.
- 5 repetitions per configuration as separate processes, repetitions in the outer loop.
  Summaries give the median with minimum and maximum; no run is discarded.
- `perf stat` wraps each of those processes, so its counters include the 100,000-key load and
  the warm-up in addition to the window.
- The unsynchronized `single` variant is run with one thread only: read-only, and with the
  same thread doing one update per 100 reads.
- A run is rejected (nonzero exit) if a read returns a foreign record, if any expected key is
  missing, if the writer made no successful update in the window, or if a thread could not be
  pinned.

## D-025 — Flame graphs (ACCEPTED, A2)

`perf record -F 1999 --call-graph fp` on the `cycles` event (all binaries and Ubuntu's system
libraries keep frame pointers), 5 s window, one capture per lock variant × reader count with
the unthrottled writer. `stackcollapse-perf.pl` weights stacks by sample period, so widths are
shares of CPU cycles. These are **on-CPU** profiles: a thread asleep in a futex consumes no
cycles and is not sampled, so blocked time appears as lost throughput, not as width.

Because `kptr_restrict = 1`, kernel frames have no symbols. The pipeline renames them to
`[kernel]` and merges consecutive ones; no sample is added, dropped or moved, and the raw
`perf script` output is kept (xz-compressed). `DEBUGINFOD_URLS` is cleared for perf because
the network lookups made `perf script` take minutes.

## D-026 — Layout microbenchmark design (ACCEPTED, A2)

`benchmarks/layout_bench.cpp` builds NSM, DSM and PAX tables of the workload's 32-byte `Trade`
from the same data and drives them with the same pre-generated access sequences. Records are
addressed by record number in all three, so the hash index (identical for all layouts) is
excluded and only the layout differs. PAX uses 4096-byte pages of 128 records with four
1024-byte mini-pages. Two table sizes: 100,000 records (3.2 MiB, the workload's size, within
L3) and 8,000,000 records (244 MiB, far beyond L3). Six access patterns; a checksum of all
values read must be identical across layouts or the run fails. Repetitions are interleaved
across layouts; a warm-up pass is discarded. Hardware counters per get are obtained by
subtracting a setup-only run from a measured run.

The generic store stays schema-agnostic (blueprint §8.4): it stores opaque bytes and cannot
know columns, so the benchmark compares layouts of the record itself, outside the store.

## D-027 — A2 Git (ACCEPTED, A2)

Branch `part-a/final` starts at tag `part-a-core`. The completed Part A is tagged
`part-a-final`. `a0-bootstrap` and `part-a-core` are preserved; nothing is merged into `main`.

## D-028 — Record layout: NSM (ACCEPTED, A2, from measurements)

The store keeps each value as one contiguous run of bytes (row-major, NSM). Basis:
`results/part_a/summary/layout_summary.csv`. Whole-row point gets, the only read the Appendix A
API offers, cost 8.98 ns (NSM) against 16.42 ns (DSM) and 14.26 ns (PAX) per row at 8,000,000
records with independent accesses, and 5.49 against 13.35 and 11.40 ns with the workload's
80/20 mix; the generic cache-miss event count per row is 1.25 against 5.18 and 4.65. DSM is
faster for single-field reads and scans (0.21 vs 0.63 ns per value scanned), which the API
cannot express. Revisit only if a column-projection or scan operation is added.

Follow-up noted, not done: arena payloads are not cache-line aligned, so about half of the
32-byte rows straddle two lines. Aligning chunk payloads to 64 bytes is a candidate
optimization for Part B, to be measured before it is kept.

## D-029 — First A2 campaign discarded (ACCEPTED, A2)

The first full measurement campaign started on battery (`powersave` governor) and the power
source changed to AC at an unrecorded moment during or after it. Its data was discarded
unpublished and the whole campaign was rerun under one condition (AC, `performance` governor
at every stage boundary, recorded in `results/part_a/conditions.log`). The governor and power
source were changed by the user, not by the agent. Only the rerun is in the repository.

## D-030 — Part A preserved; Part B on its own branch (ACCEPTED, 2026-10-12, user decision)

Part A is accepted as the baseline. Tags `a0-bootstrap`, `part-a-core`, `part-a-final` are
preserved (local and on GitHub); no history rewrite, no force-push, no tag deletion. Part B
development is on branch `part-b/rcu`, created from `part-a-final`. The Part A benchmarks,
results and report are not modified. No submission ZIP is built yet. Nothing is merged into
`main`.

## D-031 — RCU is a fourth build of the same `KVStore`; the Part A variants are unchanged (ACCEPTED, B1 — please confirm)

The assignment says the Part B change "must live inside the KVStore class itself: don't fork
the store into a second implementation", and Part D compares `single_thread`, `global_lock`,
`rwlock` and the RCU store.

Chosen: `KVStore` stays one class in one `kvstore.cpp`/`kvstore.h`; a fourth synchronization
policy `rcu` (`NOVA_KV_SYNC_RCU`) selects the Part B engine (`src/versioned_store.*`). The
three lock-based policies keep the Part A in-place engine byte-for-byte in behaviour, so the
Part A baselines and the future Part D comparison are built from one tree. The RCU engine
reuses the hash, slot format, probing and load rules, the allocation layer and, as its private
builder, the Part A `HashSegment` class; it differs in that published state is immutable and
ownership is structural.

- Alternative A (rejected for now): make all four policies run on the copy-on-write engine,
  with the lock policies merely adding a lock. That would be "one data structure" in the
  strictest sense but would change the Part A baselines (every locked write would also pay the
  copy), making them no longer the Part A code that was measured.
- Alternative B (rejected): replace the lock variants entirely. Part D could then only be run
  by checking out `part-a-final`.

This is an interpretation of the assignment's wording, so it is flagged for the user rather
than assumed settled.

## D-032 — B1 lifetime strategy: retain every replaced version (ACCEPTED, B1)

Per blueprint §8.5 and the user's instruction ("if B1 intentionally defers reclamation, retain
the old versions safely rather than implementing an incomplete reclamation protocol"): no EBR
and no cleaner in B1. Each replaced version is pushed on a writer-owned retained list (node
pre-allocated before publication) and released only when the store is destroyed. Memory grows
with every write.

Two internal hatches release retained versions under the **caller's promise that no other
thread is reading**: `release_retained_quiescent()` and `set_quiescent_reclaim(true)`. They
are reachable only through `NovaKvInspector` / `VersionedStore`, never through the public API,
are used by single-threaded tests (to exercise the full ownership graph on every write) and by
the benchmark harness after its single-threaded load, and must be removed or strictly gated in
B2. They are not a reclamation protocol and are not claimed as one.

## D-033 — Ownership model for Part B (ACCEPTED, B1; replaces D-017 for the RCU store)

Implements D-023. Versions own segment references (`Segment::owners`); each segment holds one
arena pin per distinct chunk it references (`Segment::pins`, sorted, with per-chunk entry
counts); the writer owns the writable chunk. A chunk is freed and its ID recycled only with no
pin, not writable, and not referenced by the commit in progress. The Part A behaviours of
freeing by entry count and rewinding the writable chunk are not used. All ownership is
acquired after the last fallible step of a commit, so a failed commit has no ownership to
undo. Details and invariants O1–O6: `docs/versioning.md` §5.

## D-034 — Representation choices in B1 (ACCEPTED, B1)

- A `Version` embeds its 256-entry directory instead of pointing at a separate `Directory`
  object (one allocation and one indirection fewer; still one directory per version).
- **COW segments start at 16 slots**, not the 1024 the blueprint lists as the initial default.
  Every write copies the whole segment it touches, so a segment should be as small as its
  contents allow; a mostly empty 1024-slot segment would cost 24 KiB per write. Segments still
  double under the same 0.70 load rule, so at the workload's ~391 keys per partition they are
  1024 slots anyway. The Part A variants keep 1024. This is a deviation from a default the
  blueprint marks as provisional and tunable.
- A partition whose last key is erased becomes a null directory entry (no segment, no
  tombstones to keep).
- All atomics use `memory_order_seq_cst` (blueprint §0; nothing is relaxed in B1).

## D-035 — `erase` and allocation failure in the RCU store (ACCEPTED, B1)

An erase publishes a new version and therefore allocates. If that allocation fails, `erase`
returns `false` and the key is still present; the API cannot distinguish this from "key was
absent". An absent key is detected first and costs no allocation or publication. Inside a
replacement segment, the tombstone-removal rebuild remains best-effort as in Part A.

## D-036 — Internal multi-key commit; `Transaction` stays Part C (ACCEPTED, B1)

Blueprint §8.5 asks B1 to test "transaction-like multi-key update through internal staging
(public Transaction added later)". `VersionedStore::commit(ops, n)` is that staging: grouped by
partition, one clone per touched segment, one publication. It is reachable only internally
(`NovaKvInspector::commit_batch`). `Transaction::put/commit` remain inert until C1.

## D-037 — B1 observation, not a benchmark campaign (ACCEPTED, B1)

`scripts/run_b1_observation.sh` records read throughput/latency, writer throughput and the
copy cost per publication for the RCU store with the Part A harness, dataset, read mix, pinning
and flags: 3 repetitions, 1/2/4/8 readers, paced writer (10,000/s, 3 s window) and unthrottled
writer (1 s window, because B1 retains about 2–3 GiB per second of unthrottled writing), plus
one-thread runs. The harness stops the writer and invalidates the run if retained memory
exceeds a limit. No perf profiling, no reclamation-batch study: those are B2 and Part D.

## D-038 — The fourth (`rcu`) build is accepted; what Part A vs RCU comparisons mean (ACCEPTED, 2026-10-12, user decision)

Confirms D-031. The original Part A engine and its three synchronization variants stay
unchanged; the RCU variant uses immutable version roots and copy-on-write segments. The
Part A baseline is not redesigned.

**Consequence for every comparison between a Part A variant and the RCU store:** the two
differ in storage and update strategy, not only in synchronization. A Part A write modifies
one slot in place and appends the value; an RCU write copies the whole segment it touches
(about 24 KiB at the workload's size) and the 2 KiB directory, updates a few hundred ownership
counters, and (until B2) keeps the replaced version. Read paths differ too (one extra
indirection through the version; no lock). So a throughput or latency difference between,
say, `rwlock` and `rcu` measures "lock-based in-place store" against "lock-free copy-on-write
store" as whole designs; it must not be presented as the cost or benefit of the lock alone.
This is stated wherever such numbers are reported (B1 report, and later the Part D report).

## D-039 — Pre-B2 audit findings and required changes (ACCEPTED, 2026-10-12)

Full text: `docs/b1_audit_and_b2_readiness.md`. No correctness defect in B1 as specified. One
ownership path is safe only because no cleaner exists and **must be changed before B2 enables
one** (reproduced in a scratch experiment, `results/B1/audit/`): the writer's ownership of the
writable chunk and of chunks an open commit appended to is a pointer comparison / private
list, so a concurrent release of old versions could free a chunk the commit still references.

Required before a cleaner thread runs (R1–R7), performance item to measure (R8):

- R1 writer and open-commit ownership of chunks become real pins; one freeing rule
  (the decrement to zero frees).
- R2 freed chunk IDs go from cleaner to writer through a single-producer/single-consumer
  channel; the cleaner never touches the writer's free stack.
- R3 the retained list becomes a FIFO retirement queue carrying retirement epochs.
- R4 statistics split per thread or atomic.
- R5 `release_version` resumable under work budgets.
- R6 quiescent hatches and version-pointer test interfaces removed or confined and checked.
- R7 shutdown: stop and join the cleaner before releasing.
- R8 keep `owners` / `pins` off cache lines readers load (measure first).

The B1 implementation was not modified by the audit. One test schedule was added
(`versioned_interleaving_test` S6: all older versions released while the writer holds a
prepared version).

## D-040 — B2 authorized; user decisions for reclamation (ACCEPTED, 2026-10-12, user decision)

- Fix the audited chunk-lifetime bug before any cleaner exists; deterministic tests for both
  audited schedules, passing under ASan and TSan.
- Classical gated three-epoch reclamation, per-reader announcements, `seq_cst` atomics. Age
  never establishes eligibility. Readers never wait for cleaner or writer; the cleaner never
  waits for a reader; the writer never waits for a grace period.
- Reader slots: **no fixed limit** (dynamically expandable, stable); `get()` must **not**
  return `false` because registration capacity is exhausted; registration may lock on its slow
  path, ordinary reads may not; thread-local registration must not touch a destroyed store.
- Internal foundation for explicit Snapshots now; the public Snapshot stays Part C.
- A reclamation pass = one epoch-advance attempt followed by eligible cleanup; at most `n`
  versions completely reclaimed per pass; version destruction resumable; dequeuing does not
  count as freeing.
- **No fixed retirement-backlog limit.** Allocation failure returns `false` with the published
  state unchanged.
- Dedicated background cleaner thread; polling when idle is acceptable.
- No `sudo`, no kernel-setting change; profile only if permissions allow.
- No Part C/D/E work, no submission ZIP.

## D-041 — Chunk ownership is one reference count with one freeing rule (ACCEPTED, B2; closes audit F2/F3, R1/R2)

`PinnedChunk::refs` counts segment references, the writer's reference on the writable chunk,
and an open commit's references on chunks it created or moved on from. The only way a chunk is
destroyed is the `fetch_sub` that takes `refs` from 1 to 0, on whichever thread performs it.
Nothing reads the count and then decides. Destruction order: clear the directory slot, free
the memory, then return the ID. The chunk-ID pool is a lock-free stack threaded through the
directory blocks (pushers: writer and cleaner; single popper: the writer, which makes it
ABA-free). Arena statistics shared by both threads are atomic.

## D-042 — Epoch protocol and reader slots (ACCEPTED, B2)

Protocol exactly as in the formal-safety supplement §3, all `seq_cst`; labelled operations and
proof in `docs/reclamation.md`. Slots: blocks of 32 cache-line-aligned slots in an append-only
list, never moved or freed while the store lives; one slot per (thread, store), found by a
thread-local comparison after the first read; dedicated slots for long-lived pins.

- **Exhaustion:** the only resource a read can lack is a new slot block on a thread's first
  read; then `get` throws `std::bad_alloc`. It never returns `false` for that reason and never
  proceeds unprotected. (Options considered: abort; mutex-guarded shared slot; return false.)
- **Thread exit / store destruction:** stores have never-reused IDs; a process-wide table of
  live IDs, taken under a mutex only at store construction/destruction and thread exit,
  decides whether an exiting thread may return a slot.
- Registration scans the slot list for a free slot (linear in the number of slots).
- No per-read instrumentation; only validation retries are counted, in the reader's own slot.

## D-043 — Retirement queue, passes and budgets (ACCEPTED, B2)

FIFO linked queue with a dummy node; node allocated before publication; one store hands the
record over. A pass = one gated advance attempt (only if something is retired) + destruction
of eligible versions, oldest first, until `batch` versions are completely destroyed or
`batch × work_per_version` work units are spent (unit = one directory entry visited or one
chunk reference dropped; `work_per_version` = 1024, internal). A version that does not fit is
left half-released and resumed by the next pass; it counts as reclaimed only when finished.
`set_reclaim_batch(n ≤ 0)` means 1; default 64. The background cleaner runs passes back to
back while they make progress and sleeps 200 µs otherwise; the writer never signals it.

## D-044 — Cleaner and comparator modes; quiescent hatches removed (ACCEPTED, B2; closes R6)

`CleanerMode`: Background (default), Manual (tests run passes), Inline (the writer runs passes
after each commit). All three run the same epoch protocol and are safe with concurrent
readers; Manual and Inline exist to make tests deterministic. The B1 functions
`set_quiescent_reclaim` and `release_retained_quiescent`, which freed versions on the caller's
promise, are **deleted**. `ReclaimMode`: Epoch (default), Disabled and StopTheWorld (Part D.2
comparators; StopTheWorld makes readers take a shared lock and is selected only explicitly).
Test-only accessors that return an unprotected version pointer are renamed `*_unprotected`
and documented as requiring a store in which nothing can be reclaimed. Modes are chosen through
internal options (`NovaKvInspector::set_default_rcu_options`), not the public API.

## D-045 — One sentence of the formal-safety supplement is too strong (RECORDED, B2)

Supplement §4, second bullet, argues that if the collector's scan misses a reader, its epoch
advance cannot succeed after the reader validated. It can: the scan may read the slot before
the reader announces and the advance may follow the reader's validation. The property that
actually holds, and suffices, is *bounded overtaking* (`docs/reclamation.md` Lemma B): a
participant that validated epoch *e* and is still pinned is never overtaken by a second
advance. The protocol is implemented exactly as specified; only the proof text differs.

## D-046 — B2 measurements are compared within one session only (ACCEPTED, B2)

Governor and power source have differed between milestones (`performance`/AC for Part A,
`performance`/battery for B1, `powersave`/battery at B2). `scripts/run_b2_observation.sh`
therefore re-measures, in the same session, the B1 checkpoint (built from tag
`part-b-publication`) and the Part A lock variants, and only same-session numbers are compared.
Earlier results are not altered and not used for ratios. perf was unavailable
(`perf_event_paranoid = 4`), so no profile was taken.

## D-047 — The reclaim-batch setting is a literal private member in `kvstore.h` (ACCEPTED, post-B2 audit, 2026-10-12)

PDF p. 4: "Add a private member controlling how many retired versions a reclamation pass frees
at once, plus a public setter, void set_reclaim_batch(int n), declared alongside put/get/erase
in kvstore.h and defined in kvstore.cpp." The location clause grammatically belongs to the
setter, but "a private member" next to a public setter of `KVStore` can only mean a member of
`KVStore`, whose definition is `kvstore.h`. A field of the private implementation object is
not a private member of that class. The user asked for the literal form if the PDF requires
it. Done: `std::atomic<int> KVStore::reclaim_batch_` (default 64, `nova::kDefaultReclaimBatch`),
declared before `impl_`. `VersionedStore` takes an optional pointer to the setting and loads
it in every pass, so there is one copy, not a mirror; a `VersionedStore` built on its own
(engine tests) uses its own atomic. The lock variants record the value and ignore it.
`<atomic>` is now included by the public header. No public signature changed
(`api_conformance_test`). Closes O-15 and supersedes the open question in D-006.

## D-048 — Reads during thread teardown get a fresh slot (ACCEPTED, post-B2 audit; defect found and fixed)

Found by reading `src/epoch.cpp` against assumption A3 of `docs/reclamation.md`, not by a
test: `~EpochThreadCache` returned the thread's slots but left the fast-path entry
(`tl_epoch_last`) pointing at one, and `thread_slot_slow` used the cache's vector after its
destruction. A `get` from a destructor that runs later on the same thread (another
`thread_local`, or a static on the main thread) would announce in a slot that may already
belong to another participant, or corrupt memory. Reproduced deterministically
(`results/B2_audit/late_read_reproducer_before_fix.txt`: abort, double free). Fix: the
destructor clears the entry and sets a trivially destructible flag; later lookups go to
`thread_slot_late` (fresh owned slot, previous late slot returned). Cost: at most one slot per
such thread stays owned until its store is destroyed. Not reachable from the tests, drivers or
benchmarks that existed; `part-b-final` (304a8ac) contains the defect.

Same audit: slot blocks are allocated on reader threads and therefore not through
`alloc_bytes`' single-threaded failure counter, so the `std::bad_alloc` path of `get` had no
test. Added a thread-safe hook (`test_hooks::fail_slot_block_allocations`) and two tests; the
path behaved as documented. A block leaked on that path when the domain had no first block;
fixed.

## D-049 — The stop-the-world comparator is not pin-aware (RECORDED, post-B2 audit; C1 prerequisite)

`stw_pass` frees the whole retire list once it holds `stw_lock_` exclusively. Readers hold it
shared for a whole `get`; a `VersionedStore::Pin` holds nothing. A pin in a `StopTheWorld`
store could therefore outlive its version. No test, driver or benchmark combines them and the
public API cannot create a pin yet, so nothing is unsafe today. Before `Snapshot` becomes
public (C1), `stw_pass` must respect pins (for example: under the exclusive lock, free only
entries the epoch rule allows, which is still an unbounded whole-list sweep that stops all
`get` calls) or `acquire_snapshot` must be defined for that mode.

## D-050 — `part-b-audited` is the Part B baseline (ACCEPTED, 2026-10-12, user decision)

Annotated tag `part-b-audited` on `87d38aa` (verified source `81917be`). `part-b-final`
(`304a8ac`) is kept, unmoved. Part C branches from `part-b-audited`
(`part-c/transactions`), and the Part A → Part B and Part B → Part C diffs are taken against
it. Closes O-17.

## D-051 — Stop-the-world comparator frees by the epoch rule (ACCEPTED, C1; closes D-049 / O-16)

Fixed before any public Snapshot code was written. `stw_pass` still freezes every reader with
the exclusive lock and still sweeps the whole list without batch or budget, but it frees an
entry only if `[C6]` holds, after two gated advances made under the freeze. Short readers are
frozen out, so only long-lived pins can hold an entry back. Snapshot-scoped `get`/`keys` take
the shared lock per call in that mode, so the comparator freezes them too. Regression:
`reclamation_test::test_pin_protected_in_mode` (epoch, stop-the-world, disabled); its output
against the old code is `results/C1/stw_pin_reproducer_before_fix.txt` (pinned values
overwritten, 3000 versions and their chunks freed under the pin).

## D-052 — Transaction semantics (ACCEPTED, C1)

Appendix A fixes the signatures; these points it leaves open:

- Staging copies at `put()` (blueprint §4.4). Allocation through `alloc_bytes`
  (`src/staged_writes.*`), so staging failures are injectable.
- `put()` returns `void`: a put that cannot be staged (no memory, null value with a length,
  length above `kMaxValueBytes`) marks the transaction failed and `commit()` returns `false`
  with nothing published, as the user specified.
- Last write to a key wins.
- First `commit()` of an empty transaction: `true`, no publication (the supplied generator
  does this). Any later `commit()`: `false`, no publication. `put()` after `commit()`: ignored.
  The supplied generator constructs a new `Transaction` per batch, so one commit per object is
  compatible with it.
- No `erase` in a transaction (not in Appendix A).

## D-053 — Writes are serialized by a writer mutex (ACCEPTED, C1; user direction "prefer a writer-only serialization mechanism")

Audit of `put` / `erase` / `Transaction::commit` running simultaneously: all three end in
`VersionedStore::commit`, whose writer state (`latest_`, `queue_tail_`, the arena's writable
chunk and commit marks, the popping side of the chunk-ID pool, the writer counters) is
unsynchronized by design. Two at once would corrupt it (demonstrated by mutation C19: crash).
`writer_mutex_` is now held for the whole of `put`, `erase` (including its presence check)
and `commit`. Claim made: concurrent calls are safe and are executed one at a time. Claim
**not** made: concurrent writing, or isolation between clients that read and then write
(`docs/transactions.md` §5). Readers never take the mutex. Assumption A1 of the reclamation
argument is thereby enforced rather than assumed. With `CleanerMode::Inline` the passes run
inside the mutex; that mode is for single-threaded tests.

## D-054 — Snapshot representation and failure policy (ACCEPTED, C1)

- One pin per snapshot. The handle's `void* pin_` is the `ReaderSlot*`; the version lives in
  the slot (`ReaderSlot::token`, owner-private, 8 bytes of the slot's existing cache line).
  No allocation per snapshot beyond slot blocks; the public header is unchanged.
- `acquire_snapshot()` throws `std::bad_alloc` if no slot can be had (never an unprotected
  handle; blueprint §15 asks for a tested policy).
- Wrong store or moved-from handle: `get` → `false`, `keys` → `0`, nothing dereferenced.
- A snapshot outliving its store is the caller's error (blueprint §2.2), not detected.
- `keys()` order is unspecified but repeatable; no total, no continuation (not in the API).

## D-055 — C1 on the lock-based baselines (ACCEPTED, C1)

`Transaction` **is** implemented for `single` / `global_lock` / `rwlock`: one exclusive lock
hold, undo on failure, because the supplied workload uses `Transaction` with whichever store
it is linked against and ignores `commit()`'s result. This adds a function to the baseline
library and changes no existing one; `put`/`get`/`erase` are byte-identical to
`part-a-final`, and Part A's measurements used no transactions.
`Snapshot` is **not** implemented there: `acquire_snapshot()` throws `std::logic_error`
(reason in `docs/transactions.md` §3.5). Previously it returned an inert handle. The intended
build for snapshots, `keys()` and the Part C.2 join is `nova_kv_rcu`.

## D-056 — Scheduling-dependent coverage assertions in `reclamation_stress_test` (C1; one threshold changed, disclosed)

`reclamation_stress_test` (a Part B test) asserted, besides its correctness checks, that
during the run the epoch advanced more than 100 times and more than 50 chunk IDs were reused.
Both are coverage statements whose values depend on scheduling: while the test's pinning
thread holds a pin the epoch can advance at most once more (Lemma B), so the number of
advances is about two per pin cycle.

- The first C1 verification run failed in the release stage on `advance_successes > 100`, and
  so did the clean-clone run. Measured afterwards, 100 sub-runs each, release build, same
  machine state: **`part-b-audited` (unmodified Part B): min 58, 10th percentile 141, median
  233; C1: min 113, 10th percentile 160, median 223.** The distribution is the same, and the
  Part B code itself goes below 100. It is a marginal threshold in the test, not a change in
  the store; the earlier verification runs passed by chance on that line.
- Changed: the floor is now 10 (still asserts that grace periods completed while readers,
  pins and the writer were running; the actual count is printed). **This is a change to an
  existing test's expectation and is recorded here for that reason.** No correctness
  assertion of the test was touched: no torn read, no stale read, pins consistent, backlog
  drains to zero, reclaimed == published, per-pass maximum ≤ batch, counts validate.
- Not changed: `ids_reused > 50` (serial runs observe 580–740). It does fail when the
  machine is heavily oversubscribed (reproduced on unmodified Part B code with 12 busy
  processes plus four parallel copies), because the cleaner then gets too little CPU before
  the writer has finished. Run the suite serially, as `scripts/verify_all.sh` does.

## D-057 — Isolation wording corrected (ACCEPTED, C1 audit, 2026-10-12, user direction)

`docs/transactions.md` §5 (and the C1 report, compliance note 1) said that under the
assignment's single-writer assumption every execution is serializable. Removed. Serializing
commits orders the writes; it says nothing about the reads a client based them on, and a
single thread working from an earlier snapshot can make a stale decision by itself. The
documents now state only what is implemented: serialized, atomically published commits;
consistent reads of one store version through a `Snapshot`; independent gets may see
different versions; a client-side read followed by a commit is not a transaction; no read
set, no validation; lost updates and write skew possible; no durability.

## D-058 — Reclamation progress is tested from accounting, not from an advance count (ACCEPTED, C1 audit)

Audit of the threshold change of D-056. Evidence
(`results/C1_audit/stress_epoch_advance_distribution.txt`, 100 sub-runs each, release, serial,
one session): unmodified `part-b-audited` min 99, p10 150, median 228, **one sub-run at or
below 100 and one failed check**; C1 min 86, p10 155, median 232, one sub-run at or below 100.
Same distribution: the old floor was a scheduling-sensitive coverage heuristic and its
lowering concealed no regression. (Why it is scheduling-bound: while the test's pinning
thread holds a pin the epoch can advance once more at most, so advances ≈ two per pin cycle.)

Added, so that no advance count is the progress assertion:
`reclamation_test::test_reclamation_progress` (manual passes, batches 1/8/64/1024, three
stall/release cycles): with a pin, backlog stays exactly at the number retired, nothing is
reclaimed over 3000 passes, memory held grows by at least the retained values; after release
the backlog falls by exactly the batch on every pass, reaches 0 within `retired/batch + 3`
passes, reclaimed == retired, chunks and bytes return to the idle level.
`test_background_cleaner_resumes_after_release`: the same end state with the store's own
cleaner thread.

## D-059 — Cross-store snapshot: analysis and position for C2 (RECORDED, C1 audit; O-1 stays OPEN / INSTRUCTOR)

The texts. PDF C.2: "Build a secondary index … on one non-primary-key column of a second
table you add to the store (see Appendix A for the two-table shape used by the benchmark)";
"Acquire exactly one Snapshot handle (Appendix A.1) at the very start of the join, and pass
that same handle to every keys(), index_get(), and get() call the join makes." Appendix A.2:
`workload_run(KVStore& trades, KVStore* instruments, …)`, and the supplied generator calls
`instruments.create_index(…)` on the second object. Appendix A.1: `acquire_snapshot()`,
`get(const Snapshot&, …)`, `index_get(const Snapshot&, …)`, `keys(const Snapshot&, …)` are
all member functions of one `KVStore`.

The conflict. Two `KVStore` objects are constructed, owned and destroyed independently and
each has its own "current version" pointer (the PDF's own description of what a `KVStore`
owns). One handle obtained from one of them pins one of those pointers. Nothing in the API
relates the two objects.

Can one handle cover both without changing the API? Only by coupling the objects behind the
interface: every `KVStore` in the process would publish through one hidden shared root (and
share one epoch domain and one writer order), so that a handle acquired from any store pins
all of them and any store accepts it. No signature changes, but it changes what the API
visibly promises: stores stop being independent (a write to one republishes the shared root;
writers of different stores serialize; a snapshot of one store holds back reclamation in all
of them; destruction order matters), and it is a redesign of root publication, i.e. of the
part of the system whose safety argument has just been audited. The assignment does not
describe any such "database" object. Implementing it would be inventing cross-store
atomicity that the stated contract does not ask for in so many words.

Alternative reading: "a second table you add to **the** store" — both tables in one
`KVStore`, one snapshot, literally one handle. But `create_index(field_offset, field_len)`
indexes "a byte range within each stored value" of that store, the two row types have the
field at different offsets, and the supplied benchmark passes two objects. Not compatible
with Appendix A.2 as given.

Position for C2 unless the instructor says otherwise (the minimal interpretation that
violates nothing in the API): the join acquires **one Snapshot per store, both at the very
start**, and passes each store's handle to every call on that store. Guarantee: each table is
read at one unchanging version for the whole join; never rows from two versions of the same
table; the writer is never blocked. **Not** guaranteed, and to be stated wherever the join is
described: that the two versions belong to one instant. If both tables are written
concurrently, the pair is not an atomic cross-store snapshot, and the PDF's literal "exactly
one Snapshot handle" is not met. (With the benchmark's shape, where instruments are loaded
once and not modified during the join, the two instants are indistinguishable from one.)

Question for the instructor (verbatim proposal):

> Part C.2 asks the join to "acquire exactly one Snapshot handle … and pass that same handle
> to every keys(), index_get(), and get() call", while Appendix A.2 gives the benchmark two
> separate KVStore objects (trades, instruments), and Appendix A.1 makes acquire_snapshot()
> and the snapshot-scoped calls members of a single KVStore. A Snapshot taken from one
> KVStore pins that store's version only. Which do you intend?
> (a) one Snapshot per store, both acquired at the start of the join (each table consistent
>     on its own; the Instrument table is static during the benchmark);
> (b) both tables inside one KVStore, so that one handle covers them — and if so, how should
>     create_index's single byte range apply to two row types; or
> (c) the two KVStore objects must share one global version, so that a handle from one is
>     valid for the other — and if so, may stores be coupled internally (shared writer order
>     and reclamation) to achieve it?

## D-060 — Cross-store joins use ONE SNAPSHOT PER KVSTORE ("Option A") (ACCEPTED, 2026-10-13, user decision; final)

The user chose option (a) of D-059 and declared it final for this implementation: no further
clarification is to be requested. A join over two `KVStore` objects acquires exactly one
`Snapshot` from each store at its start (`benchmarks/join.h::acquire_join_snapshots`), uses
each handle for every snapshot-aware call on its own store, acquires no other snapshot and
makes no current-version read during the join, and keeps both handles until the join ends.

Guaranteed: the Trades table is read from one immutable version; the Instruments table (rows
and index) is read from one immutable version; each table is internally consistent for the
whole join; writers keep publishing.

Not guaranteed, and to be said wherever the join is described: the two snapshots are not one
globally atomic point in time; and the implementation uses two handles, **not** the PDF's
literal "exactly one Snapshot handle". Recorded as a **specification deviation**
(`docs/part_c_compliance.md` J2), never as a satisfied requirement.

Rationale: the PDF's benchmark shape (Appendix A.2) is two independently owned stores and the
snapshot API (Appendix A.1) is per store. A single handle would need either a hidden global
version coupling all stores (changes what independent stores mean, redesigns the audited
publication path) or both tables in one store (incompatible with `create_index`'s single byte
range and with Appendix A.2). Neither is done: no global cross-store versioning, no merged
table. O-1 is closed by this decision as "deviation, documented".

## D-061 — Secondary index structure (ACCEPTED, C2)

- The index is part of the `Version`: definition (`index_offset`, `index_len`) and 64
  pointers to immutable `IndexSegment`s. One root swap publishes data and index (INV-12).
- An `IndexSegment` is one allocation: (field hash, key) pairs sorted by (hash, key), i.e.
  posting lists in ascending key order, plus a linear-probing table over the distinct hashes.
  It stores no field bytes and no pointers, so it owns nothing: ownership is one `owners`
  count, as for primary segments, and no new kind of reference was needed.
- Lookups verify each candidate against the primary data of the same version, byte for byte.
  Hash collisions are therefore harmless, and tested by cutting the hash to a few bits.
- Updates: a commit records index deltas from the private copies (so repeated writes to a key
  inside one transaction are followed), then rebuilds each touched index partition by a
  linear merge. An overwrite that does not change the field produces no delta.
- 64 index partitions (not 256): the directory grows by 512 bytes and a commit takes at most
  64 more owner references.
- Cost accepted for now: a field change rebuilds whole index partitions (measured in
  `results/c2/`); a finer-grained posting-list structure is a possible later optimization and
  was not attempted ("profile before optimizing"; no profiler is available).

## D-062 — `create_index` and `index_get` semantics the assignment leaves open (ACCEPTED, C2)

- One index per store. Same range again: `true`, nothing published. Different range: `false`.
- `field_len == 0`, or a range beyond the 1 GiB value limit (including overflow): `false`.
- Backfill is built privately and published, with the unchanged primary data, in one swap.
- **A value shorter than `offset + len` is not indexed.** (The blueprint §2.2 suggested
  requiring values to cover the range; refusing such writes would make `put` fail for a
  reason the Part A/B contract does not have, so they are stored and simply not indexed.)
- `index_get`: `field_len` must equal the index's; null pointers or `max_keys <= 0` give 0;
  ascending key order; on truncation the smallest `max_keys` keys; returns the number written.
- Snapshot overload: the pinned version's index; moved-from or wrong-store handle gives 0.

## D-063 — No secondary index on the lock-based baselines (ACCEPTED, C2)

`create_index` returns `false` and `index_get` returns 0 on `single` / `global_lock` /
`rwlock`. The Part C index is defined as part of a published version; those stores have none,
and a join needs `Snapshot`, which they do not have either (D-055). The supplied generator's
`use_join` path consequently finds nothing on them. The intended build for Part C.2 is
`nova_kv_rcu`.

## D-064 — The join lives outside the store, over the public API (ACCEPTED, C2)

`benchmarks/join.h` (header, used by `tests/join_test.cpp` and `benchmarks/join_bench.cpp`).
Appendix A has no join entry point, and writing the join against `kvstore.h` only shows that
the public API suffices. `keys()` and `index_get()` report how many they wrote, not how many
exist: the join asks again with a larger buffer when one comes back full (both results are in
a repeatable order), which resolves O-18 without an API extension. Records of the wrong size
are skipped and counted.

## D-065 — The supplied generator's index probe and its indeterminate eighth byte (RECORDED, C2; measured and corrected in the C2 audit)

Source, unmodified (`supplied/workload_gen.cpp`): `make_trade` declares `Trade t;` (no
initializer) and fills `symbol` with `std::snprintf(t.symbol, sizeof(t.symbol), "SYM%03d",
(i + j) % 50)`, which writes 6 characters and a terminator: bytes 0–6. **Byte 7 of
`Trade::symbol` is never written**; its value is indeterminate by the language. The whole
struct is stored (`trades.put(t.trade_id, &t, sizeof(t))` or the `Transaction` equivalent),
so that byte becomes part of the stored row. `run_reader`, when `cfg.use_join` is set, reads a
trade and calls `instruments->index_get(t.symbol, sizeof(t.symbol), match_ids, 8)`: an 8-byte
probe. `load_instruments` creates the index over `Instrument::symbol` (offset 0, 8 bytes) and
writes instruments from `Instrument ins{}`, so their byte 7 is 0.

Effect on a byte-exact index (which the requirement demands): the generator's probe matches a
trade exactly when that trade's stored byte 7 is 0.
`tests/reference_workload_link_test.cpp` asserts that equivalence and prints the counts.

**Measured** (`results/C2_audit/generator_probe_hit_rate.txt`): in every build of this
repository on this machine (debug, release, ASan+UBSan, TSan; 200 trades, and the default
100,000 trades with puts and with transactions) the byte was 0 in every stored trade and the
probe matched 100 %. **So the generator's probe does hit here.** The C2 report said it "may
miss": that is what the source permits, not what happens with these binaries, and nothing was
measured wrongly because of it. It remains a property of the supplied program that another
compiler, flag or call sequence could change.

What it affects: only what work the generator's probe does (a miss is a hash and one bucket
probe; a hit adds one primary lookup and a compare), i.e. the latency of `use_join` runs. It
affects no correctness check: the generator discards the probe's result. For Part D: every
generator run with `use_join` records the measured hit rate of that binary next to its
numbers; index and join behaviour is measured with the repository's own harness
(`benchmarks/join_bench.cpp`), whose data is fully initialized. The supplied file is not
modified and the comparison is not loosened.

## D-066 — Leak in a no-change commit on an indexed store (C2; found by source review, fixed)

`commit_locked` allocates the index-delta array before applying the operations. Its early
return for a commit that changes nothing (a batch consisting only of erases of absent keys)
did not free it. Not reachable through the public API (`KVStore::erase` checks for the key
first and `Transaction` has no erase), but reachable through the engine's batch commit. Found
while reviewing the function before declaring C2 complete; the first C2 verification run was
stopped for it. Reproduced under ASan before the fix
(`results/C2/noop_commit_leak_before_fix.txt`: 9600 bytes in 50 allocations), fixed by
freeing the array on that path, regression test
`index_test::test_commit_without_change_on_indexed_store` (detected by the leak checker of the
ASan build). The observation set in `results/c2/` was measured at `92e11ec`, before this
one-line fix; the fixed path is not exercised by any benchmark.

## D-067 — Part C API coverage on the lock-based variants: assessment (RECORDED, C2 audit; no gap found)

Question: must `create_index` / `index_get` (and `Snapshot`) work on the three Part A
variants, or only on the RCU store? From the PDF:

| PDF text | Bearing |
|---|---|
| Overview: Part C "extend[s] the store with atomic, isolated multi-key writes and a secondary index used for an index-nested-loop join, **both built on the RCU mechanism from Part B**" | Part C is defined on the RCU store |
| Part C intro: "without adding a second locking scheme on top of RCU" | same |
| C.1: commit publishes "with a single pointer-swap, built the same way as a single-key write in Part B" | a lock-based store has no pointer swap |
| Appendix A.1, Snapshot: the version "must stay pinned by your Part B reclamation scheme" | meaningless without versions |
| Part A: "Implement a KVStore class with the interface from Appendix A (put, get, erase)" | the baseline's required interface is three functions |
| Part B: "The change must live inside the KVStore class itself: don't fork the store" | one class, one header: which is why the declarations are visible to every build |
| D.1: point gets "for all four configurations", generator "with its default parameters" (`use_txn = false`, `use_join = false`) | the only requirement that names the lock variants uses neither Part C API |
| D.3: join and transaction benchmarks | no list of configurations; they measure the Part C store |
| Appendix A.2: the Part A driver example passes `nullptr` for `instruments` | the baselines are not expected to see an index call |

Conclusion: nothing in the assignment requires the Part C APIs to function on the lock-based
variants. They must *compile and link* there (one header, one generator source), which they
do. `Transaction` happens to work on them (D-055); `acquire_snapshot` throws and
`create_index` / `index_get` report "no index" (D-055, D-063), both documented and tested.
**Not a Part C requirement gap.**

Residual risk, stated rather than hidden: someone running the generator with `use_join` (or
`use_txn` plus a snapshot-based tool) against a lock driver gets "no index". If that were ever
wanted, the smallest correct remedy would be a mutable hash index inside the baseline stores,
updated under the existing lock in `put` / `erase` / `commit_staged` (with undo), about the
size of the existing baseline transaction code; it would touch only the lock branch of
`kvstore.cpp` and leave the `part-a-final` checkpoint and its measurements as they are. Not
done: not required, and it would add index maintenance to the Part A write path that Part D.1
compares.

## D-068 — Part D campaign: method and deviations fixed before measuring (ACCEPTED, Part D, 2026-10-13)

`scripts/run_part_d.sh` implements `docs/part_d_plan.md`; §9 of the plan lists the deviations
decided after the pilot and before the campaign (no pinned-cleaner set; 16 GB memory guard;
the parameters of the backlog-building workload; the added read-only and generator-key cells;
the perf c2c and flame-graph cases; perf stat around every run; no optimization in this
milestone). Rules the script enforces: Release `-O3` with frame pointers; mains power and the
`performance` governor at every checkpoint, else the campaign is marked INVALID; an idle
machine at the start (CPU idle time, not load average); a clean tree; an empty output
directory stamped with a campaign id that the aggregator checks on every stage; five
repetitions; every run's command and exit status kept; a run that fails its own validation
fails the campaign. Track R (supplied generator, preload-only) and track C (writer active)
are never placed in one table or one figure.

## D-069 — Part D findings that contradict an expectation, recorded as measured (Part D, 2026-10-13)

Campaign `20261013T090846-615f4357f015-all` (`results/part_d/`, report `reports/part_d_report.md`).

1. **The reclaim batch does not trade read throughput against reader stalls in this design.**
   The assignment expects "a bigger batch should mean higher throughput but a longer
   worst-case stall". Measured over batches 1–1024 in two workloads: read throughput and
   p99.9 independent of the batch; longest stall without a trend (and of the same order with
   reclamation disabled). The batch changes reclamation throughput and memory: one version per
   pass is the least efficient. Reported as measured; no trade-off is manufactured.
2. **With an unthrottled writer and 4 or 8 readers the cleaner cannot keep up** on this
   machine in `performance` mode: about 101,000 versions retired per second against about
   87,000 destroyed at 8 readers; the backlog grows by about 25,000 versions (0.65 GiB) per
   second. The audited Part B binary does the same
   (`results/part_d_diagnostics/part_b_reference_backlog.txt`). The B2 observation
   (`powersave`, slower writer) had not shown it. There is no back-pressure (D-040). The
   paced 10,000 updates/s workload is steady.
3. **Audit item R8 (owner counts on reader-visible cache lines) is only weakly supported** by
   `perf c2c`: reader loads that hit a remotely modified line are 6.7 % of such samples and
   0.07 % of all sampled loads; the dominant coherence traffic is writer versus cleaner. Reads
   do run at half speed next to an unthrottled writer; the cause is not established beyond
   "stalled on memory". **No optimization was made**: the store measured is the audited Part C
   store, and R8 stays an untested idea rather than a known fix.
4. The supplied generator's index probe matched 100 % in these binaries (D-065).

## D-070 — Two findings from the post-campaign verification run (Part D, 2026-10-13)

1. **`bench_validate_reclaim_stw` failed under TSan** (`results/verification/1006123097e1/`,
   kept as evidence; the other three modes passed). Cause: a wrong expectation in the new
   benchmark, not the store. `reclaim_bench` required the retire list to reach zero after the
   last pin is released, but the stop-the-world comparator sweeps only once the list is at
   least 1,024 long (`kStwBacklogTrigger`); under TSan the writer is slow enough that the list
   was 833 at release and stayed there. Correction: for `--reclaim stw` the run is also valid
   when the list rests below the trigger (unchanged for 50 ms); the epoch mode still must reach
   zero. The engine is unchanged. The campaign is unaffected: its five stop-the-world runs
   released 7,178–10,596 versions and drained to zero in 10.5–15.9 ms; the measurement code
   path for a list that reaches zero is the same as before. The campaign stays stamped with
   615f435, the commit that produced it.
2. **The `perf stat` wrapper around every timed run (plan deviation, D-068) is not neutral**
   for cells whose threads block often: global mutex at 8 readers about +20 %, stop-the-world
   read throughput about one half, shared mutex 0–6 % lower, RCU within 4 %; longest stalls
   unaffected (`results/part_d_diagnostics/perf_stat_wrapper_effect.txt`). Decision: the
   campaign tables stay as measured, with the bias stated in the report (§6), the checklist
   and the milestone report. No conclusion changes. A second campaign with unwrapped timing
   runs and separate counter runs would remove the bias; it is offered to the user, not done.

## D-071 — Corrected Part D campaign: unwrapped timing track, separate profiling track (user decision, 2026-10-13)

The user authorized a second campaign after D-070. Method: `docs/part_d_plan.md` §10.

- Timing runs start the executable directly; profilers run in separate sessions afterwards and
  never supply a throughput or latency figure.
- The first campaign is preserved at tag `part-d-final` (9062d83, never moved) and, in the
  tree, moved unchanged to `results/part_d/original-wrapped-20261013T090846-615f4357f015/`
  (Git tree `607e836`, identical to `part-d-final:results/part_d`; a gate checks it). It is
  diagnostic evidence, not a source of results.
- The corrected campaign lives in its own directory `results/part_d/unwrapped-<time>-<sha>/`,
  named in `results/part_d/PRIMARY`; `results/part_d/summary/summary.md` is a copy of its
  summary. No ratio mixes the two campaigns.
- The stop-the-world validation of `e09da24` stays. The comparator's behaviour below its
  trigger is specified by the engine test `reclamation_test::test_comparator_modes` since B2
  (500 retired versions: no sweep; 3,000: one sweep frees all), and pin safety in that mode by
  `test_pin_protected_in_mode`; neither depends on the benchmark. The benchmark gained one
  assertion (a list that reached the trigger must have been swept).
- The store is unchanged. No optimization.

---

## Open items carried forward

| ID | Item | Needed from | Blocks |
|---|---|---|---|
| O-1 | CLOSED by user decision D-060: one Snapshot per store; recorded as a deviation from the PDF's "exactly one Snapshot handle", not as compliance | — | — |
| O-2 | RESOLVED for user-space profiling: `perf_event_paranoid = 1` (set by the user, D-022). Remaining: `kptr_restrict = 1` hides kernel symbols (D-025) | User, if kernel symbol names are wanted (`sudo sysctl kernel.kptr_restrict=0`) | Nothing mandatory |
| O-3 | RESOLVED: FlameGraph installed project-locally at a pinned commit (D-021) | — | — |
| O-4 | Team roll numbers for the ZIP name | User | Final packaging |
| O-7 | RESOLVED in B1 for ownership (D-033). Reclamation itself (EBR, cleaner) is B2 | — | B2 |
| O-8 | RESOLVED: the fourth `rcu` build is accepted (D-038) | — | — |
| O-9 | RESOLVED (D-042) | — | — |
| O-10 | RESOLVED by user decision: no fixed limit, never return false; `std::bad_alloc` if a slot block cannot be allocated (D-040, D-042) | — | — |
| O-11 | RESOLVED: polling (D-040, D-043) | — | — |
| O-12 | RESOLVED (D-040, D-043) | — | — |
| O-13 | RESOLVED: no backlog limit; allocation failure fails the write cleanly (D-040) | — | — |
| O-15 | RESOLVED: the member is declared in `kvstore.h` (D-047) | — | — |
| O-16 | RESOLVED in C1 (D-051) | — | — |
| O-17 | RESOLVED: tag `part-b-audited` (D-050) | — | — |
| O-18 | RESOLVED: the join re-asks with a larger buffer (D-064) | — | — |
| O-14 | `kernel.perf_event_paranoid` was 1 for the Part D campaign (set by the user; not persistent: it reverts to 4 on reboot) | User, only if profiling is to be repeated | Nothing |
| O-5 | Instructor's original `workload_gen.h`, if one exists as a file | User | Nothing (transcription is in use) |
| O-6 | Blueprint §14 review checklist: error contracts confirmed (D-012). Tunables (partition count, segment capacity, chunk size, thresholds) are in use at blueprint defaults (D-018); A2 did not sweep them | User / measurements in B1 | Should be settled before or during B1 |
