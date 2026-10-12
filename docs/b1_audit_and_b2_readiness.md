# Pre-B2 audit: ownership, publication, and readiness for reclamation

Audited code: tag `part-b-publication` (`2d9c02c`), files `src/versioned_store.{h,cpp}`,
`src/cow_segment.{h,cpp}`, `src/pinned_arena.{h,cpp}`, the `rcu` branch of `src/kvstore.cpp`.
Line numbers refer to that commit. Date 2026-10-12.

This is a review of code as written, not a proof. Where a claim rests on an argument rather
than on a test, it says so.

**Summary of findings**

| # | Finding | Kind | Action |
|---|---|---|---|
| F1 | No correctness defect found in B1 as specified (single writer, no cleaner, versions retained). | — | none |
| F2 | The writer's ownership of the writable chunk, and of chunks an open commit has appended to, is expressed as a pointer comparison and a writer-private list, not as a count. A cleaner releasing old versions during a commit can free a chunk the commit still references. **Reproduced.** Unreachable in B1. | Must change before B2 enables a cleaner | R1 |
| F3 | Chunk freeing mutates writer-private state (free-ID stack, arena statistics). Two threads cannot both free chunks as written. | Must change in B2 | R2 |
| F4 | The retained list is a writer-private LIFO; ownership statistics are plain fields. | Must change in B2 | R3, R4 |
| F5 | `owners` shares a cache line with the fields readers load (`capacity`, `slots`); `pins` sits next to the first payload bytes of a chunk. Each publication writes ~255 owner counters, so it invalidates the header line of every shared segment in every reader's cache. | Performance, not correctness | R8 (measure first) |
| F6 | Segment owner counts are safe against a concurrent cleaner as designed (no increment from zero). Demonstrated for the B1 code by a deterministic test. | — | none |
| F7 | Several B2 dependencies are not pinned down by the blueprint (reader-slot lifetime across thread exit and store destruction; slot exhaustion in `get`; how the cleaner is woken; pass cadence). | Needs decisions | §5.3 |

The design can be extended to concurrent cleanup without changing its architecture. F2–F4 are
local changes to how ownership is *represented*, listed in §3.4.

---

## 1. Ownership and lifetime, object by object

Threads in B1: one writer; any number of readers; no cleaner. "Cleaner" below means the B2
thread that will call `release_version`.

### 1.1 Version

| | |
|---|---|
| Allocated by | Writer: `commit` phase 3 (`versioned_store.cpp:233`); the initial empty one in the constructor (`:70`). |
| Owned by | Exactly one owner at any time: the store while it is `current_` (mirrored in writer-private `latest_`); then the writer's local `old` between the exchange and the push (`:287`–`:301`); then the retained list. |
| Acquired | By construction. A version never has two owners and is never shared. |
| Transferred | `current_.exchange` (`:287`) moves ownership of the old version to the local `old`; the push (`:298`–`:300`) moves it to the retained list using a node allocated before the exchange (`:238`). |
| Released | `release_version` (`:308`): only from the destructor (`:83`–`:84`) or the quiescent hatches. |
| Accessed by | Readers: read-only, through the pointer they loaded. Writer: reads `latest_->segments` (`:191`, `:248`); writes a version only before it is published (`:245`–`:269`). |
| May be destroyed when | No reader holds a pointer to it. **B1 guarantees this only by never destroying a version while the store is in use.** B2 must establish it with EBR. |

Unpublished versions: `next` is raw storage until `:245`; a failure before that frees the
storage (`:182`). After `:245` nothing can fail, so a constructed version is always published.

### 1.2 Segment

