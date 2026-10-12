# Part B compliance checklist

Requirement by requirement, from `assignment2.pdf` page 4 (Part B), page 7 (marks) and pages
8–10 (Appendix A). Status values: PASS (artifact exists and its check ran and passed),
PARTIAL, FAIL, NOT RUN. Statuses were filled in from the verification run recorded in
`reports/milestones/B2.md`.

## A. RCU reads

| # | Requirement (PDF p. 4) | Artifact | Check | Status |
|---|---|---|---|---|
| A1 | "The change must live inside the KVStore class itself: don't fork the store into a second implementation." | One `KVStore` class, one `kvstore.h` / `kvstore.cpp`; `rcu` is a build policy of it (`src/sync_mode.h`); engine in `src/versioned_store.*` | all variants build from one tree; accepted by the user (decision D-038) | PASS, with the recorded interpretation: the Part A lock variants keep their in-place engine |
| A2 | Writer "builds a new version of the table (or just the buckets it touched...) off to the side" | `VersionedStore::commit` phases 1–4; `SegmentBuilder` | `cow_segment_test` (published base byte-for-byte untouched), `versioned_store_test` (one clone per touched segment) | PASS |
| A3 | "publishes it with one atomic pointer-swap of a current version pointer" | `current_.exchange(next)` `[W1]` | publication counters = commits; interleaving S3/S4/S5; mutation "two publications" detected | PASS |
| A4 | "A reader takes exactly one atomic load of that pointer at the start of its operation" | `VersionedStore::get` `[P4]`: one `current_.load` per call. It is preceded by the reader's epoch announcement, which reads the epoch counter, not the version pointer | code: single load site in `get`; schedules with a reader parked after it | PASS |
| A5 | "sees a consistent snapshot for the rest of that operation no matter what the writer does" | immutable versions; EBR keeps the loaded version alive | reader parked across 2000–3051 publications returns its original bytes | PASS |
| A6 | Single writer thread may be assumed | documented; not enforced | — | PASS (assumption used) |
| A7 | `get()` copies the value out while the read-side critical section is open (p. 10) | `read()` copies before `unpin` `[P5]` | buffer-guard checks in `kvstore_test_rcu`; schedule with a reader parked before the copy | PASS |

## B. Bounded-pause reclamation

| # | Requirement (PDF p. 4) | Artifact | Check | Status |
|---|---|---|---|---|
| B1 | Old versions "can't be freed the instant they're unpublished"; implement epoch-based reclamation **or** hazard pointers | Epoch-based: `src/epoch.*`, `VersionedStore::epoch_pass` | `reclamation_test`, `epoch_test` | PASS |
| B2 | "State which one you picked and why." | `reports/part_b_readme.md` ("Reclamation: epoch-based, and why") | read by a human | PASS |
| B3 | "readers announce an epoch, and a version is freed once no reader is still in an older epoch" | announce + validate `[P1]–[P3]`; gated advance `[C1]–[C4]`; eligibility `[C6]` (two advances beyond the retirement tag) | `test_two_advances_required`; five reader-parked schedules; mutations (one advance, unconditional advance, root loaded before announcing) all detected | PASS |
| B4 | "Add a private member controlling how many retired versions a reclamation pass frees at once" | `VersionedStore::reclaim_batch_` (private `std::atomic<int>`), reached through `KVStore`'s private `impl_` | see note 1 | PASS, see note 1 |
| B5 | "plus a public setter, `void set_reclaim_batch(int n)`, declared alongside put/get/erase in kvstore.h and defined in kvstore.cpp" | `include/kvstore.h`; `src/kvstore.cpp` | `api_conformance_test` pins the signature | PASS |
| B6 | "each call may free at most as many retired versions as the current reclaim-batch setting" | `epoch_pass`: `while (freed < batch)`; a version counts only when completely destroyed | `test_batch_limit` for 1, 8, 64, 256, 1024, 0, −5: maximum freed in any pass ≤ batch; stress test asserts the same under concurrency; mutation "batch not applied" detected | PASS |
| B7 | "must never block a reader" | readers take no lock; the cleaner only reads their slots | no lock type in the `rcu` build's read path; TSan/stress | PASS |
| B8 | "must never block ... the writer" | hand-over is one store into a pre-allocated node; no grace-period wait | writer-parked schedules; code | PASS |
| B9 | "A stop-the-world sweep over the entire retire list does not satisfy this." | a pass stops at the first ineligible version, at the batch limit, or at the work budget; destruction of one version is resumable | `test_cleanup_interrupted_and_resumed` | PASS |

Note 1. The assignment says "a private member". The store's state is behind a private
implementation pointer (`KVStore::impl_`, decision D-006), so the batch size is a private data
member of the implementation object `KVStore` privately owns, not a field spelled out in
`kvstore.h`. It is not accessible to users of the class by any route but the setter. If the
grader requires the member to appear literally in `kvstore.h`, that is a one-line change
(move the atomic into `KVStore`); flagged here rather than assumed.

## C. Appendix A.1 statement about single calls

| # | Requirement | Artifact | Status |
|---|---|---|---|
| C1 | "A single get() or erase() call ... acquires a snapshot internally and releases it when the call returns" (p. 4, p. 9) | `get`: pin, load, read, unpin. `erase` is a writer operation: it reads the current version it alone can replace | PASS |
| C2 | Explicit reusable snapshot handle | Part C. Internal foundation only: `VersionedStore::Pin` (dedicated slot, movable, tested) | NOT RUN (Part C) |

## D. Deliverables (p. 4)

| # | Deliverable | Artifact | Status |
|---|---|---|---|
| D1 | "your modified kvstore.cpp / kvstore.h" | `src/kvstore.cpp`, `include/kvstore.h` and the engine files they use | PASS |
| D2 | "the diff against your Part A version" | `results/diffs/part_a_to_part_b.diff` (`git diff part-a-final <B2 commit> -- src include`), plus `…_stat.txt`; reproducible with `git diff part-a-final part-b-final -- src include` | PASS |
| D3 | "a README.md describing your reclamation design and the tunable parameter" | `reports/part_b_readme.md` (linked from the repository `README.md`); design detail in `docs/reclamation.md` | PASS |
| D4 | "a stress test (concurrent readers hammering get while the writer hammers put) showing no reader ever sees a torn or partially-updated row" | `tests/concurrent_lock_test.cpp` built as `concurrent_lock_test_rcu`; `tests/reclamation_stress_test.cpp` | PASS (runs in all four build modes) |

## E. Marks table (p. 7)

| Item | Marks | Evidence |
|---|---:|---|
| RCU pointer-swap + lock-free snapshot reads | 15 | section A |
| Epoch- or hazard-pointer reclamation + tunable batch size | 15 | section B |
| README + no-torn-read correctness test | 10 | D3, D4 |

Whether the marks are awarded is the grader's decision.

## Known gaps and caveats

- Note 1 above (where the private member lives).
- A1's interpretation: `rcu` is a fourth build of the same class rather than a replacement of
  the lock variants.
- The safety argument (`docs/reclamation.md` §5) is a manual proof sketch.
- "Bounded pause" is bounded work per pass (version count and work units), not a bound on
  wall-clock time.
- `get` can throw `std::bad_alloc` on a thread's first read if a reader slot cannot be
  allocated. The assignment's signature does not forbid it, and it avoids reporting an
  existing key as missing.
- Measurements with reclamation are an observation set, not the Part D campaign.
