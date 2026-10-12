# Post-B2 source-level safety audit

Date: 2026-10-12. Branch `part-b/rcu`. Audited: tag `part-b-final` (`304a8ac`) and, after the
corrections below, commit `81917be`. This is an audit of the code as written, read line by
line against `docs/reclamation.md`; tests and sanitizers are reported separately and are not
used as the argument.

**Verdict: B2 AUDIT PASS — READY FOR C1**, after two corrections made during the audit
(§10). The tag `part-b-final` predates them and was not moved (open item O-17).

## Scope completed

1. Pushed `part-b/rcu` and `part-b-final` (ordinary pushes; hashes compared with the remote).
2. Source audit of the EBR protocol: every protocol atomic, its memory order, seven scenarios.
3. Source audit of ownership and reclamation.
4. Reclaim-batch semantics against the PDF.
5. The header requirement, decided from the PDF's wording.
6. Part B deliverables checked against the PDF, not against the existing checklist.
7. Full verification re-run on the corrected commit.
8. C1 prerequisites.

Not done, by instruction: C1, Part D, profiling, any change to Part A results, any tag move.

## Requirements covered (PDF section/page)

PDF p. 4 (Part B), Appendix A.1 (`set_reclaim_batch`), pp. 4–5 and Appendix A.1 (C.1, for
readiness only).

## 1. Push

| Ref | Local | Remote (`git ls-remote origin`) |
|---|---|---|
| `refs/heads/part-b/rcu` at push time | `304a8acaa6d222e62fea661f535332ac5911e4b0` | same |
| `refs/tags/part-b-final` (tag object) | `9b35663b02e1123c50c002e54116fc570f693228` | same |
| `part-b-final^{}` (commit) | `304a8acaa6d222e62fea661f535332ac5911e4b0` | same |
| `refs/heads/main` | `a3cccd84dec0f1df3e522868f60f064986549df6` | same, unchanged |

Before pushing: `git fetch` showed the remote branch at `bbe44af`, an ancestor of the local
branch (fast-forward), and no unexpected refs. Earlier tags on the remote are unchanged
(`a0-bootstrap` 2ef5f19, `part-a-core` 80bc092, `part-a-final` 3444f23, `part-b-publication`
2d9c02c). No force push.

- https://github.com/krishna-prabu-kgpian/NOVA-KV/tree/part-b/rcu
- https://github.com/krishna-prabu-kgpian/NOVA-KV/releases/tag/part-b-final
  (or `/tree/part-b-final`)

The audit corrections were committed afterwards (`81917be`, then this report) and pushed to
the same branch with an ordinary push; the final hashes are in "Git commit SHA" below. The tag
was not touched.

## 2. EBR protocol: operations and memory orders as written

Every protocol operation is `memory_order_seq_cst`. Non-`seq_cst` operations are listed in the
last rows with the reason each is harmless.