| | |
|---|---|
| Allocated by | Writer: slot array in `HashSegment::clone_from` / `build_table` (through `SegmentBuilder::begin`, `prepare_put`), header in `SegmentBuilder::prepare_finish` (`cow_segment.cpp:107`). |
| Owned by | Before `finish()`: the `SegmentBuilder` (its destructor frees everything, `cow_segment.cpp:28`–`31`). After: jointly by every Version whose directory points at it; `owners` counts them. |
| Acquired | New segment: `owners.store(1)` (`versioned_store.cpp:263`) on behalf of `next`. Shared segment: `owners.fetch_add(1)` (`:275`) on behalf of `next`. Both in phase 4, after the last fallible step and before the exchange. |
| Transferred | Never individually; it follows the versions that own it. |
| Released | `owners.fetch_sub(1)` per owning version in `release_version` (`:313`); the decrement that takes it from 1 to 0 drops the segment's chunk pins (`:314`) and destroys it (`:317`). |
| Accessed by | Readers: read-only (`capacity`, `slots`, the slots). Writer: reads the base version's segments to clone them; never writes a published segment. `owners` is written by the writer (and, in B2, the cleaner); readers never read it. |
| May be destroyed when | `owners == 0`, i.e. every version that pointed at it has been released, which in turn requires each of those versions to be unreachable by readers. |

### 1.3 Chunk (`PinnedChunk`)

| | |
|---|---|
| Allocated by | Writer: `PinnedArena::new_chunk` (`pinned_arena.cpp:82`), from `append` during a commit. |
| Owned by | (a) every existing Segment that lists it in `pins[]`, one pin each (`pins` counter); (b) the writer, while it is the writable chunk, expressed as `c == current_`; (c) the open commit, for chunks it created, expressed as membership of the `commit_new_` list. |
| Acquired | (a) `arena_.pin` (`versioned_store.cpp:262`, `pinned_arena.cpp:198`) for each chunk of a new segment, before the segment is published. (b), (c) implicitly, by pointer. |
| Transferred | Writer ownership ends when another chunk becomes writable (`pinned_arena.cpp:147`–`148`) or at `end_commit`. |
| Released | Pin: `unpin` (`:202`) when a listing segment is destroyed. Freed in `free_chunk` (`:110`) from three places: last unpin of a non-writable chunk (`:206`); `end_commit` for chunks the commit sealed or created that nobody pinned (`:164`–`:171`); `rollback_commit` for chunks the failed commit created (`:182`–`:186`). |
| Accessed by | Readers: the payload bytes of ranges named by published `ValueRef`s, via the directory. Writer: appends at `used` and beyond; `used`, `sealed`, `link` are writer-only. `pins`: writer (and cleaner in B2). |
| May be destroyed when | No segment pins it, it is not writable, and no open commit references it. |

### 1.4 Chunk directory

| | |
|---|---|
| Top level | `blocks_[1024]` of `std::atomic<std::atomic<PinnedChunk*>*>`, inline in the arena; exists for the arena's lifetime. |
| Block | Allocated by the writer on first use (`pinned_arena.cpp:57`), fully initialized, then installed with one `seq_cst` store (`:66`). Never replaced, never freed before the arena's destructor. |
| Slot | `std::atomic<PinnedChunk*>`. Set when a chunk is created (`:104`), before any version naming that chunk is published. Nulled first thing in `free_chunk` (`:111`); only afterwards is the ID pushed on the free stack (`:112`). |
| Accessed by | Readers load block then slot (`pinned_arena.h:94`–`97`), only for chunk IDs taken from a published segment. Writer loads/stores. |
| Chunk IDs | Issued fresh (`next_fresh_id_`) or popped from the writer-private free stack (`:78`). Capacity of the stack is reserved when an ID is first issued (`:68`–`:76`), so returning an ID never allocates. |

### 1.5 Retained list and its nodes

| | |
|---|---|
| Node allocated by | Writer, phase 3 (`versioned_store.cpp:238`), before the exchange. |
| List | `retired_`: writer-private, singly linked, newest first (`:298`–`:300`). |
| Released | Only in `release_retained_quiescent` (`:326`). |

### 1.6 Builders and unpublished state

`SegmentBuilder` and `BuilderSet` are stack objects of one `commit` call. They own every
allocation they make until `finish()` hands a segment over; their destructors free the rest.
They touch no shared counter. This is what makes a failed commit leave no ownership to undo.

---

## 2. Publication and memory ordering

### 2.1 The atomic operations, all of them

Every one is `memory_order_seq_cst` except initializations of objects no other thread can
yet reach (marked "private").

