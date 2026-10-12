# Post-C1 source-level audit

Date: 2026-10-12. Branch `part-c/transactions`. Audited: tag `part-c-transactions`
(`1e6af10`, source as verified at `a409d6b`). This is a reading of the code as committed,
against the PDF, `docs/transactions.md`, `docs/reclamation.md`, the blueprint and the
formal-safety supplement. Tests are reported separately and are not the argument.

**Verdict: C1 AUDIT PASS — READY FOR C2.** No defect was found in the store; no file under
`src/` or `include/` changed. The audit changed tests (new deterministic progress tests) and
documents (an over-strong isolation claim removed).

## Scope completed

1. Pushed `part-c/transactions` and `part-c-transactions`; remote and local hashes compared.
2. Source audit: staging, commit, rollback, writer serialization, snapshot acquisition, move,
   destruction, snapshot `get` and `keys`, slot and pin ownership, concurrent reclamation.
3. The stop-the-world correction re-inspected.
4. The lowered threshold of D-056 audited with measurements; deterministic progress tests
   added.
5. Isolation claims reviewed and corrected.
6. C.2 requirements read; cross-store snapshot problem analysed; question for the instructor.
7. Full verification on the audit commit.

Not done, by instruction: any part of C2.

## Requirements covered (PDF section/page)

PDF pp. 4–5 (C.1, C.2 for readiness), Appendix A.1 and A.2.

## 1. Push

| Ref | Local | Remote (`git ls-remote origin`) |
|---|---|---|
| `refs/heads/part-c/transactions` at push time | `1e6af102bbd780275036bbc03b608b59f8b08209` | same |
| `refs/tags/part-c-transactions` (tag object) | `d130fc870c7604c216093b465c0b00a7d3c17d72` | same |
| `part-c-transactions^{}` (commit) | `1e6af102bbd780275036bbc03b608b59f8b08209` | same |
| `refs/heads/main` | `a3cccd84dec0f1df3e522868f60f064986549df6` | same, unchanged |
| `refs/heads/part-b/rcu` | — | `87d38aa`, unchanged |

Tree clean before pushing. The six earlier tags resolve to the same commits locally and on
the remote (`a0-bootstrap` 2ef5f19, `part-a-core` 80bc092, `part-a-final` 3444f23,
`part-b-publication` 2d9c02c, `part-b-final` 304a8ac, `part-b-audited` 87d38aa). Ordinary
pushes; nothing forced, merged or rewritten.

- https://github.com/krishna-prabu-kgpian/NOVA-KV/tree/part-c/transactions
- https://github.com/krishna-prabu-kgpian/NOVA-KV/tree/part-c-transactions

The audit's commits (`314d652`, then this report) were pushed to the same branch afterwards
with an ordinary push; final hashes under "Git commit SHA" below. The tag was not touched.

## 2. Transaction: findings from the code

