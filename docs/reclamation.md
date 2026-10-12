# Reclamation in the Part B store: design and safety argument

Milestone B2. Code: `src/epoch.{h,cpp}`, `src/versioned_store.{h,cpp}`,
`src/pinned_arena.{h,cpp}`, `src/cow_segment.{h,cpp}`.

This document does two things. Sections 1–4 describe the mechanism. Section 5 is the
correctness argument required before physical reclamation may be enabled; it is written
against the actual operations in the code, which carry labels such as `[P2]` or `[W1]` in
comments at the line where they occur.

**What kind of argument this is.** It is a manual proof sketch over the C++ memory model's
single total order of `seq_cst` operations. It has not been machine-checked. Tests and
sanitizer runs are evidence that the code matches the argument, not a replacement for it;
they are listed separately in `reports/milestones/B2.md`.

---

## 1. The three roles

| Role | Threads | What it does | What it never does |
|---|---|---|---|
| Reader | any number | announces an epoch in its own slot, validates it, loads the root once, reads immutable objects, copies the value out, clears its slot | take a lock, allocate (after its first read), write shared memory other than its own slot, wait for anyone |
| Writer | exactly one | builds replacement segments privately, swaps the root once per logical write, tags and queues the replaced version | wait for a grace period, wait for the cleaner, modify anything published |
| Cleaner | one at a time | in passes: tries to advance the epoch, destroys retired versions whose grace period has passed | write the root, wait for a reader, destroy anything by age |

The cleaner is normally a background thread owned by the store. Tests can instead run passes
by hand (`CleanerMode::Manual`) or have the writer run them after each commit
(`CleanerMode::Inline`); in every case it is the same pass code, and an atomic flag
(`pass_running_`) makes a second concurrent pass return immediately.

## 2. Epoch protocol

Global state: one `std::atomic<uint64_t> epoch_`, starting at 0. Per participant: one
`ReaderSlot`, a cache-line-aligned `std::atomic<uint64_t> state` holding either the epoch it
announced or `kEpochInactive`.

```text
Reader (EpochDomain::pin, then VersionedStore::get):
    loop:
[P1]    e = epoch.load()
[P2]    slot.state.store(e)                 // announce
[P3]    if epoch.load() == e: break         // validate; otherwise announce again
[P4]  v = current.load()                    // the ONE load of the root, after validation
      ... find the entry in v, copy the bytes out ...
[P5]  slot.state.store(INACTIVE)

Writer (VersionedStore::commit, after building everything):
[W1]  old = current.exchange(next)          // publication
[W2]  tag = epoch.load()                    // AFTER the exchange
[W3]  queue_tail->next.store(node{old, tag})// one store hands the record to the cleaner

Cleaner (EpochDomain::try_advance, then VersionedStore::epoch_pass):
[C1]  e = epoch.load()
[C2]  for each slot block (following next pointers), [C3] for each slot:
          s = slot.state.load();  if s != INACTIVE and s != e: return   // no advance
[C4]  epoch.compare_exchange(e, e + 1)
[C5]  oldest = queue_head->next.load()
[C6]  if epoch.load() < oldest.tag + 2: stop                           // not eligible
      take `oldest`; release the version (resumable); count it reclaimed when finished
```

Every operation above is `memory_order_seq_cst`. The only non-`seq_cst` atomics in the Part B
code are statistics counters, the retry counter in a slot, and stores that initialize an
object no other thread can reach yet.

A long-lived **pin** (`VersionedStore::acquire_pin`) is a reader whose `[P5]` is postponed: it
owns a dedicated slot, performs `[P1]–[P4]` once, and keeps the slot announcing that epoch
until `release_pin`. Moving a pin copies a pointer pair; no slot is written. This is the
internal basis for the Part C `Snapshot`.

## 3. Reader slots

- Slots live in blocks of 32. A block is fully initialized (all slots inactive and free)
  before one `seq_cst` store links it after the last block `[R1]`. Blocks are never unlinked,
  moved or freed while the store exists. **There is no limit on their number.**
- A thread gets one slot per store on its first read of that store (registration: a mutex,
  possibly an allocation) and finds it afterwards with one thread-local comparison: the store's
  ID against the ID cached with the slot. No mutex and no allocation on later reads.
- **Resource exhaustion.** The only thing a read can fail to obtain is a new slot block on a
  thread's first read. In that case `get` throws `std::bad_alloc`. It never returns `false`
  for lack of resources, so an existing key is never reported absent, and it never proceeds
  unprotected.