| Site | Operation | Purpose |
|---|---|---|
| `versioned_store.cpp:116` | `current_.load` | **The reader's single load of the root.** |
| `versioned_store.cpp:287` | `current_.exchange(next)` | **Publication; the only write to the root after construction.** |
| `versioned_store.cpp:78` | `current_.store(v)` | Constructor: installs the initial empty version (private). |
| `versioned_store.cpp:263` | `owners.store(1)` | New segment owned by `next` (private until `:287`). |
| `versioned_store.cpp:275` | `owners.fetch_add(1)` | Shared segment gains `next` as an owner. |
| `versioned_store.cpp:313` | `owners.fetch_sub(1)` | A released version gives up a segment. |
| `pinned_arena.cpp:198` | `pins.fetch_add(1)` | A new segment pins a chunk. |
| `pinned_arena.cpp:205` | `pins.fetch_sub(1)` | A destroyed segment unpins a chunk. |
| `pinned_arena.cpp:165`, `:171` | `pins.load` | `end_commit`: is this chunk referenced? |
| `pinned_arena.cpp:66` | `blocks_[i].store(block)` | Install a directory block. |
| `pinned_arena.cpp:104` | `slot.store(chunk)` | Publish a chunk in the directory. |
| `pinned_arena.cpp:111` | `slot.store(nullptr)` | Withdraw a freed chunk from the directory. |
| `pinned_arena.h:94`–`97` | `blocks_[i].load`, `slot.load` | Reader resolves a chunk ID. |
| `cow_segment.cpp:116`, `pinned_arena.cpp:98`, `:24` | relaxed stores | Initialization of private objects. |

No compare-exchange, no relaxed or acquire/release ordering on any shared path.

### 2.2 Linearization points

- **Successful `put`, and successful `erase` of a present key:** the `exchange` at
  `versioned_store.cpp:287`. Before it, every reader's load returns the old version; after it,
  every later load returns the new one. One exchange per successful call.
- **`erase` of an absent key:** returns at `:136` having read only writer-owned state; it
  linearizes at any point during the call (there is one writer, so the store's key set does
  not change during it).
- **`get`:** the load at `:116`.
- **Failed `put`/`erase`:** no linearization point; the store is unchanged.

### 2.3 Why a reader cannot see a partially initialized root

Within the writer thread, every write that initializes `next` (`:245`–`:269`), the new
segments (`cow_segment.cpp:113`–`126`), their slot arrays, the directory entries for any new
chunks (`pinned_arena.cpp:104`) and the value bytes (`pinned_arena.cpp:152`) is *sequenced
before* the exchange at `:287`. A reader obtains `next` only from a `seq_cst` load that
observes that exchange, which gives *happens-before* from all of those writes to everything
the reader then does. None of those locations is written again. (A release store with an
acquire load would already suffice; `seq_cst` is used throughout per blueprint §0.)

Evidence beyond the argument: interleaving S3 (writer parked at `:284`, new version complete
but unpublished: 800 observations, all old); mutation B3 (root stored early) is detected.

### 2.4 Why a published segment cannot be mutated

There is no code path that writes through a `Segment*` taken from a version, other than the
`owners` counter. `SegmentBuilder::begin` copies the slot array (`HashSegment::clone_from`)
and the pin list into private memory; all edits go to the copy. Value bytes: `append` writes
only at offsets ≥ `used`, and every published `ValueRef` ends at or below the `used` of the
moment it was created; `used` is lowered only by `rollback_commit`, to its value at the start
of the failed commit, which is still ≥ every published reference. (The Part A arena's rewind
of the writable chunk does not exist here.)

Evidence: byte-for-byte comparison of retained versions' slot arrays after thousands of later
writes; `cow_segment_test::test_base_is_untouched`; mutations B4, B11.

### 2.5 Why a failed update cannot become partially visible

All fallible operations (`:188`–`:239`) precede the first write to any shared location (`:262`).
On failure (`:180`–`:186`): the arena is rolled back (chunks created by the commit freed, write
offset restored), `next` and `node` storage are freed, the builders' destructors free the rest.
`current_` has not been written and no `owners`/`pins` counter has been changed.

Two things do persist after a failed commit, by design, and are invisible to readers: a
directory *block* and a larger free-ID stack, if the failed commit was the one that needed
them (`pinned_arena.cpp:56`–`76`).

Evidence: `versioned_alloc_failure_test`, 1566 injected failures; mutations B6, B7.