| Property | Finding | Where |
|---|---|---|
| Staged values owned independently of caller buffers | `StagedWrites::put` `memcpy`s the bytes into the set's own buffer before returning; only `{key, offset, length}` is kept (an offset, not a pointer, so buffer growth cannot dangle). No caller pointer is stored anywhere | `src/staged_writes.cpp` |
| Growth cannot lose or corrupt | both arrays are allocated (if needed) before either is changed; then copied; a failure returns `false` with the set as it was | same |
| One successful commit publishes exactly one root | `Transaction::commit` → `Impl::commit_staged` → one call of `VersionedStore::commit` → one `current_.exchange` (the only `exchange`/`store` of `current_` on a write path; the others are construction and destruction) | `kvstore.cpp`, `versioned_store.cpp` |
| Failed commits publish nothing | every allocation of `commit_locked` is before the line "Nothing below this line can fail"; the exchange is after it. `commit_staged`'s own allocation is before the call | same |
| Duplicate keys | operations are applied in staging order to one private builder per partition; a later `commit_put` overwrites the earlier entry in the private copy and adjusts the chunk list; only the final state is published. Last write wins, as documented | `cow_segment.cpp` |
| Empty commit | `count() == 0` short-circuits to `true` without calling the engine | `Transaction::commit` |
| Repeated commit | `finished` is set first; any later call returns `false` before anything else; `put` after commit returns at once | same |
| Each touched segment cloned at most once | phase 1 assigns one builder per distinct partition (`builder_of[]`), `begin()` is called once per builder, and phase 4 calls `finish()` once per builder | `commit_locked` |
| Writer serialization covers put, erase, commit | `put` → `commit` (locks); `erase` locks, then checks presence, then `commit_locked`; `Transaction::commit` → `commit` (locks); the inspector's batch commit → `commit`. `commit_locked` has no other caller | `versioned_store.cpp` |
| No reader-side mutex introduced | `get`, snapshot `get`, `keys`: no mutex (a `shared_lock` object is constructed unlocked and locked only in the stop-the-world comparator, as before C1). **Qualification:** `acquire_snapshot()` obtains its slot through `EpochDomain::acquire_slot`, which takes the registration mutex. That mutex exists since B2 (a thread's first read) and C1 made it reachable once per snapshot. It is shared only among threads that are registering; the writer and the cleaner never take it, so it cannot block or be blocked by a commit or a reclamation pass | `epoch.cpp` |
| Allocation failure cannot leak | staged memory is owned by `StagedWrites` and freed by `clear()` / the destructor on every path; the operation array is freed on both outcomes; the engine's `fail()` path frees the raw version and node and the builders free their copies | all three files |
| Allocation failure cannot partially publish | as "failed commits publish nothing" | — |

### Lock-baseline Transaction (`single`, `global_lock`, `rwlock`)

Read as committed (`KVStore::Impl::commit_staged`, lock branch):

- The undo array is allocated **before** the lock is taken and before any change.
- One `WriteGuard` spans the whole batch: a reader under `rwlock`/`global_lock` sees none or
  all. (`single` has no lock and is single-threaded by definition.)
- Per operation the fallible steps (`prepare_upsert`, `arena.append`) precede
  `commit_upsert`, as in the Part A `put`.
- A replaced value's reference is **kept** in the undo record instead of being released, so
  its chunk stays alive for the undo. On success all of them are released; on failure the
  loop runs in reverse: a replaced key gets its old reference back (`prepare_upsert` on an
  existing key returns before any allocation), an inserted key is erased (no allocation), and
  the staged value's reference is released.
- Repeated keys work in both directions because each record's "old" is the previous staged
  value.
- After an undo the logical content is identical; physically a segment may have grown or hold
  tombstones, which the store's invariants allow (the inspector's `validate` passes in
  `txn_failure_test_single` after each of 108 failures).
- `put`/`get`/`erase` of the baselines are untouched (`git diff part-a-final
  part-c-transactions -- src/kvstore.cpp` shows only additions around them plus the
  constructor/setter change of the B2 audit).

Not covered by a test: baseline transactions under *concurrent* readers. The argument is the
single lock hold.