- **Thread exit.** A thread's cached slots are returned when the thread ends. Stores have IDs
  that are never reused, and a process-wide table (mutex-protected, touched only on store
  construction/destruction and thread exit) says which IDs are alive. The exit path returns a
  slot only if its store is still in the table; the store removes itself from the table before
  freeing its slots. A thread that outlives a store therefore never touches it.
- A released slot is inactive and may be handed to another thread or pin.

## 4. Retirement, passes and budgets

**Queue.** A linked FIFO with a dummy head. The writer appends at the tail with `[W3]`; the
cleaner consumes from the head. The node is allocated before `[W1]`, so handing over cannot
fail.

**One reclamation pass** (`VersionedStore::epoch_pass`) is, in this order:

1. one gated attempt to advance the epoch (`try_advance`), only if something is retired;
2. destruction of retired versions that are eligible `[C6]`, oldest first, until
   - `n` versions have been **completely** destroyed, where `n` is the reclaim batch, or
   - the work budget of the pass is spent, or
   - the oldest remaining entry is not eligible.

`void KVStore::set_reclaim_batch(int n)` sets `n` (values ≤ 0 are treated as 1). It takes
effect at the next pass and never blocks a reader or the writer.

**Work budget.** Destroying one version means visiting its 256 directory entries, and for
each segment it was the last owner of, dropping that segment's chunk references and freeing
it. One *work unit* is one directory entry visited or one chunk reference dropped. A pass may
spend `n × W` units, with `W` = 1024 by default (`set_reclaim_work_per_version`, internal).
An ordinary version costs a few hundred units, so normally the version count `n` is what ends
a pass. A version that costs more than the remaining budget is left half-released, with its
position recorded (`Job`: partition index, segment, chunk index), and is resumed by the next
pass. It is counted as reclaimed only when its last reference has been dropped and its memory
freed; merely dequeuing it counts for nothing. So:

