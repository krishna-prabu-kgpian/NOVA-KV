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

---

## Open items carried forward

| ID | Item | Needed from | Blocks |
|---|---|---|---|
| O-1 | Exactly-one-Snapshot join across two `KVStore` instances (PDF p. 5 vs Appendix A two-table shape) | Instructor | Full PASS of C2 |
| O-2 | `perf_event_paranoid = 4`: unprivileged `perf` is unusable on this machine. Deferred to A2 by D-013; no sudo/kernel change without explicit permission | User (root: `sudo sysctl kernel.perf_event_paranoid=1`, or run captures with privileges) | A2 profiling, D flame graphs |
| O-3 | FlameGraph scripts (`stackcollapse-perf.pl`, `flamegraph.pl`) not installed. Deferred to A2 by D-013 | User approval to fetch github.com/brendangregg/FlameGraph | A2, D |
| O-4 | Team roll numbers for the ZIP name | User | Final packaging |
| O-5 | Instructor's original `workload_gen.h`, if one exists as a file | User | Nothing (transcription is in use) |
| O-6 | Blueprint §14 review checklist: error contracts confirmed (D-012). Tunables (partition count, segment capacity, chunk size, thresholds) are in use at blueprint defaults (D-018) but not yet explicitly confirmed or measured | User / A2 measurements | Should be settled before B1 |
