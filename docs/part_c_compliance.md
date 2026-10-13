# Part C compliance checklist

Requirement by requirement, from `assignment2.pdf` pp. 4–5 (Part C) and Appendix A.1.
Status values: PASS (artifact exists and its check ran and passed), PARTIAL, FAIL, NOT RUN,
DEVIATION (implemented differently from the PDF's wording, on a recorded decision). Statuses
were filled in from the verification runs recorded in `reports/milestones/C1.md` and
`reports/milestones/C2.md`.

**Summary.** Every Part C requirement is implemented and tested, with one documented
deviation: the join uses one Snapshot per store (two handles), not the single handle of the
PDF's wording (J2).

## C.1 Atomic, isolated multi-key writes (30 marks)

| # | Requirement | Artifact | Check | Status |
|---|---|---|---|---|
| T1 | "Add a Transaction class (Appendix A)" with the exact interface | `include/kvstore.h` (unchanged since A0); `src/kvstore.cpp` | `api_conformance_test` pins the signatures, non-copyable, non-movable | PASS |
| T2 | "whose put() stages writes" | `Transaction::put` → `nova::StagedWrites` (copies at once) | `transaction_test`: staging invisible, caller buffers reused and freed | PASS |
| T3 | "commit() publishes them all as one new version with a single pointer-swap, built the same way as a single-key write in Part B" | `KVStore::Impl::commit_staged` → `VersionedStore::commit` (the path `put` uses), one `current_.exchange` | `transaction_test`: 1/4/16/100 keys → publications +1, clones = partitions touched; `txn_concurrency_test::test_none_then_all` | PASS |
| T4 | "If a Transaction is destroyed without commit() being called, discard its staged writes." | `Transaction::~Transaction` | `test_abandoned_transaction`; ASan (no leak) | PASS |
| T5 | Atomicity: "a concurrent reader must observe either none or all of a committed transaction's writes, never a partial subset" | one root swap | writer parked before / after the swap, for 1/4/16/100 keys; stress: 0 partial snapshots; mutation C9 (per-key publication) detected | PASS |
| T6 | Isolation: "any single read … must see one consistent, unchanging snapshot for its whole duration, even if the writer commits several transactions while it runs" | `get`: one root load, immutable version; `Snapshot` for multi-call reads | `acquire_snapshot` parked at each pin step with 799 commits meanwhile; 3000 publications under one snapshot; 0 unstable snapshots in stress | PASS |
| T7 | "Design note: state precisely which isolation level this gives you … explain write skew … state what you give up by leaving durability out of scope" | `docs/transactions.md` §5, §6 | read by a human | PASS (see note 1) |
| T8 | `commit()` returns "true if committed; all staged writes become visible in one publish" | §1.3 of the note | `transaction_test`, `txn_failure_test` | PASS |
| T9 | Neither `KVStore` nor `Transaction` copyable or movable | header | `api_conformance_test` | PASS |

## Snapshot handle (Appendix A.1), implemented in C1

| # | Requirement | Artifact | Check | Status |
|---|---|---|---|---|
| S1 | `class Snapshot`: move constructor, move assignment, destructor; copy deleted | header (unchanged) | `api_conformance_test`; `snapshot_test::test_move_semantics` | PASS |
| S2 | "An opaque, move-only handle on one published version" | one pin; `void*` to the reader slot | `snapshot_test` | PASS |
| S3 | "For as long as a Snapshot object is alive, the version it was acquired against must stay pinned by your Part B reclamation scheme (exactly as if a reader were still inside a call against that version)" | the snapshot's slot keeps announcing its epoch; `docs/reclamation.md` Theorem 1 | 3000 publications under one snapshot: 0 versions reclaimed, 0 chunks freed, 0 IDs reused; in epoch, stop-the-world and disabled modes; mutations C1, C3, C20 detected | PASS |
| S4 | `Snapshot acquire_snapshot() const` — "pinned to the current version" | `KVStore::acquire_snapshot` | `test_snapshot_is_the_state_at_acquisition`; parked-acquisition schedules | PASS on `rcu`; throws `std::logic_error` on the lock baselines (note 2) |
| S5 | `bool get(const Snapshot&, long, void*, size_t) const` | `KVStore::get(snap, …)` | buffer contracts, later commits invisible; mutation C6 detected | PASS |
| S6 | `size_t keys(const Snapshot&, long*, size_t) const` — "enumerates up to max_keys primary keys present in the table under snap; returns the number written to keys_out" | `VersionedStore::keys_pinned` | `test_keys_basic`, `test_keys_through_structural_changes`; mutations C7, C8 detected | PASS |
| S7 | `int index_get(const Snapshot&, …) const` | `KVStore::index_get(snap, …)` → `VersionedStore::index_get_pinned`: the pinned version's own index | `index_test::test_snapshot_uses_its_own_index`; `index_concurrency_test`; mutations D5, D13 detected | PASS |
| S8 | copy-out convention ("the caller never holds a pointer that could be reclaimed") | `get` copies into the caller's buffer under the pin | code; no internal pointer is returned by any function | PASS |

## C.2 Secondary index and index-nested-loop join (30 marks)

| # | Requirement | Artifact | Check | Status |
|---|---|---|---|---|
| J1 | "Build a secondary index (a hash index, or a small B+-tree …) on one non-primary-key column of a second table" | hash index: `src/secondary_index.*`, 64 immutable segments in each `Version` (`docs/secondary_index.md`) | `index_test_rcu` against a reference model | PASS |
| J1a | `bool create_index(size_t field_offset, size_t field_len)` — "builds a secondary index over a byte range within each stored value" | `KVStore::create_index`; backfill + one publication | empty and populated stores, invalid ranges, repeated calls, under allocation failure, parked while building | PASS |
| J1b | `int index_get(const void*, size_t, long*, int) const` — "returns the number of matches (up to max_keys), primary keys in keys_out" | `KVStore::index_get`: one root load, byte-for-byte check | duplicates, zero bytes, truncation, invalid arguments; forced hash collisions | PASS |
| J1c | Index consistent with the data ("can a join … run without ever seeing a torn mid-update state") | index is part of the version; one swap per commit | `index_concurrency_test` (parked before/after the swap; 0 disagreeing snapshots in stress); `validate()` checks every retained version | PASS |
| J2 | "Acquire **exactly one** Snapshot handle (Appendix A.1) at the very start of the join, and pass that same handle to every keys(), index_get(), and get() call the join makes." | `benchmarks/join.h::acquire_join_snapshots`: **one Snapshot per store, i.e. two handles**, both at the very start; each used for every call on its own store | `join_test`: per-table consistency asserted; cross-table instant deliberately not asserted | **DEVIATION** (note 3) |
| J2a | "it must never block the writer" | snapshots are pins; no lock on any read | concurrent-writer join test; writer throughput in `results/c2/` | PASS |
| J2b | "must never return rows from two different versions" | holds **per table** (each table from one version) | `test_join_with_concurrent_writer`: 0 joins with mixed versions inside a table | PASS per table; see note 3 for across tables |
| J3 | "Enumerate the outer (primary) table's rows with keys() … under that same snapshot, rather than reusing whatever key list the workload generator happened to produce" | step 1 of the join | `join_test`; mutation D18 detected | PASS |
| J4 | "for each key keys() returned, probe the secondary index on the inner table (via the snapshot-scoped index_get()) and, for each match, fetch the full inner row with the snapshot-scoped get() to emit it" | steps 2–6 of the join | two index-free oracles; one-to-many, missing, deletes, updates; mutations D17, D19, D20 detected | PASS |
| J5 | "You don't need to support an index on more than one column" | one index per store | repeated `create_index` | PASS |

## Deliverables

| Deliverable | Artifact | Status |
|---|---|---|
| "your modified source with the diff against the Part B version" | `results/diffs/part_b_to_part_c.diff` = `git diff part-b-audited part-c-final -- src include` (checked by gate `part_c_artifacts`) | PASS |
| "a short README.md / design note covering both the isolation-level argument from C.1 and the index/join design from C.2" | `reports/part_c_readme.md`; full notes `docs/transactions.md`, `docs/secondary_index.md` | PASS |

Marks table (PDF p. 7), Part C: atomic multi-key writes + isolation note; secondary index +
index-nested-loop join; README. Whether the marks are awarded, and how J2 is judged, is the
grader's decision.

## Notes

1. The assignment says of the isolation level: "It is snapshot isolation, not
   serializability." The note states what is implemented without stretching it: atomic,
   serialized commits with snapshot-consistent reads. A read-only operation pinned to one
   `Snapshot` gets what snapshot isolation gives a reader. The `Transaction` interface has no
   reads and `commit()` validates nothing, so a read followed by a commit is not an isolated
   transaction: lost updates (which snapshot isolation would prevent) and write skew are both
   possible, and serializing the writers does not change that. Whether the grader wants the
   bare phrase "snapshot isolation" is the grader's call; the note explains both readings.
2. `Snapshot` exists on the `rcu` build only. The lock-based baselines are Part A artifacts
   with no versions to pin; `acquire_snapshot()` throws there instead of returning a handle
   whose reads would silently report "absent" (`docs/transactions.md` §3.5).
3. **One Snapshot per store (decision D-060, final for this implementation).** The PDF asks
   for exactly one handle for the whole join; the PDF's own benchmark passes two independently
   owned `KVStore` objects, and a `Snapshot` pins one version of one `KVStore`. The join
   acquires one snapshot from each store at its start. Guaranteed: each table (rows and index)
   is read from one immutable version throughout, and the writer is never blocked. **Not
   guaranteed: that the two versions are one globally atomic point in time; and it is two
   handles, not the literal single handle.** This requirement is therefore recorded as a
   deviation, not as satisfied. No global cross-store versioning was added and the tables were
   not merged into one store.
4. The secondary index, like `Snapshot`, exists on the `rcu` build only. On the lock-based
   baselines `create_index` returns `false` and `index_get` returns 0.