One theoretical gap, not fixed because it is unreachable: `n * sizeof(BatchOp)` (and the
baseline's `n * sizeof(Undo)`) is not overflow-checked. Staging needs 24 bytes per operation
from the allocator first, so `n` cannot approach 2⁵⁹.

## 3. Snapshot lifetime: findings from the code

**The opaque pin.** `Snapshot` = `{const KVStore* owner_; void* pin_}` (header unchanged).
`pin_` is a `nova::ReaderSlot*` in the store's epoch domain. The slot's `state` atomic holds
the announced epoch; its `token` field holds the `const Version*`. `token` is written by
`acquire_pin` (after the root load) and by `release_pin` (cleared), both by the handle's
owner; the collector reads only `state`, so `token` is not shared data. The slot lives in a
block that is never freed while the store exists.

| Property | Finding |
|---|---|
| `acquire_snapshot` pins one consistent published version | `acquire_pin`: `acquire_slot()` (may throw, nothing pinned yet) → `EpochDomain::pin` (announce `[P2]`, validate `[P3]`) → **one** `current_.load` `[P4]` → `token = version`. The version is whatever one committed root was current at that load; every object reachable from it was complete before its publication |
| Move transfers protection exactly once | move constructor and assignment copy the two words and null the source; no slot write, no epoch change. Assignment first calls `release()` on the target. Self-assignment is guarded |
| Moved-from is harmless to destroy | `release()` does nothing when `pin_` is null |
| Movement between threads | the slot is not thread-local and is not in any thread's slot cache, so the acquiring thread's exit does not return it. `release_slot` needs no thread affinity |
| Wrong-store use cannot dereference invalid memory | `pin_for` compares `owner_` with `this` and returns an inert pin **before** converting or dereferencing `pin_`; the callers return `false`/`0` on an inert pin |
| Destruction releases exactly once | `release()`: `token = nullptr`, `state = INACTIVE` `[P5]`, `owned = 0`; then the handle's own fields are nulled, so a second call is a no-op |
| The cleaner cannot reclaim protected versions or chunks | the slot announces epoch *e* from acquisition to release. By Lemma B the epoch stays ≤ *e*+1; a version is taken from the queue only when the epoch ≥ tag+2 (`epoch_pass` and, since C1, `stw_pass`); the pinned version is current or has tag ≥ *e*. Its segments keep `owners ≥ 1` and its chunks `refs ≥ 1` until `run_job` processes it |
| Chunk IDs cannot be reused while a snapshot needs them | an ID enters the pool only in `destroy`, reached only by the decrement of `refs` to zero, which cannot happen while a segment of a protected version lists the chunk |

Limits of what the code protects, all documented: a handle used after its store was destroyed
(or with a new store constructed at the old address) dereferences freed memory; a handle used
from two threads at once is a data race on the caller's object; a snapshot left alive stops
all reclamation of later versions.

## 4. Stop-the-world comparator

Read as committed (`VersionedStore::stw_pass`): takes `stw_lock_` exclusively, calls
`try_advance()` twice, then loops: finish the current job, else look at the oldest entry and
**break if `epoch < tag + 2`**, else take and release it completely. There is no other exit
that frees, and no path that frees without that comparison.

- **Epoch mode:** `epoch_pass` is unchanged from the audited B2 code.
- **Disabled mode:** `reclaim_pass` does nothing; versions are destroyed only by the
  destructor.
- **Stop-the-world:** with a live snapshot at epoch *e* the two advances reach *e*+1 at most,
  so no entry with tag ≥ *e* passes the comparison. The lock no longer decides anything about
  what is freed; it only excludes the short readers so that nothing but pins can hold the
  epoch.
- Snapshot-scoped `get`/`keys` take the shared lock per call in that mode. A handle is never
  locked between calls, so moving it between threads is unaffected.
- `destroy_everything` (destructor) is the one place that frees regardless of epochs, under
  the shutdown contract.

Permanent regression tests: `reclamation_test::test_pin_protected_in_mode` (epoch,
stop-the-world, disabled) and `snapshot_test::test_snapshot_survives_reclamation` (four
configurations, through the public API). The output of the first against the old code is
kept (`results/C1/stw_pin_reproducer_before_fix.txt`), and reverting the comparison is
mutation C1 (detected by both).

## 5. The lowered progress threshold (D-056)

**Finding: a scheduling-sensitive coverage heuristic; no progress regression.**

Measurement saved in `results/C1_audit/stress_epoch_advance_distribution.txt`: 100 sub-runs
each, release build, serial, one session (battery, `powersave`, load ≈ 1):

| Build | min | 10th pct | median | max | sub-runs ≤ 100 | checks failed |
|---|---:|---:|---:|---:|---:|---:|
| `part-b-audited`, unmodified (floor 100) | 99 | 150 | 228 | 2378 | 1 | 1 |
| C1 (floor 10) | 86 | 155 | 232 | 2246 | 1 | 0 |

(An earlier, unsaved sample gave min 58 for the Part B code and 113 for C1.) The two
distributions are the same, and the unmodified Part B code fails its own floor. Why the count
is what it is: the test has a thread that holds a pin almost continuously, and while a pin is
held the epoch can advance at most once more, so the number of advances is about two per pin
cycle — a property of the test's scheduling, not of how well reclamation works. In every one
of the 200 sub-runs the correctness and progress assertions held: backlog drained to zero,
reclaimed == published, per-pass maximum ≤ batch.

Because an advance count should not be what stands for "reclamation makes progress", two
deterministic tests were added to `reclamation_test` (decision D-058):

`test_reclamation_progress`, manual passes, batches 1 / 8 / 64 / 1024, three stall–release
cycles each:

| Required | Asserted |
|---|---|
| 1. A pinned reader can delay reclamation | over 3000 passes with a pin: backlog exactly 600 (= versions retired since the pin), 0 reclaimed, 0 chunks freed, epoch ≤ pin epoch + 1, ≥ 2990 advance attempts blocked, oldest active epoch = the pin's |
| 2. Reclamation resumes after its release | from the first passes after release the backlog falls on **every** pass by exactly the batch (or what is left); it never rises |
| 3. Retired versions are eventually reclaimed | backlog 0 within `600/batch + 3` passes; reclaimed == retired |
| 4. Memory/backlog metrics reflect actual progress | stalled: ≥ 600 × 20,000 bytes more held than idle, 206 chunks alive; after: 6 chunks, 392 KiB, i.e. the current state only; store validates |

Observed: 600 / 75 / 10 / 1 passes to drain at batch 1 / 8 / 64 / 1024.

`test_background_cleaner_resumes_after_release`: the same end state with the store's own
cleaner thread, three cycles: while pinned the backlog is exactly 400 although the cleaner
has been running; after release it reaches 0 without any call from the test.

## 6. Isolation documentation

One over-strong claim found and removed (decision D-057): `docs/transactions.md` §5, the C1
report and compliance note 1 said that under the assignment's single-writer assumption every
execution is serializable. Serializing commits orders the writes; it says nothing about the
reads a client based them on, and one thread working from an earlier snapshot can make a
stale decision by itself.

§5 now says, and only says:

- commits are serialized and each is published atomically;
- a `Snapshot` gives consistent reads of one version of one store;
- independent ordinary gets may observe different versions;
- a `Transaction` has no reads; there is no read set and no validation of any kind;
- **a client-side read followed by a commit is not one transaction**, and serializing the
  writers does not make it one;
- lost updates and write skew are possible (a table lists each property as provided or not);
- no crash durability: a commit that returned `true` is lost if the process dies.

The name used is "atomic, serialized commits with snapshot-consistent reads". Relative to the
assignment's sentence "It is snapshot isolation, not serializability": a read-only operation
on one `Snapshot` gets what snapshot isolation gives a reader; read-then-write clients get
less than snapshot isolation (no first-committer-wins).

## 7. C2 prerequisites and the cross-store snapshot

Requirements read (PDF C.2, Appendix A): secondary index on one non-key column of a second
table; `create_index(field_offset, field_len)`; `index_get` (plain and snapshot-scoped), up
to `max_keys` primary keys copied out; exactly one Snapshot at the start of the join, passed
to every `keys()`, `index_get()` and `get()`; outer loop over `keys()`; index-nested-loop
with full inner-row fetch; never block the writer; never rows from two versions.

**The contract problem.** Appendix A.2 hands the benchmark two independently constructed
`KVStore` objects (`trades`, `instruments`), and Appendix A.1 makes `acquire_snapshot()` and
every snapshot-scoped call a member of one `KVStore`. Each store has its own current-version
pointer. A handle from one store pins that store's version; nothing in the API relates the
two objects. C.2 nevertheless asks for exactly one handle for the whole join.

**Can a coherent global snapshot be built without changing the API?** Only by coupling all
`KVStore` objects behind the interface: one hidden shared root, one epoch domain, one writer
order, so that a handle from any store pins all of them. No signature would change, but the
stores would stop being independent (a write to one republishes the shared root; a snapshot
of one holds back reclamation in all; destruction order matters), and it would redesign the
publication path whose safety argument has just been audited. The assignment describes no
such database object. That would be inventing cross-store atomicity; I have not done it and
do not propose to without an instruction.

The other reading — both tables inside one `KVStore` ("a second table you add to the store")
— gives literally one handle, but `create_index` has a single byte range for "each stored
value", the two row types keep the field at different offsets, and the supplied benchmark
passes two objects.

**Minimal compliant interpretation for C2** (decision D-059): one Snapshot per store, both
acquired at the very start of the join; each store's handle passed to every call on that
store.

- Guaranteed: each table is read at one unchanging version for the whole join; no rows from
  two versions of the same table; the writer is never blocked.
- **Unresolved and not claimed:** that the two versions belong to one instant, and the PDF's
  literal "exactly one Snapshot handle". If both tables are written during a join, the pair
  is not an atomic cross-store snapshot. (In the benchmark's shape the Instrument table is
  loaded once and not modified, so the two instants cannot be told apart.)

**Question for the instructor:**

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

Other C2 prerequisites (none blocks starting):

| # | Prerequisite | Note |
|---|---|---|
| Q1 | The index must be part of the `Version` | so that one commit changes data and index in the same root swap, and a snapshot pins both |
| Q2 | Index maintenance on every write path | `put`, `erase`, `Transaction` (overwrite that changes the indexed field; removal; repeated keys in one batch) |
| Q3 | `create_index` on a non-empty store | build and publish in one commit; failure leaves no index |
| Q4 | Values shorter than the indexed byte range | define (not indexed) and test |
| Q5 | `index_get` with more matches than `max_keys`, null/zero arguments, `int` return | define, test; copy-out under the pin |
| Q6 | Failure injection and ownership for index structures | extend `txn_failure_test`; reclamation of old index nodes with their version |
| Q7 | `keys()` gives no total (O-18) | the join must size its buffer; an internal count exists (`size_pinned`) |
| Q8 | Baselines | the supplied generator calls `create_index`/`index_get` on whichever variant it is linked with; decide whether the lock variants get an index or keep reporting "no index" |
| Q9 | A separate join benchmark | the generator's `use_join` path is an index probe, not a join |
| Q10 | Final B→C diff and the combined Part C README | at the end of C2, against `part-b-audited` |

## Changed files and why

| File | Change | Why |
|---|---|---|
| `tests/reclamation_test.cpp` | `test_reclamation_progress`, `test_background_cleaner_resumes_after_release` | progress asserted from accounting (D-058) |
| `docs/transactions.md` | §5 rewritten; §7, §8 adjusted | D-057; pointer to D-059 |
| `docs/part_c_compliance.md`, `reports/milestones/C1.md` | wording corrected (the correction is marked in the report) | D-057 |
| `docs/decisions.md` | D-057, D-058, D-059; O-1 updated | — |
| `CLAUDE.md` | two rules | — |
| `results/C1_audit/` | measured distribution; test output; clean-clone log | evidence |
| `src/`, `include/` | **no change** | — |

## Design decisions and invariants

D-057 (isolation wording), D-058 (progress tested from accounting), D-059 (cross-store
position and question). No protocol operation, ownership rule or public signature changed.

## Build commands and exact exit statuses

`scripts/verify_all.sh` on a clean tree at `314d652`, every mode from a deleted build
directory; cmake 4.2.3, GCC 15.2.0. **Exit 0; 20 of 20 mandatory gates PASS**
(`results/verification/314d65257a81/summary.md`). Informational `perf_stat_usable`: FAIL
(`perf_event_paranoid = 4`). One run; it passed.

| Mode | Configure + build exit | Warnings | Tests | `ctest` exit | Wall time |
|---|---:|---:|---|---:|---:|
| Debug | 0 | 0 | 58/58 | 0 | 78.0 s |
| Release | 0 | 0 | 58/58 | 0 | 57.0 s |
| ASan + UBSan | 0 | 0 | 58/58 | 0 | 168.0 s |
| TSan | 0 | 0 | 58/58 | 0 | 274.0 s |

Clean checkout (`results/C1_audit/clean_clone_build.log`): `git clone`, checkout `314d652`,
release configure exit 0, build exit 0 (0 warnings), `ctest` exit 0, 58/58.

## Tests run (PASS/FAIL/NOT RUN)

All 58 CTest tests PASS in all four modes. Run again by hand in each verified build directory
with full output kept (`results/C1_audit/key_test_output.txt`, 36 runs, all exit 0):

| Requested | Program | Result (each of 4 modes) |
|---|---|---|
| Transaction tests | `transaction_test_rcu`, `transaction_test_rwlock`, `txn_failure_test_rcu`, `txn_failure_test_single` | 0 failed |
| Snapshot tests | `snapshot_test_rcu`, `txn_concurrency_test` | 0 failed |
| Reclamation regression tests | `reclamation_test` (77,234 checks: pin in every mode, progress ×4 batches, background resume, all earlier schedules), `reclamation_stress_test`, `epoch_test` | 0 failed |

Tests added by this audit: 2 (`test_reclamation_progress` for 4 batch sizes,
`test_background_cleaner_resumes_after_release`).

NOT RUN: anything of C2; benchmarks; profiling; machine-checked verification; the C1 mutation
set was not repeated (no source changed).

## Sanitizer / race checks (PASS/FAIL/NOT RUN)

| Check | Result |
|---|---|
| ASan + UBSan, 58 tests | PASS, exit 0, 0 reports |
| TSan, 58 tests | PASS, exit 0, 0 reports |
| Key programs by hand under both | PASS, 0 reports in the captured output |

Sanitizer success means no problem was observed in the executions that ran; the findings of
§2–§4 come from reading the code.

## Benchmarks and measured environment (if applicable)

None. (The distribution measurement of §5 is a test statistic, taken on battery with the
`powersave` governor; it compares two builds in one session and is not a performance result.)

## Raw evidence paths and SHA-256 hashes

| Path | SHA-256 |
|---|---|
| `results/verification/314d65257a81/summary.md` | `fb39d1751faea52773252239be8c8b9b69ad4e70a32cae6fb945ced14bc703e9` |
| `…/logs/debug_ctest.log` | `03ed37d1c026beba37ddfcc466d54b52db2d37615986003647aaf3a8116934f2` |
| `…/logs/release_ctest.log` | `3797c26244f0fdcc5c5c2fee497646017a777e42afd9707fdaba690557f318b9` |
| `…/logs/asan_ubsan_ctest.log` | `4fff17e65798a711d57326c38d9c597c5b19a52a96b7b02344f4684e14ee56b1` |
| `…/logs/tsan_ctest.log` | `016844f67b259aaa871f8146df9d08e0b3df6a299d7da83ba0c75d53f8108e5a` |
| `results/C1_audit/key_test_output.txt` | `af1faa685c29ae3a35b3f1a913800fcf8b43da0173bd1ee6f18332ce36e452ee` |
| `results/C1_audit/clean_clone_build.log` | `56347df9b1b51a381224858f84c4fc1108cf6afdc7ea8333b52dfa95d854c7cc` |
| `results/C1_audit/stress_epoch_advance_distribution.txt` | `0e638119d074d89b363dd400cb6103a5374cafef80773d3dc8d02c5a341998f4` |

## Profiling bottlenecks and next hypotheses

Not applicable.

## Known limitations and source ambiguities

- A read followed by a commit is not isolated; lost updates and write skew are possible.
- No durability.
- A snapshot must not outlive its store; a handle is for one thread at a time; a live
  snapshot stops reclamation of later versions. None is enforced.
- `acquire_snapshot()` takes the registration mutex (contended only among registering
  threads) and scans the slot list.
- Baseline transactions are not tested under concurrent readers.
- `reclamation_stress_test` keeps scheduling-dependent coverage floors (`advance_successes >
  10`, `ids_reused > 50`); the second can fail on a heavily oversubscribed machine. Run the
  suite serially.
- The safety argument is manual, not machine-checked.
- **OPEN / INSTRUCTOR:** one snapshot across two stores (§7).

## Git commit SHA, tag, diff summary

| Commit | Content |
|---|---|
| `1e6af10` | tag `part-c-transactions` (pushed; unchanged) |
| `314d652` | audit: progress tests, isolation wording, D-057…D-059, distribution evidence. **Verified commit.** No change under `src/` or `include/` |
| (next) | this report and the verification evidence |

`git diff part-c-transactions 314d652 -- src include` is empty, so the interim Part B → Part C
diff (`results/diffs/part_b_to_part_c1.diff`) is unchanged and still equals
`git diff part-b-audited part-c-transactions -- src include`. `main` untouched (`a3cccd8`);
nothing merged; no tag moved or created.

## Gate verdict: PASS / FAIL / BLOCKED

**PASS.** Read as committed, the C1 code does what C1 claimed: one root per successful
commit and none for a failed one; staged values owned by the transaction; writers serialized
on all three entry points with no mutex on any read; a snapshot is one pin that is
transferred, never duplicated, and released once; protected versions, chunks and chunk IDs
cannot be reclaimed in any of the three reclamation modes. The threshold change concealed no
regression, and progress is now asserted deterministically from the store's own accounting.
The one over-strong statement in the documents has been removed. 20/20 mandatory gates pass
in four build modes on the audit commit.

Qualifications, none of which is an unresolved safety obligation for C1: the argument is
manual; contracts on snapshot use and shutdown are the caller's; `acquire_snapshot()` uses
the registration mutex; the cross-store question is open and belongs to C2.

**C1 AUDIT PASS — READY FOR C2**

## Next milestone proposed (not started)

C2 (blueprint §8.8), on your approval, with the cross-store interpretation of §7 unless the
instructor answers differently.