| Step | Code | Atomic operation | Order |
|---|---|---|---|
| Slot registration: find a free slot | `EpochDomain::acquire_slot`, under `registration_mutex_` | `owned.load`, `owned.store(1)` | seq_cst |
| Slot registration: new block | same | block built privately (`owned.store(1)` on its first slot: relaxed, unreachable); `last->next.store(block)` **[R1]** | seq_cst |
| Slot publication to the thread | `thread_slot_slow` | none: thread-local cache entry and `tl_epoch_last` | — |
| Announcement | `EpochDomain::pin` | `epoch_.load` **[P1]**; `slot->state.store(e)` **[P2]** | seq_cst |
| Validation and retry | `pin` | `epoch_.load == e` **[P3]**, else loop from [P1] | seq_cst |
| Root acquisition | `VersionedStore::get`, `acquire_pin` | `current_.load` **[P4]**, once, after `pin` returns | seq_cst |
| Reader exit | `EpochDomain::unpin` | `slot->state.store(INACTIVE)` **[P5]** | seq_cst |
| Slot return | `release_slot` | `state.store(INACTIVE)` then `owned.store(0)` | seq_cst |
| Take ownership before publication | `commit` phase 4 | new segment: `refs.fetch_add` per listed chunk, `owners.store(1)`; shared segment: `owners.fetch_add(1)` | seq_cst |
| Publication | `commit` phase 5 | `current_.exchange(next)` **[W1]** | seq_cst |
| Retirement tag | `commit` phase 6 | `epoch_.load` **[W2]**, after [W1]; written to a plain field of an unreachable node | seq_cst |
| Queue hand-off | `commit` phase 6 | `queue_tail_->next.store(node)` **[W3]** (node's own `next.store(nullptr)` is relaxed: unreachable) | seq_cst |
| Collector scan | `try_advance` | `epoch_.load` **[C1]**; `b->next.load` **[C2]**; `s.state.load` **[C3]** | seq_cst |
| Gated advance | `try_advance` | `epoch_.compare_exchange_strong(e, e+1)` **[C4]** (one order argument: failure is seq_cst too) | seq_cst |
| Eligibility | `epoch_pass` | `queue_head_->next.load` **[C5]**; `epoch_.load() < tag + 2` → stop **[C6]** | seq_cst |
| Segment destruction | `run_job` | `owners.fetch_sub(1) == 1` → this thread destroys | seq_cst |
| Chunk destruction | `PinnedArena::drop_ref` / `destroy` | `refs.fetch_sub(1) == 1`; `slot.store(nullptr)`; free; push ID | seq_cst |
| Chunk-ID pool | `push_free_id`, `pop_free_id` | `free_head_.load`; `next_free[id].store/load`; `free_head_.compare_exchange_weak` | seq_cst |
| Chunk creation | `new_chunk` | `refs.store(1)` relaxed (unreachable); `blocks_[i].store(block)`; `slot.store(c)` | seq_cst |
| One pass at a time | `reclaim_pass` | `pass_running_.exchange(true)` / `store(false)` | seq_cst |
| *Relaxed, not protocol* | statistics counters; `ReaderSlot::retries` (owner-only RMW); writer's `blocks_[i].load` of a block only it installs; destructors (single-threaded by contract) | | relaxed |

Plain (non-atomic) shared data and why it is not a race: a `RetireNode`'s `version` and
`retire_epoch` are written before [W3] and read after [C5] returned that node; `Version` and
`Segment` contents are written before [W1] and never again; chunk payload bytes are written
at offsets beyond every published `ValueRef`; `PinnedChunk::used/sealed/link` are writer-only.

### The seven scenarios, checked against the code

Notation: *R* validated epoch *e* at [P3]; *V* is the version *R* loads at [P4]; *r* is *V*'s
retirement tag.

1. **A reader registers during a collector scan.** If the slot is in an existing block, the
   scan either read it before [P2] (saw INACTIVE) or after (saw *e*). If the slot is in a
   block linked at [R1] after the scan passed the end of the list, the scan does not visit it.
   In both "missed" cases the advance *e* → *e*+1 may succeed. A further advance needs a pass
   whose [C1] reads *e*+1, i.e. starts after that CAS, hence after *R*'s [P3] and [P2] and
   after [R1]; its traversal loads every `next` after that and reaches the block; it reads *e*
   in *R*'s slot and returns without [C4]. So the epoch stays ≤ *e*+1. This is Lemma B, and I
   re-derived it from `try_advance` and `acquire_slot` as written: it holds. The alternative
   claim in the supplement (a missed reader blocks even the first advance) is false for this
   code and is not relied on (D-045).
2. **A reader announces an old epoch while advancement occurs.** [P1] reads *e*, the epoch
   becomes *e*+1 (or more: nothing was announced yet), then [P2] stores *e*. [P3] reads ≠ *e*
   and the loop repeats. Between the stale [P2] and the corrected one the slot can make one
   `try_advance` fail spuriously; that is a delay, not a safety issue. Nothing is dereferenced
   before [P3] succeeds (`pin` touches only `epoch_` and the slot).
3. **Validation just before or after an advance.** [P3] and [C4] are totally ordered. If [C4]
   (*e* → *e*+1) is first, [P3] fails and the reader re-announces *e*+1. If [P3] is first, the
   reader is pinned at *e* with the epoch at *e*+1: allowed by Lemma B, and covered by the
   two-advance eligibility rule.
4. **A writer publishes during those steps.** If [W1] precedes [P4], the reader gets the new
   version, which is not retired. If [P4] precedes [W1], then [P3] < [P4] < [W1] < [W2], so
   *e* ≤ *r* (the epoch never decreases: it changes only at [C4], by +1). With Lemma B the
   epoch stays ≤ *e*+1 ≤ *r*+1 < *r*+2 while *R* is pinned, and [C6] is the only way a queue
   entry becomes a job. The tag being read **after** the exchange is essential and is what the
   code does (`node->retire_epoch = epochs_.epoch()` follows `current_.exchange`).
5. **Two successful advances.** After two advances past *r* (epoch ≥ *r*+2), any participant
   still pinned validated an epoch ≥ *r*+1 (else Lemma B is violated), so its [P3] came after
   the advance to *r*+1, which is after [W2], after [W1]: its [P4] returned a newer version.
   No participant can hold *V*; none can obtain it again (the writer never republishes a
   retired pointer). Destroying *V* is safe.
6. **A reader suspended arbitrarily long.** Its slot keeps *e*; every later `try_advance`
   from *e*+1 fails at [C3]; nothing tagged ≥ *e* is ever eligible. No timeout, age or queue
   length appears anywhere in `epoch_pass` (checked: the only condition is [C6]). The cost is
   unbounded backlog, by decision (D-040).
7. **A reader exits and its slot is reused.** `release_slot` stores INACTIVE before clearing
   `owned`; only `acquire_slot` (under the mutex) sets `owned`. The next owner's critical
   sections are simply later ones in the same slot. **Here the audit found a defect**: at
   thread exit the slot was returned but the thread's fast-path entry still named it, so a
   read later in the same thread's teardown would announce in a slot that may belong to
   someone else (assumption A3 broken). Fixed; see §10.

**Proof review result.** With the correction of §10, every obligation of
`docs/reclamation.md` §5 (A2 and A3 established by the code; Lemma B; Theorem 1; §5.4–5.9) is
discharged by the code as written. A1 (one writer) and A4 (quiescent shutdown) are caller
obligations stated by the assignment and the blueprint; they are not enforced and not proved.
The argument remains manual, over the `seq_cst` total order; it is not machine-checked.

## 3. Ownership and reclamation

| Object | Reference held by | Taken | Dropped | Final destruction |
|---|---|---|---|---|
| Writable chunk | the writer (birth reference) | `new_chunk` | when it is sealed the same reference becomes a commit reference; `end_commit`/`rollback_commit` drop it | `drop_ref` decrement to zero |
| Chunk used by an open commit | the commit (`commit_refs_` list): chunks it created, and the chunk that was writable when it began if it moved on | `append` | `end_commit`, after every new segment did `add_ref`; or `rollback_commit` (which keeps the reference of the chunk that becomes writable again) | same |
| Chunk listed by a segment | the segment, one per distinct chunk | `commit` phase 4, before [W1], never from zero | `run_job`, when the segment is destroyed | same |
| Segment | each version pointing at it (`owners`) | phase 4: `store(1)` or `fetch_add` from the current version's reference | `run_job`, per directory entry | `fetch_sub` returning 1 |
| Current version | `current_` / `latest_` | [W1] | [W1] of the next commit moves it to the writer's local, then to a queue node [W3] | — |
| Retired version | exactly one of: queue node, `job_` | [W3]; [C6] | end of `run_job` | `run_job`, once |
| Reader protection record | the thread (cache) or the `Pin` | `acquire_slot` | `release_slot` (thread exit; `release_pin`) | with the domain |
| Chunk-directory entry | — | `new_chunk`: `slot.store(c)` before any `ValueRef` naming it is published | `destroy`: `slot.store(nullptr)` first | blocks freed with the arena |
| Chunk ID | pool or exactly one live chunk | `pop_free_id` (writer only) or fresh | `destroy`: pushed last, after the memory is freed | — |

Checks, each against the implementation:

- **The reproduced chunk-lifetime bug is fixed.** In B1, a commit that sealed its writable
  chunk left it with only segment references, and the cleaner could drop the last one while
  the commit still held unpublished `ValueRef`s into it. Now `append` moves the sealed chunk,
  with its reference, onto `commit_refs_` (`pinned_arena.cpp`, the `current_->link =
  commit_refs_` branch); the cleaner's `release` can take `refs` no lower than 1. There is no
  `c != current_` test and no "load the count, then free" anywhere in the file.
- **The `end_commit` / unpin double free is fixed.** Both paths end in `drop_ref`, a single
  `fetch_sub`; `destroy` is called only by the caller that got 1. `grep` confirms `destroy(`
  has exactly one call site.
- **A commit cannot lose a chunk it needs.** A builder's `ValueRef` names either a chunk
  listed by the base segment (alive: `base` is current and is retired only by this same
  commit's [W3], after phase 4) or a chunk covered by the writable/commit reference until
  `end_commit`, which runs after the `add_ref` loop.
- **Only one thread performs final destruction.** Segments: `fetch_sub == 1`. Chunks:
  `fetch_sub == 1`. Versions: single owner at every moment (table above).
- **Writer and cleaner cannot free or reuse one ID incorrectly.** An ID is in the pool at most
  once (pushed once per destruction; a chunk is destroyed once). Pop is writer-only, so the
  head it read cannot be popped and re-pushed underneath it; concurrent pushes change the head
  and fail its CAS. The slot is null before the push, so `new_chunk` always stores into a null
  slot.
- **A reclaimed version is inaccessible to every reader.** §2, scenarios 4 and 5.
- **No shared segment dies while a retained version references it.** `owners` counts the
  versions whose entry `run_job` has not yet processed; `run_job` processes a version only
  after its own [C6].
- **Cleanup after allocation failure.** Every allocation of a commit precedes phase 4. `fail()`
  rolls the arena back, frees the raw `next`/`node` storage, and lets the builders free their
  private copies; no `owners` or segment reference has been touched, `current_` is unwritten.
  `rollback_commit` releases only references the failed commit itself held. One finding: a
  slot block leaked if registration was attempted on a domain whose first block had failed to
  allocate (the store is `!ok()` in that case); fixed in §10.
- **Shutdown.** `~VersionedStore`: `stop_cleaner()` (flag + join), then `destroy_everything()`
  on the calling thread through the same `run_job`, then member destructors (`arena_`, then
  `epochs_`, which leaves the live-domain table before freeing its slots). Readers, pins and
  the writer must be gone (A4); that is the documented contract and is not checked at run time.

One hazard recorded, not a defect today: the stop-the-world **comparator** frees the whole
retire list under its exclusive lock without consulting pins. `get` holds the lock shared, a
`Pin` holds nothing. No code combines the two and the public API cannot create a pin yet. It
must be closed in C1 before `Snapshot` exists (D-049, O-16).

## 4. Reclaim-batch semantics

PDF p. 4: "each call may free at most as many retired versions as the current reclaim-batch
setting, and must never block a reader or the writer. A stop-the-world sweep over the entire
retire list does not satisfy this."

| Check | Finding in the code | Test evidence |
|---|---|---|
| Maximum versions freed per pass | `epoch_pass`: `while (freed < batch)`; `++freed` only after `run_job` returned true (every reference dropped, memory freed) | `reclamation_test::test_batch_limit`; new `kvstore_test_rcu::test_public_reclaim_batch_setter` through the public API |
| Resumable partial destruction | `run_job` keeps `Job{version, partition, segment, pin}`; a pass that runs out of budget leaves it and the next pass continues; never re-processes an entry | `test_cleanup_interrupted_and_resumed` |
| Progress after a stalled reader exits | no state remembers the stall: the next `try_advance` simply succeeds | tail of every `reader_parked` schedule; long-lived pin test |
| Memory and queue under heavy writes | no backlog limit; writer never waits; measured in B2: backlog ≤ 569, 17–19 MiB held with an unthrottled writer (`results/b2/`) | `reclamation_stress_test` memory check |
| Settings 1, 8, 64, 256, 1024 | per-pass maximum equals the setting when enough is eligible | tables below |
| Zero and negative | treated as 1 (`n <= 0 ? 1 : n`), in `KVStore::set_reclaim_batch` | same |
| Never blocks reader/writer | the pass takes no lock a reader or the writer takes (Epoch mode); it reads slots and the queue | code |
| Not a whole-list sweep | stops at the batch, at the first ineligible entry, or at the work budget | code |

**Two budgets, kept apart.** The *version-count budget* is the assignment's tunable `n`. The
*work budget* (`n × 1024` units; one unit = one directory entry visited or one chunk
reference dropped) is an internal safeguard so that one unusually expensive version cannot
make a pass long; it is set only through an internal test interface. `docs/reclamation.md` §4
and `reports/part_b_readme.md` now say this explicitly. With defaults a version costs a few
hundred units, so `n` is what ends a pass.

`reclamation_test::test_batch_limit`, 3000 retired versions, passes run by hand (identical in
debug, release, ASan+UBSan and TSan; `results/B2_audit/key_test_output.txt`):

| `set_reclaim_batch(n)` | Effective | Passes to free 3000 | Most freed in one pass |
|---:|---:|---:|---:|
| 1 | 1 | 3001 | 1 |
| 8 | 8 | 376 | 8 |
| 64 | 64 | 48 | 64 |
| 256 | 256 | 13 | 256 |
| 1024 | 1024 | 4 | 1024 |
| 0 | 1 | 3001 | 1 |
| −5 | 1 | 3001 | 1 |

The new `kvstore_test_rcu::test_public_reclaim_batch_setter` repeats this through
`KVStore::set_reclaim_batch` alone for the same seven settings (per-pass maximum equals the
effective batch; default is 64; a change on a live store applies to later passes). Resumable
destruction: 2 versions took 46 passes of ≤ 16 units, 43 interruptions, not counted until
finished.

## 5. Header requirement

PDF p. 4, verbatim (line breaks removed): "Add a private member controlling how many retired
versions a reclamation pass frees at once, plus a public setter, void set_reclaim_batch(int
n), declared alongside put/get/erase in kvstore.h and defined in kvstore.cpp." Appendix A.1
lists only the setter ("added to KVStore: void set_reclaim_batch(int n);").

Reading: the clause "declared … in kvstore.h and defined in kvstore.cpp" attaches to the
setter. But the sentence does require *a private member*, and a private member paired with a
public setter of `KVStore` is a member of `KVStore`, whose definition is `kvstore.h`. A field
of the private implementation object is not a private member of that class. So the literal
form is required.

**Decision (D-047): implemented.** `include/kvstore.h` now declares
`std::atomic<int> reclaim_batch_;` in the private section. It is not a mirror: the engine is
constructed with a pointer to it and every pass loads the batch from it. No public signature
changed (`api_conformance_test` passes unchanged); the header gained `#include <atomic>`. The
lock variants record the value and ignore it (they retire nothing).

## 6. Part B deliverables, checked against the PDF

| PDF requirement (p. 4) | Where, verified by reading | Status |
|---|---|---|
| Change inside `KVStore`, not a forked store | one `include/kvstore.h`, one `src/kvstore.cpp`; `rcu` is a build of the same class (accepted, D-038) | PASS |
| New version built aside, one atomic pointer swap | `VersionedStore::commit`: one `current_.exchange` per logical write, after everything is built | PASS |
| Reader: exactly one atomic load of the pointer, consistent for the operation | `get`: one `current_.load`; everything reached is immutable | PASS |
| EBR or hazard pointers; say which and why | `src/epoch.*`; `reports/part_b_readme.md` "Reclamation: epoch-based, and why" | PASS |
| Private member + public `set_reclaim_batch(int)` in `kvstore.h` / `kvstore.cpp` | §5 | PASS (after correction) |
| Bounded pass, never blocks reader or writer, not a full sweep | §4 | PASS |
| Modified `kvstore.cpp` / `kvstore.h` and the diff against Part A | `results/diffs/part_a_to_part_b.diff`, regenerated: 20 files, 2631 insertions, 27 deletions; a gate now fails if it is stale | PASS |
| README describing the reclamation design and the tunable | `reports/part_b_readme.md` (140 lines); design and proof `docs/reclamation.md` | PASS |
| Stress test: readers hammer `get`, writer hammers `put`, no torn row | `tests/concurrent_lock_test.cpp` built as `concurrent_lock_test_rcu`; `reclamation_stress_test` | PASS |
| Reclamation batch tests | `reclamation_test::test_batch_limit`, stress test at 4 batch sizes, public-API test | PASS |
| Reproduction instructions | `README.md` "Build and test", "Part B store"; `reports/part_b_readme.md` "Build and run"; `reports/milestones/B2.md` | PASS |
| Part B completion checkpoint | tag `part-b-final` = `304a8ac` (pushed). It lacks the two audit corrections; see O-17 | PASS, with that note |

## Changed files and why

| File | Change | Why |
|---|---|---|
| `include/kvstore.h` | private `std::atomic<int> reclaim_batch_`; `#include <atomic>` | D-047 |
| `src/kvstore.cpp`, `src/kvstore_impl.h`, `src/versioned_store.h/.cpp`, `src/config.h` | the engine reads the batch through a pointer to that member; default in `config.h` | D-047 |
| `src/epoch.h/.cpp` | `thread_slot_late`; cache destructor clears the fast-path entry; `new_slot_block` with a thread-safe failure hook; leak on a failure path fixed | D-048 |
| `tests/epoch_test.cpp` | `test_read_after_thread_cache_destroyed`, `test_slot_block_allocation_failure` | D-048 |
| `tests/reclamation_test.cpp` | `test_get_when_no_reader_slot_can_be_allocated` | D-048 |
| `tests/kvstore_test.cpp` | `test_public_reclaim_batch_setter` | D-047 |
| `scripts/verify_all.sh` | `part_b_artifacts`: member present; exported diff matches HEAD | keep the artifact honest |
| `results/diffs/*` | regenerated | source changed |
| `docs/reclamation.md`, `docs/part_b_compliance.md`, `reports/part_b_readme.md`, `docs/decisions.md`, `CLAUDE.md` | brought in line; D-047…D-049; O-15 closed, O-16/O-17 opened | — |

## Design decisions and invariants

D-047 (literal private member, one copy of the setting), D-048 (a thread never announces in a
slot it has returned), D-049 (stop-the-world comparator must become pin-aware in C1). No
protocol operation, memory order or ownership rule was changed.

## Build commands and exact exit statuses

`scripts/verify_all.sh` on a clean tree at `81917be`, every mode from a deleted build
directory; cmake 4.2.3, GCC 15.2.0. **Exit 0; 19 of 19 mandatory gates PASS**
(`results/verification/81917be4e6aa/summary.md`). Informational `perf_stat_usable`: FAIL
(`perf_event_paranoid = 4`).

| Mode | Configure + build exit | Warnings | Tests | `ctest` exit | Wall time |
|---|---:|---:|---|---:|---:|
| Debug | 0 | 0 | 47/47 | 0 | 42.6 s |
| Release | 0 | 0 | 47/47 | 0 | 31.3 s |
| ASan + UBSan | 0 | 0 | 47/47 | 0 | 92.3 s |
| TSan | 0 | 0 | 47/47 | 0 | 189.8 s |

Clean checkout (`results/B2_audit/clean_clone_build.log`): `git clone`, checkout `81917be`,
release configure exit 0, build exit 0 (0 warnings), `ctest` exit 0, 47/47.

## Tests run (PASS/FAIL/NOT RUN)

All 47 CTest tests PASS in all four modes. Run again by hand in each verified build directory,
with full output kept (`results/B2_audit/key_test_output.txt`, 32 runs, all exit 0):

| Requested check | Program | Result (each of 4 modes) |
|---|---|---|
| Deterministic reclamation tests | `reclamation_test` | 56,760 checks, 0 failed |
| Epoch protocol, slots, thread exit, teardown reads, slot-allocation failure | `epoch_test` | 6,908 checks, 0 failed |
| Chunk ownership regressions (audited schedules) | `pinned_arena_test`; `reclamation_test` (`commit_seals_chunk_while_cleaner_runs` ×2, `end_commit_races_with_release`); `versioned_interleaving_test` | 4,228 / — / 573 checks, 0 failed |
| Long-lived reader and pin | `reclamation_test` (five parked-reader schedules; pin moved between threads), `reclamation_stress_test` | 0 failed; 120,178 checks in the stress test (debug) |
| Allocation failure | `versioned_alloc_failure_test`; new slot-block tests | 0 failed |
| No torn read | `concurrent_lock_test_rcu` | 0 torn |
| Public API incl. reclaim batch | `kvstore_test_rcu` | 22,182 checks, 0 failed |

New tests added by the audit: 4 (two in `epoch_test`, one in `reclamation_test`, one in
`kvstore_test`). The late-read test fails (abort) on the code at `part-b-final`.

NOT RUN: machine-checked verification; profiling (perf unavailable); the Part D campaign; the
B2 mutation set was not repeated (the new late-read test was shown to fail on the unfixed
code, see §10).

## Sanitizer / race checks (PASS/FAIL/NOT RUN)

| Check | Result |
|---|---|
| ASan + UBSan, 47 tests | PASS, exit 0, 0 reports |
| TSan, 47 tests | PASS, exit 0, 0 reports |
| Key programs by hand under both | PASS, 0 reports in the captured output |

These show that no problem was observed in the executions that ran. The correctness argument
is §2–§3 and `docs/reclamation.md` §5.

## Benchmarks and measured environment (if applicable)

None run for this audit. The B2 observation set (`results/b2/`) stands as **preliminary**:
battery, `powersave` governor, three repetitions. Ratios in `reports/milestones/B2.md` compare
only runs of the same session and are not controlled speedup claims. Part A results are
byte-identical to `part-a-final` (gate `part_a_checkpoint_unchanged`).

Setup needed before Part D (nothing here was changed; no `sudo`):

- `kernel.perf_event_paranoid ≤ 1` (currently 4; it reverts on reboot). As root:
  `sysctl kernel.perf_event_paranoid=1`, or persistently a file in `/etc/sysctl.d/`.
- Mains power and the `performance` governor for the whole campaign; `conditions.log` is
  checked and a campaign whose conditions change part-way is rerun (D-029).
- A quiet machine (the B2 runs saw load averages up to 5).
- For D.2, a workload in which reclamation can fall behind, or the batch size has nothing to
  trade off (B2 observation).

## Raw evidence paths and SHA-256 hashes

| Path | SHA-256 |
|---|---|
| `results/verification/81917be4e6aa/summary.md` | `b06f947d274a40dca259f2d67dc02cf888592467c1b157ced4e7d3701e51e980` |
| `…/logs/debug_ctest.log` | `40409a661c800e19f263839f2696f36e16998ceaeb837cadede9050ddbdd5d21` |
| `…/logs/release_ctest.log` | `1798de06c86278b84ed1cd8ad9f3f8e606c7d6040a224b5eee680f7a320141da` |
| `…/logs/asan_ubsan_ctest.log` | `86172f090ce16d74879db69241d5d4446cee1970760f94294c9e4fb9603574d3` |
| `…/logs/tsan_ctest.log` | `225ad706d104403355d51a0e3f3b479965a2263f6f152407f2440b9c984abfb7` |
| `results/B2_audit/key_test_output.txt` | `f47491c99bd52bbe65f1d31fb607af5b436a2c87f339057d89f2665038abb05f` |
| `results/B2_audit/clean_clone_build.log` | `61913feb0eb4e1e73c2493df51c459a48f253084318598ab0e2c212f5d1c8cda` |
| `results/B2_audit/late_read_reproducer_before_fix.txt` | `9972161383fbe8e76e65f18e517aa72ae5a4b08c16cf6cc2c0efd1150970bfdb` |
| `results/diffs/part_a_to_part_b.diff` | `7c3046fbf9d02345f236bf4ebef6b710527eaef852c92a341f1da262e349e05f` |

## Profiling bottlenecks and next hypotheses

Not applicable to this audit. Carried from B2 unchanged: ownership counters on reader-visible
cache lines (R8), cleaner CPU under an unthrottled writer, linear slot scan at registration.

## 10. Defects found and corrections made

1. **Read during thread teardown reused a returned reader slot (safety; fixed).** Found by
   reading `src/epoch.cpp` against assumption A3. `~EpochThreadCache` returned the thread's
   slots but left `tl_epoch_last` naming one, and `thread_slot_slow` used the cache's vector
   after its destruction. A `get` from a destructor that runs later on the same thread
   (another `thread_local` object; a static object on the main thread) announced in a slot
   that might already belong to another participant, or corrupted memory. Reproduced: the new
   test aborts with `free(): double free detected` on the unfixed code
   (`results/B2_audit/late_read_reproducer_before_fix.txt`). Fix: the destructor clears the
   entry and sets a trivially destructible flag; later lookups take `thread_slot_late` and
   get a slot of their own. Not reachable from any existing test, driver or benchmark, but a
   real hole in "a reader exits and its slot becomes reusable". **`part-b-final` contains it.**
2. **The private member was not in `kvstore.h` (compliance; fixed).** §5.
3. **Untested failure path (test gap; closed).** `get` throwing `std::bad_alloc` when a slot
   block cannot be allocated had no test, because slot blocks cannot use the single-threaded
   failure counter. Added a thread-safe hook and two tests; the path behaved as documented.
4. **Leak on a failure path (minor; fixed).** A slot block was allocated and dropped when
   registration was attempted on a domain whose first block had failed to allocate.
5. **Wrong number in a commit message (record; corrected in B2.md).** 93 runs, not 126.
6. **My new test had a wrong expectation at first** (3001 puts retire 3001 versions, not
   3000, because the first put retires the initial empty version). The test was corrected; no
   engine behaviour was involved.

## Known limitations and source ambiguities

Remaining safety or API concerns:

- A1 (single writer) and A4 (no reader, pin or writer during destruction) are not enforced or
  detected.
- The stop-the-world comparator is not pin-aware (D-049); must be fixed in C1.
- The safety argument is manual, not machine-checked; it assumes `seq_cst` throughout.
- A stalled reader or pin stops all reclamation; memory grows until allocation fails (by
  decision).
- `get` can throw `std::bad_alloc` on a thread's first read. Appendix A does not forbid it
  (no `noexcept`), but a caller that does not expect exceptions will terminate.
- A thread that reads during its own teardown keeps one slot owned until the store is
  destroyed (bounded: one per such thread).
- Registration takes a mutex and scans the slot list linearly: irrelevant for `get`
  (once per thread), relevant for C1 if `acquire_snapshot` is called per join.
- `part-b-final` predates corrections 1 and 2 (O-17).
- R8 (counter placement) and probe-length instrumentation remain open from B2.
- OPEN / INSTRUCTOR: exactly-one-Snapshot across two stores (O-1).

## 9. C1 readiness

What B2 already provides: `VersionedStore::commit(ops, n)` (one publication for any number of
keys, all fallible steps first, last write to a key wins, a no-change batch publishes
nothing), tested under allocation-failure injection; `Pin` with `acquire_pin`, `get_pinned`,
`release_pin`, movable between threads without touching the slot; the reclamation protocol
that keeps a pinned version and everything reachable from it alive.

Missing, to be built in C1 (none blocks starting it):

| # | Prerequisite | Note |
|---|---|---|
| P1 | Stop-the-world comparator pin-aware | D-049. First task of C1 |
| P2 | `Snapshot` holds one `void* pin_`; a `Pin` is two words | Either allocate the pin record (then `acquire_snapshot` allocates) or keep the version pointer in the reader slot, which has room in its cache line (no allocation). Decide and record |
| P3 | Owner validation | `get(snap, …)`, `keys(snap, …)` must check `snap.owner_ == this` and that the handle is not moved-from; define the result (false / 0) |
| P4 | `keys(snap, …)` | no enumeration of a version exists yet; a walk over the pinned version's immutable segments |
| P5 | Snapshot destroyed after its store | contract violation today (A4). Either document, or make release safe with the live-domain table |
| P6 | `Transaction` staging | `put` returns `void`: it must copy the value and defer any allocation failure to `commit()` returning false; define commit-twice, empty commit, duplicate keys (engine: last wins) |
| P7 | `Transaction` on the lock variants | the supplied workload links against every variant; decide whether the baselines get a real (one lock hold) implementation |
| P8 | `acquire_snapshot` cost | registration mutex + linear slot scan per call; measure before D.3, consider a per-thread free list |
| P9 | Tests named by blueprint §8.7 | 1/4/16/100-key atomicity, different segments, fail after n-th allocation, abandoned transaction, empty commit, snapshot across many commits, moved snapshot, no chunk reuse while pinned |

## Git commit SHA, tag, diff summary

| Commit | Content |
|---|---|
| `304a8ac` | tag `part-b-final` (pushed; unchanged; pre-audit) |
| `81917be` | audit corrections and their tests. **Verified commit.** |
| (next) | this report and the verification evidence; no source change |

`git diff --stat 304a8ac 81917be`: source and tests of the table above. Part A → Part B diff:
20 files, 2631 insertions, 27 deletions. `main` untouched (`a3cccd8`); nothing merged; no tag
moved or created.

## Gate verdict: PASS / FAIL / BLOCKED

**PASS.** Every proof obligation of the EBR argument is discharged by the code as corrected;
the two reproduced B1 ownership bugs are fixed in the implementation, not only in tests; the
batch limit and the header requirement meet the PDF's wording; all Part B deliverables exist;
19/19 mandatory gates pass on the corrected commit in four build modes.

Qualifications, none of which is an unresolved safety obligation for Part B: the argument is
manual; A1 and A4 are caller contracts; the stop-the-world comparator must be made pin-aware
at the start of C1; `part-b-final` predates the corrections.

**B2 AUDIT PASS — READY FOR C1**

## Next milestone proposed (not started)

C1 (blueprint §8.7), starting with P1. Needs your approval, and your decision on O-17 (which
tag marks the audited Part B state; the B→C diff should start from it).