- the batch size bounds how many versions one pass can free (the assignment's requirement);
- the work budget bounds how much one pass can do even if a version is unusually expensive;
- neither is a time bound: a pass's duration also depends on the allocator and the scheduler.

**No backlog limit.** If a participant stays pinned, the epoch stops one step past it, nothing
retired from then on becomes eligible, and the queue grows with every write. The writer never
waits for that. Memory is finite, so eventually an allocation fails; the write then returns
`false` with the published state unchanged. No scheme with finite memory can promise
unlimited successful writes under an indefinitely stalled reader.

**Comparator modes** (for Part D.2; not for normal use): `Disabled` retires versions and
never destroys them; `StopTheWorld` has every reader take a shared lock, and the cleaner take
it exclusively and free the whole queue at once.

---

## 5. Safety argument

### 5.0 Conventions and assumptions

- "Before/after" means the order of `seq_cst` operations in their single total order, which
  is consistent with each thread's program order.
- **A1.** One writer thread calls `put`/`erase`/`commit` (the assignment's assumption; not
  enforced).
- **A2.** At most one reclamation pass runs at a time (`pass_running_`,
  `versioned_store.cpp: reclaim_pass`).
- **A3.** A slot is used by one participant at a time, and that participant performs one
  critical section at a time in it (thread-affine slots; a pin's dedicated slot).
- **A4.** Shutdown contract: when the destructor runs, no reader, pin or writer is active.
- **Fact E.** `epoch_` changes only at `[C4]`, by exactly +1, so it never decreases.

### 5.1 A reader cannot acquire an unprotected root

In `VersionedStore::get` and `acquire_pin` the load `[P4]` follows the return of
`EpochDomain::pin`, which returns only after `[P3]` has read the same epoch the slot
announced at `[P2]`. No pointer obtained from the store is dereferenced before `[P4]`. These
are the only two places a non-writer thread loads `current_` during operation.
(`current_version_unprotected()` and `stats()` are documented as requiring a quiet store.)

### 5.2 Bounded overtaking: the collector cannot overlook a pinned participant

**Lemma B.** Let participant *R* have completed `[P3]` reading epoch *e*, and not yet executed
`[P5]`. Then `epoch_ ≤ e + 1`.

*Proof.* Suppose some `[C4]` — call it *G₂* — changes the epoch from *e*+1 to *e*+2 while *R* is
pinned. Let *G₁* be the `[C4]` that changed it from *e* to *e*+1.

1. *R*'s `[P3]` read *e*, so by Fact E it is before *G₁*. `[P2]` is before `[P3]`.
2. The pass that performed *G₂* read *e*+1 at its `[C1]`, so that `[C1]` is after *G₁*, hence
   after *R*'s `[P2]`.
3. That pass then examined slots at `[C3]`, all after its `[C1]`.
4. *R*'s slot was among them: the slot's block was linked at `[R1]` (or is the first block)
   before the slot was handed to *R*, hence before *R*'s `[P2]`, hence before this pass's
   `[C1]`; the traversal `[C2]` loads each `next` pointer after that and so reaches the block;
   blocks are never unlinked.
5. Between its successful `[P3]` and its `[P5]`, *R* does not store to its slot (the loop only
   stores before a successful `[P3]`), and by A3 nobody else does. So the pass's `[C3]` on
   *R*'s slot read *e*.
6. *e* ≠ *e*+1 and the slot is active, so that pass returned without reaching `[C4]`.
   Contradiction. ∎

Note what is and is not claimed. The advance from *e* to *e*+1 **can** happen without the
scan having seen *R* (the scan may read *R*'s slot before `[P2]`, or the block may not have
been linked yet when the traversal passed). That is harmless and is exactly why eligibility
needs two advances. What cannot happen is a second advance.

*The formal-safety supplement* (§4, second bullet) argues that a scan which misses *R* cannot
be followed by a successful advance. As stated that is too strong; the correct statement is
Lemma B, and the supplement's conclusion (no retirement tagged ≥ *e* becomes eligible while
*R* is pinned) follows from it unchanged. Recorded as a deviation in wording, not in protocol.

### 5.3 An old reader prevents premature advancement; a retired version is not destroyed early

**Theorem 1.** If participant *R* obtained version *V* at `[P4]` and has not executed `[P5]`,
*V* is not destroyed, and neither is anything reachable from it.

*Proof.* Let *X* be the `[W1]` that replaced *V*, and *T* the following `[W2]` with value
*r* (*V*'s tag). If there is no such *X*, *V* is still current and is never given to the
cleaner. Otherwise:

1. *R*'s `[P4]` returned *V*, so it is before *X*: after *X*, `current_` holds other versions
   and the writer never stores a retired pointer again.
2. *R*'s `[P3]` (value *e*) is before its `[P4]`, so before *X*, so before *T*. By Fact E,
   *e* ≤ *r*.
3. By Lemma B, while *R* is pinned the epoch is ≤ *e*+1 ≤ *r*+1.
4. The cleaner takes *V* out of the queue only after `[C6]` has read an epoch ≥ *r*+2
   (`versioned_store.cpp`, the only place a queue entry becomes a `Job`). By 3 that read does
   not happen while *R* is pinned.
5. Versions are destroyed only as jobs (`run_job`) or in the destructor (A4). ∎

*After* `[C6]` succeeds for *V*, no participant can still be using it: any that had it has
executed `[P5]` (else contradiction with Theorem 1), and none can newly obtain it (step 1). So
destroying it is safe. Nothing in the eligibility test refers to time, to the number of
commits, or to the queue length.

### 5.4 A shared segment cannot reach zero owners while a retained version references it

`Segment::owners` is changed in exactly three places: `store(1)` when a new segment is
attached to `next`; `fetch_add(1)` when `next` shares a segment of `base`; `fetch_sub(1)` when
a version being released gives the segment up (`run_job`).

**Invariant O1.** `owners` = number of versions that point at the segment and whose directory
entry for it has not yet been processed by `run_job`.

- The `fetch_add` is never an increment from zero. The writer takes the pointer from `base`
  (= `latest_`, the current version). `base` holds a reference, and `base` reaches the cleaner
  only through `[W3]` of *this* commit, which is after the `fetch_add`. So throughout,
  `owners ≥ 1` on account of `base`, regardless of what the cleaner releases concurrently.
- The decrement that reaches zero destroys; `fetch_sub` returns 1 to exactly one caller.
- A version's entries are processed only after it became a job, i.e. after its own `[C6]`.

So if any version that is not yet eligible (in particular any version a participant is
protected in, by Theorem 1) points at a segment, that segment's count is at least 1.

### 5.5 A chunk cannot reach zero references while a published segment or an open commit references it

`PinnedChunk::refs` (see `pinned_arena.h`) counts:

- one per existing segment that lists the chunk (`add_ref` in `commit`, `release` in
  `run_job`);
- one for the writer while the chunk is the writable one (its birth reference);
- one for an open commit on each chunk it created that is not the writable one, and on the
  chunk that was writable when the commit began if the commit moved on from it
  (`commit_refs_` list).

The chunk is destroyed only inside `drop_ref`, by the `fetch_sub` that returns 1. There is no
other path and no place that reads the count and then frees.

- *Published segment.* It holds its reference from before its publication until `run_job`
  destroys it, which by §5.4 is after every version owning it was eligible.
- *Open commit.* A `ValueRef` held by a builder names either (i) a chunk listed by the base
  segment, which is alive because `base` owns that segment; or (ii) the chunk the commit
  appended to, which is the writable chunk (writer's reference), or was writable when the
  commit began and has since been sealed (the reference stays, now on `commit_refs_`), or was
  created by the commit (birth reference, on `commit_refs_` unless it is the writable chunk).
  These references are dropped only in `end_commit`, after the new segments have executed
  `add_ref`, or in `rollback_commit`.
- `add_ref` is never an increment from zero, for the same two reasons.

This closes audit finding F2. The schedule that crashed B1 (commit seals its writable chunk;
the cleaner drops the last segment reference) now leaves `refs == 1`, the commit's reference.
The second audited schedule (`end_commit` and the cleaner both dropping the last references)
is two `fetch_sub` operations; exactly one returns 1.

### 5.6 Chunk-ID reuse cannot redirect an old ValueRef to another allocation

`destroy` (`pinned_arena.cpp`) performs, in order: store `nullptr` to the directory slot; free
the memory; push the ID to the pool. An ID is handed out again only from the pool. So an ID is
reused only after its chunk's `refs` reached zero.

A `ValueRef` naming that ID exists only inside segments (each holding a reference while it
exists) or inside an open commit's builders (covered by §5.5). So when `refs` is zero no
`ValueRef` to that chunk exists in any segment or builder. A reader holds a `ValueRef` only
while protected in a version that owns the segment it came from; that segment exists; hence
`refs > 0`. Therefore no `ValueRef` a reader or the writer can hold names an ID that is being,
or has been, reused. The directory slot a reader loads is non-null for the same reason.

**The ID pool** is a Treiber stack with several pushers (whoever destroys a chunk: writer or
cleaner) and one popper (the writer). A pop reads head *H* and link *L*, then CASes *H* → *L*.
That is wrong only if *H* was popped and re-pushed with a different link in between; only the
writer pops, and it is the thread performing this pop, so that cannot happen. Concurrent
pushes change the head and make the CAS fail, and the pop retries.

### 5.7 Retirement records cannot be lost or consumed twice

- *Not lost.* The node is allocated in phase 3 of `commit`, before `[W1]`. Between `[W1]` and
  `[W3]` the replaced version is referenced by the local `old` and then by the node; nothing
  in between can fail, throw or return. `[W3]` is one store that makes a complete node
  (version and tag already written, `next` null) reachable from the queue.
- *Not consumed twice.* Only the cleaner role reads or writes `queue_head_`, one pass at a
  time (A2). Taking the oldest entry copies its payload into `job_` and makes that node the
  new dummy; its payload is never read again.
- *The writer never touches a freed node.* The writer writes only `queue_tail_->next`. The
  cleaner frees a node only after it has a successor (it frees the old dummy when advancing
  past it). The tail has no successor until `[W3]` gives it one, so the tail is never freed
  under the writer.
- *Order.* Tags are read from a non-decreasing epoch by one thread and appended in that order,
  so the queue is sorted by tag; if the oldest entry is not eligible, no entry is.

### 5.8 Slot registration and thread exit are safe

- A slot handed out by `acquire_slot` is in a linked block before the caller can announce in
  it (§5.2 step 4 depends on this). For a slot in a new block, `[R1]` precedes the return.
- Only registration (under `registration_mutex_`) sets `owned`; release clears it after
  storing `INACTIVE`. A free slot is inactive, so the collector ignores it. If a slot is
  re-issued, the new owner's announcements are simply later critical sections in the same
  slot; Lemma B applies to each.
- Thread exit: `EpochThreadCache::~EpochThreadCache` holds the live-domain mutex while it
  looks up each cached domain ID and releases the slot only if found. `~EpochDomain` removes
  its ID under the same mutex before freeing its blocks. So either the thread returns its slot
  before the domain starts dying, or the domain is already unregistered and the thread does
  not touch it. IDs are never reused, so a stale cache entry never matches a new store, even
  one at the same address.

### 5.9 Cleanup and shutdown cannot double-free

- Versions: each is owned by exactly one of {`current_`/`latest_`, the writer's local `old`,
  a queue node, `job_`}, and moves between them by single-threaded steps or by `[W1]`/`[W3]`.
  It is freed once, at the end of `run_job`.
- Segments and chunks: freed only by the decrement to zero (§5.4, §5.5).
- A half-released version (`job_`) records how far it got; resuming continues from there and
  never re-processes an entry.
- Destructor (`~VersionedStore`): first `stop_cleaner()` (sets a flag and joins the thread),
  so no other thread runs a pass; then `destroy_everything()` finishes the job, releases every
  queued version regardless of epoch (A4: nobody is reading), then the current one, on the
  destructor's thread, using the same `run_job`. Then members are destroyed: the epoch domain
  unregisters and frees its slot blocks; the arena frees the writable chunk.

### 5.10 Dangerous interleavings and why each is prevented

| # | Interleaving | Prevented by | Test |
|---|---|---|---|
| I1 | Reader reads the epoch `[P1]`, is descheduled before announcing; versions are retired and destroyed; reader resumes | It announces a stale epoch, `[P3]` fails, it re-announces and only then loads the root: it never sees the destroyed version | `reclamation_test`: parked before announcing — 2000 of 2000 versions reclaimed while parked, reader returns the latest value, 1 retry |
| I2 | Reader announces `[P2]`, is descheduled before validating | Epoch advances once (allowed), then is blocked by the slot; on resuming, `[P3]` fails and it re-announces | parked after announcing — 0 reclaimed, epoch +1 |
| I3 | Reader validates, is descheduled before loading the root | Lemma B holds the epoch at ≤ *e*+1; whatever root it then loads was current after its validation | parked after validating |
| I4 | Reader holds an old root (or an entry of it) for thousands of commits | Theorem 1 | parked after loading the root / before copying bytes — original bytes returned after 2000 publications, 0 chunks freed, 0 IDs reused |
| I5 | Collector's scan misses a reader that registers in a brand-new block during the scan | Lemma B: at most one advance | cleaner parked mid-scan and after its scan while a reader registers in a new block |
| I6 | Writer swaps the root and is descheduled before tagging/queueing | The old version is owned by the writer's local; the cleaner sees nothing to do; when queued, its tag is ≥ any reader's epoch | writer parked after the root swap / after tagging |
| I7 | Cleaner releases old versions while a commit shares segments with the current one | §5.4 | `versioned_interleaving_test` S6 |
| I8 | Commit appends to the writable chunk, seals it; cleaner drops the chunk's last segment reference (audit F2) | §5.5: the commit's reference | `commit_seals_chunk_while_cleaner_runs`, success and failure variants; arena-level `test_commit_reference_protects_sealed_chunk` |
| I9 | `end_commit` and the cleaner drop the last two references to one chunk | one `fetch_sub` returns 1 | `end_commit_races_with_release` |
| I10 | Chunk freed by the cleaner while the writer allocates one: both touch the ID pool | lock-free pool, single popper | `pinned_arena_test` concurrent test; stress under TSan |
| I11 | A commit fails on allocation while the cleaner is active | rollback releases only the commit's own references | `commit_seals_chunk_while_cleaner_runs(true)`; `versioned_alloc_failure_test` with passes after every commit |
| I12 | A pass ends in the middle of a version | the job is resumable; the version is not counted until finished | `test_cleanup_interrupted_and_resumed` |
| I13 | A thread exits (returning its slot) while the store is being destroyed, or after | live-domain table | `epoch_test::test_thread_outlives_domain` |
| I14 | Store destroyed with a backlog and the cleaner mid-pass | stop and join first, then release on one thread | `repeated_init_and_shutdown` |
| I15 | A long-lived pin is moved to another thread | the slot is not touched by a move | `test_long_lived_pin_moved_between_threads` |

### 5.11 What is not proved

- The argument is not machine-checked and relies on A1–A4; A1 and A4 are the caller's
  obligations and are not enforced.
- No progress or bounded-memory claim is made beyond: if every participant eventually unpins
  and the cleaner keeps running, every retired version is eventually destroyed.
- Correctness under orderings weaker than `seq_cst` is not claimed; none are used on protocol
  operations.
- Reader-side cost is not addressed here (two `seq_cst` stores per `get`); see the milestone
  report for measurements.