### 2.6 Why an old root remains safe while a reader uses it

**In B1: because it is never destroyed while the store is in use.** That is the entire
argument. A reader that loaded version `V` reaches only `V`, segments `V` owns, and chunks
those segments pin; `V` stays on the retained list until the destructor.

This is *not* a concurrent-reclamation guarantee. B1 has no mechanism by which the store could
know that a reader has finished with `V`.

### 2.7 Why publication and retirement cannot lose ownership

The node is allocated at `:238`, before the exchange. From `:287` to `:300` the old version is
referenced by the local `old`; nothing between those lines can fail or return. If the writer
thread is suspended in that window the old version is simply still held by it. Evidence:
interleaving S4 (writer parked at `:290`).

### 2.8 What comes from retention and what B2 must establish

| Guarantee | B1 source | B2 must provide |
|---|---|---|
| A reader never dereferences a freed Version / Segment / chunk | Nothing is freed during operation | EBR: announce + validate before the root load; destroy a version only after two gated epoch advances beyond its retirement epoch |
| A chunk ID is never reused under a reader | Follows from the line above plus pins | Same structural argument, valid once the line above is |
| A version handed over for retirement is not lost | Pre-allocated node, writer-private list | Same node, but a queue the cleaner can consume without losing or duplicating entries |
| Owner and pin counts are exact | One thread updates them | Atomic updates from two threads, with no increment from zero (§3) |
| Memory is bounded | **Not provided** | Cleaner with batch and work budgets; still unbounded if a reader parks forever |

---

## 3. Writer versus cleaner

### 3.1 Can a segment reach zero owners while the writer prepares a version that references it?

No, by this argument:

1. The writer shares a segment `S` with `next` only by copying a pointer out of `base`
   (`:248`), where `base == latest_` is the current version.
2. `base` owns one reference to `S` for as long as `base` exists.
3. `base` is not handed to the retained list (and so cannot reach a cleaner) until phase 6 of
   the commit that replaces it (`:298`), which is after `next` has taken its own reference to
   `S` (`:275`).
4. A cleaner decrements only on behalf of versions it has been handed.

So from the moment the writer reads `base->segments[p]` until `next` holds its own reference,
`S.owners ≥ 1` on account of `base`, whatever the cleaner releases. The increment at `:275` is
never an increment from zero, and the decrement that reaches zero can only come after both
`base` and every version sharing `S` have been released. With `fetch_add`/`fetch_sub` on an
atomic this needs no further synchronization.

Demonstration on the B1 code, `versioned_interleaving_test` schedule S6 (added by this audit):
the writer is parked at `:284` with a prepared version that shares 36 segments with the current
one; another thread then releases all 120 older versions. Result: none of the current
version's segments has zero owners (36 have two, the 4 replaced ones have one), the current
version reads back in full, and after the writer resumes the new version does too; `validate()`
is clean. Passing in debug, release, ASan+UBSan and TSan.

### 3.2 Can a chunk reach zero pins while the writer prepares a version that references it?

Three cases for a chunk `C` referenced by a segment the writer is building:

1. **`C` holds entries carried over from the base segment.** The base segment lists `C` and is
   owned by `base` (§3.1), so `C.pins ≥ 1` throughout. Safe.
2. **`C` was created by this commit.** No existing segment lists it, so no cleaner unpins it;
   its ID was either fresh or recycled after its previous holder was fully freed. Safe from
   `unpin`. (It is, however, only protected from `end_commit`/`rollback` by being on the
   writer's `commit_new_` list — fine for one thread.)
3. **`C` was the writable chunk when the commit began, and the commit appended to it.** Here
   the only things keeping `C` alive are pins held by *other* segments, possibly only retired
   ones, and the test `c != current_` at `pinned_arena.cpp:206`. If the commit goes on to seal
   `C` (a later value needs a new chunk), `C` is no longer `current_`; if a cleaner then
   releases the last retired segment listing `C`, `unpin` takes `pins` to zero and **frees
   `C` while the commit holds an unpublished `ValueRef` into it.**

**Case 3 is finding F2 and was reproduced** (`results/B1/audit/`): with one extra pause point
between the appends and phase 3, releasing the retained versions frees the chunk
(`chunks_freed=1`) and the resumed commit crashes in `PinnedArena::pin` at
`pinned_arena.cpp:198` (null directory slot), under both a plain and an ASan build. It cannot
occur in B1, where nothing releases versions during a commit, and no B1 test or mode reaches
it. It would occur in B2 as soon as a cleaner runs concurrently.

A second, related race exists in `end_commit` (`:164`–`:171`): it *loads* `pins`, and frees if
zero. With a cleaner, the last `unpin` (1 → 0, cleaner frees) and that load (sees 0, writer
frees) can both free the same chunk.

### 3.3 Other writer/cleaner races in the code as written

| Shared state | Writer use | Cleaner use (via `release_version` → `unpin` → `free_chunk`) | Problem |
|---|---|---|---|
| `current_` of the arena (`PinnedArena::current_`) | written in `append`, `rollback_commit` | read at `:206` | data race; and the wrong notion of ownership (F2) |
| Free-ID stack `free_ids_`, `free_count_` | pop in `new_chunk` (`:78`, `:91`), push in `free_chunk` | push in `free_chunk` (`:112`) | data race; lost or duplicated IDs (F3) |
| `PinnedArena::stats_` | everywhere | `free_chunk`, `unpin` | data race (statistics only) |
| `VersionedStore::stats_` | `commit` | `release_version` (`:315`–`:320`) | data race (statistics only), F4 |
| `retired_` list | push (`:298`–`:300`) | would need to pop | not a concurrent structure; newest-first order is the wrong order for epoch-ordered release (F4) |
| `owners`, `pins` | `fetch_add` | `fetch_sub` | **safe as is** (atomic; §3.1, §3.2 cases 1–2) |
| Directory slots | `store` | `store(nullptr)` | safe: distinct slots, or same slot only after the chunk was fully freed and its ID returned |
| Segment and Version memory | read (base only) | freed | safe: the writer reads only `base`, which the cleaner does not hold |

Readers versus cleaner is the EBR problem itself and is not listed here.

### 3.4 Required changes before a cleaner thread may run

None changes the architecture; all are local to how ownership is represented.

- **R1 (F2). Writer ownership of chunks becomes a real pin.** The writable chunk carries one
  pin owned by the writer for as long as it is writable, and every chunk an open commit
  appended to carries one "commit pin" until the commit has either pinned it through its new
  segments or rolled back. A chunk is then freed by exactly one rule: the `fetch_sub` that
  takes `pins` to zero frees it, whichever thread performs it. The `c != current_` test and
  the load-then-free in `end_commit` disappear. The scratch experiment becomes a permanent
  test (with its pause point) and must pass.
- **R2 (F3). Freed chunk IDs travel through a channel.** The cleaner must not touch the
  writer's free stack. A single-producer/single-consumer hand-off from cleaner to writer
  (blueprint §6.4 lists this event), sized for the directory so a push never allocates or
  fails; the writer drains it when it needs an ID. Order of operations stays: null the slot,
  free the memory, then offer the ID.
- **R3 (F4). The retained list becomes the retirement queue.** FIFO, single producer (writer)
  and single consumer (cleaner), linked through the nodes that are already pre-allocated, each
  carrying its retirement epoch. FIFO matters: retirement epochs are non-decreasing in queue
  order, so the cleaner can stop at the first ineligible entry.
- **R4. Counters split by thread or made atomic.** Writer-side and cleaner-side statistics in
  separate, cache-line-separated blocks; anything read across threads is atomic.
- **R5. `release_version` becomes resumable.** One version is up to 256 owner decrements, each
  possibly a segment destruction with up to several hundred unpins. The cleaner needs a
  cursor (version, next partition, next pin) and work budgets, and must count a version as
  reclaimed only when the cursor reaches the end (blueprint §15.3).
- **R6. The quiescent hatches are removed or confined** to "store has no cleaner and the
  caller is the only thread", checked, not promised. `current_version()` / `get_at()` /
  `validate()` as test interfaces become unsafe once versions are freed and must run under an
  epoch pin or with the cleaner stopped.
- **R7. Shutdown order.** Stop and join the cleaner, then release everything from the
  destructor's thread.
- **R8 (F5, performance; measure before acting). Separate `owners` from reader-visible
  fields**, e.g. by aligning the counter to its own cache line or moving it to a control
  block, and likewise keep `pins` off the first payload line of a chunk. Today every
  publication writes to the header line of ~255 shared segments, which readers then have to
  re-fetch. This is a plausible cause of the B1 observation that one reader beside an
  unthrottled writer achieves 8.66 M gets/s against 17.67 M alone; **that link is a
  hypothesis and has not been measured.**

---

## 4. B2 readiness, item by item

Requirements reviewed: assignment PDF p. 4 (Part B) and p. 9 (Snapshot pin semantics);
blueprint §5, §8.6, §15.2–15.4; formal-safety supplement §2–§4 and H01, H07, H09, H12, H13,
H14; runbook §4 and §10.

| Item | What the specifications require | State of the code | Work in B2 |
|---|---|---|---|
| Reader-slot registration | Fixed, stable registry; slots initialized INACTIVE before use; no add/remove during operation; no unregistered read; one active read per slot; explicit exhaustion behaviour (supplement §2.2–2.3, H01, H09) | None | Slot array inside the store, cache-line aligned; per-(thread, store) slot acquired once and cached. **Open points D-a, D-b below.** |
| Epoch announcement and validation | `e = epoch.load; slot.store(e); if epoch.load == e break;` then exactly one `current` load; no dereference before validation; all `seq_cst` (supplement §3) | The marked line in `get` (`versioned_store.cpp:108`–`116`) | Guard object around the load in `get`; pause points at each step |
| Epoch advancement | Cleaner scans every slot; advances by one with a CAS only if every active slot announces the scanned epoch; never an unconditional increment; never waits for a reader (supplement §2.7) | None | Scanner + CAS; pause points before/after scan and CAS |
| Retirement queue ordering | Retirement epoch sampled **after** the exchange; descriptor reserved before publication; never lost between exchange and enqueue (supplement §2.6, H12) | Node reserved before publication; hand-over cannot fail; no epoch, LIFO list | R3; sample the epoch right after `:287` |
| Cleanup eligibility | `epoch ≥ r + 2` and actually received by the cleaner, plus structural ownership (supplement §2.8) | None | Eligibility test on the queue head only (FIFO) |
| Incremental destruction | No hidden unbounded cascade in one pass; a version counts as reclaimed only when all its ownership work is done (supplement §2.10, H13) | `release_version` is one unbounded call | R5 |
| `set_reclaim_batch(n)` | Public setter; at most `n` retired versions freed per pass; `n ≤ 0` clamps to 1 (blueprint §2.2); read atomically; pass never blocks reader or writer (PDF p. 4) | Inert | Atomic `int`; cleaner reads it per pass. **Open point D-d (what a "pass" is).** |
| Chunk-directory publication | Slot published before any version naming the chunk; block lifetime stable | Done (§1.4) | Unchanged |
| Chunk-ID reuse | Only after the last structural pin is gone and the old roots are safe to destroy (supplement §2.9, H07) | Correct for one thread; F2, F3 with two | R1, R2; the ABA test of H07 with a parked reader |
| Explicit `Snapshot` lifetime | Dedicated slot from the same registry, active from acquisition to destruction, unchanged by moves (blueprint §4.5, §15.2; PDF p. 9) | Not implemented (Part C) | Reserve snapshot slots in the registry now so C1 needs no registry change. A live snapshot stops epoch advancement past `e + 1`, so **all** reclamation waits for it, not just its own version's |
| Cleaner shutdown | Join after all external threads have stopped; then release the whole ownership graph; leak checks (blueprint §15.4) | Destructor releases everything on the caller's thread | R7 |
| Long-lived reader | Blocks reclamation, not readers or publication; must be tested across thousands of commits (supplement §4) | B1 analogue tested (S1, S2: reader parked across 3051 publications) | Same schedules with reclamation enabled: nothing retired at or after the reader's epoch may be freed |
| Backlog and memory growth | Monitor queue length and retained bytes; document that a stalled reader or slow cleaner grows memory without bound; fail commits cleanly on exhaustion (blueprint §5.4, §15.4) | Retained count and bytes are tracked; every version is backlog today | Backlog counters readable by the harness; decide the behaviour at a memory limit (**D-e**) |

### 4.1 Proposed order of work for B2 (for approval, not started)

1. R1–R4 and R6–R7 with the cleaner still absent: ownership representation that is correct for
   two threads, verified by the existing tests plus the F2 experiment as a permanent test.
2. Epoch slots, reader guard, retirement epochs, FIFO queue — **with physical freeing still
   disabled** (the cleaner computes eligibility and counts, frees nothing).
3. The proof gate: line-by-line mapping of the code to the supplement's assumptions 1–10, and
   the eight deterministic schedules of its §4 on pause points.
4. Only then enable freeing; R5 (resumable release, budgets), `set_reclaim_batch`.
5. No-reclaim and stop-the-world comparator modes for Part D; torn-read stress with
   reclamation; measurements; Part B README and the A→B diff.

---

## 5. Correctness dependencies the blueprint does not settle

### 5.1 Found in the code (covered by R1–R7 above)

F2 in particular: the blueprint names a "writer allocation owner" and "no pending unpublished
references" as conditions for freeing a chunk (§3.4) but does not say they must be visible to
the cleaner as part of the count. As implemented they are not.

### 5.2 Consequences of the design worth stating before B2

- **EBR is coarse.** Any pinned participant, reader or Snapshot, holds back every retired
  version once the epoch cannot advance twice, not only the version it uses. A long join under
  one Snapshot (Part C) means backlog for its whole duration.
- **Each `get` will perform two `seq_cst` stores** (announce, clear) to its own slot. On x86
  these are locked instructions. Expect a measurable cost against the B1 read numbers; it has
  to be measured, and the slots must not share cache lines.
- **Cross-thread frees.** Segments and chunks are allocated by the writer and will be freed by
  the cleaner. glibc handles this but it is a known source of allocator contention; reader
  tail latency under a busy cleaner is exactly what D.2 measures.
- **The allocation-failure hook is not thread-safe** (`src/alloc.cpp`); failure-injection
  tests must keep the cleaner stopped or the hook must be made thread-aware.

### 5.3 Decisions needed (not specified anywhere I could find)

- **D-a. Slot lifetime across thread exit and store destruction.** A per-(thread, store)
  cached slot must be given back when the thread exits, and a thread's cache must not touch a
  store that has since been destroyed. Proposal: each store has a never-reused 64-bit ID; a
  thread caches (store ID → slot); a thread-exit hook releases slots only for stores still
  registered in a small global table (mutex-protected, off the read path).
- **D-b. Slot exhaustion in `get`.** `get` returns only `bool`. Returning `false` when no slot
  is free would report an existing key as missing. Options: (1) size the registry generously
  (say 256 reader + 256 snapshot slots) and **abort** on exhaustion, treating it as a
  configuration error; (2) fall back to a mutex-guarded shared slot, which makes that reader
  wait — against the assignment's "reads never wait" but only in the overflow case;
  (3) return `false`. I recommend (1).
- **D-c. How the cleaner is woken.** The writer must not block. Options: the cleaner polls
  with a short sleep or yield when idle; or the writer signals it (a wake-up is a system call
  on the write path when the cleaner is asleep). I recommend polling, with the idle interval
  as an internal constant, and measuring the backlog it produces.
- **D-d. What one "reclamation pass" is.** The assignment bounds what "each call" may free by
  the batch size but does not say how often passes run. Proposal: the cleaner runs passes back
  to back while eligible work exists and idles otherwise; one pass = one epoch-advance attempt
  plus at most `n` versions fully destroyed, within the work budgets. Then the batch size
  bounds the length of one uninterrupted stretch of cleaner work, which is what D.2 varies.
- **D-e. Behaviour at a memory limit.** With a stalled reader the backlog is unbounded.
  Options: no limit (allocation eventually fails and `put` returns `false`), or a configurable
  backlog cap at which `put` fails fast. The blueprint says to "report pressure/failure
  behavior" without choosing.

---

## 6. Verification run for this audit

See `reports/milestones/B1_audit.md` for the commands, exit statuses and evidence paths. No
source file of the B1 implementation was changed by the audit; one test schedule (S6) was
added to `tests/versioned_interleaving_test.cpp`.
