# NOVA-KV Part B — RCU reads and bounded-pause reclamation

This is the README the assignment asks for with Part B (PDF p. 4): what was built, which
reclamation scheme was chosen and why, the tunable parameter, and how to check it. The full
design and the safety argument are in `docs/reclamation.md` (reclamation) and
`docs/versioning.md` (publication and ownership). The requirement-by-requirement checklist is
`docs/part_b_compliance.md`; the source diff against Part A is
`results/diffs/part_a_to_part_b.diff`.

## What changed from Part A

Part A's store mutates one hash index in place under a lock. Part B adds, inside the same
`KVStore` class and source files, a build (`rcu`) with no lock at all:

- **Writer builds off to the side.** A write clones only the hash segments it touches (the
  index has 256 of them), shares the rest, and assembles a new immutable *version*.
- **One atomic pointer swap publishes it.** `current.exchange(next)`; that instruction is the
  moment the write becomes visible, whole.
- **A reader takes exactly one atomic load** of that pointer per operation and then reads
  only immutable memory, so it sees one consistent snapshot whatever the writer does. It takes
  no lock and never waits for the writer or for reclamation. `get` copies the value into the
  caller's buffer before it leaves its protected section.
- **One writer thread** is assumed, as the assignment allows.

The three Part A lock variants are still built from the same tree, unchanged. Note that a
comparison between them and `rcu` compares two storage/update strategies (in-place versus
copy-on-write), not only two synchronization schemes.

## Reclamation: epoch-based, and why

A replaced version cannot be freed at once: a reader may be inside it. We use **epoch-based
reclamation (EBR)** with per-reader announcements and *gated* epoch advancement (the
"classical three-epoch" scheme).

- There is one global epoch counter. Before loading the current-version pointer a reader
  writes the epoch it sees into its own slot, checks the epoch has not moved, and only then
  loads the pointer. On leaving it clears the slot.
- When the writer replaces a version it tags the old one with the epoch read *after* the swap
  and appends it to a retirement queue.
- A cleaner thread tries to advance the epoch. It may do so only if every active reader slot
  shows the current epoch; one reader still in an older epoch stops it. It never waits for
  that reader; it tries again on its next pass.
- A retired version is freed only when the epoch is **at least two advances beyond its tag**.
  A reader that could still hold the version stops the epoch after one. Age and queue length
  play no part.

**Why EBR rather than hazard pointers.** A `get` here follows a chain: version → segment →
slot → chunk. With hazard pointers each hop needs its own published pointer and a
re-validation (which also sits awkwardly with the assignment's "exactly one atomic load of
the current pointer"), and the snapshot handle of Part C would have to protect an entire
version's worth of objects. With EBR a reader announces once and everything reachable from the
version it loads is protected for as long as it stays announced; a long-lived snapshot is just
a reader that has not left yet. The price is EBR's known weakness, which we accept and
document: **one stalled reader delays all reclamation**, not only that of the version it
holds, so memory can grow while it is stalled.

Freeing a version releases what it alone owned: versions own references to segments, and each
segment owns a reference to every value chunk it points into, so a chunk outlives every
version that can reach it and its ID is recycled only after that.

## The tunable parameter: `void KVStore::set_reclaim_batch(int n)`

Declared next to `put`/`get`/`erase` in `kvstore.h`, defined in `kvstore.cpp`; the value is a
private member of the store (an atomic integer, default 64; `n <= 0` is treated as 1).

**A reclamation pass** is: one attempt to advance the epoch, then freeing retired versions
that have become eligible, oldest first. **One pass completely frees at most `n` versions.**
The cleaner runs passes back to back while there is work and sleeps briefly when there is
none.

What makes a pass bounded, and what it never does:

- it frees at most `n` versions, and additionally spends at most `n × 1024` *work units*
  (one unit = one directory entry visited or one chunk reference dropped); a version that
  would exceed the remaining budget is left half-released and resumed by the next pass, and it
  counts as freed only when completely gone;
- it never walks the whole retire list (it stops at the first version that is not yet
  eligible, or when a limit is reached);
- it never blocks a reader: readers take no lock and are not paused; a reader in the way only
  makes the epoch advance fail;
- it never blocks the writer: the writer hands a version over with a single store into a
  queue node it allocated before publishing, and does not wait for any grace period.

A larger `n` lets each pass do more (fewer passes, more work per uninterrupted stretch of
cleaner activity); a smaller `n` spreads the same work over more, shorter passes. Measured
effect on this machine: `reports/milestones/B2.md`. The batch size is not a time bound: how
long a pass takes also depends on the allocator and the scheduler.

Two extra modes exist only as reference points for Part D.2 and are not reachable through the
public API: reclamation disabled (versions are retired and never freed), and a naive
stop-the-world sweep (readers take a shared lock; the cleaner takes it exclusively and frees
the whole list).

## Limits you should know about

- A reader or snapshot that stays pinned stops reclamation; the backlog and memory then grow
  with every write. The writer is never made to wait. With finite memory, an allocation
  eventually fails and that `put` returns `false` with the store unchanged.
- A write costs far more than under a lock: it copies the touched segment (about 24 KiB at
  the benchmark's size) and a 2 KiB directory to change one value.
- A thread's first `get` on a store registers a reader slot (a mutex and possibly an
  allocation); if that allocation fails `get` throws `std::bad_alloc` rather than report the
  key as missing. Later `get`s by that thread take no lock and allocate nothing.
- Exactly one writer thread. The store must outlive all threads using it, and those threads
  must stop before it is destroyed.
- `Transaction`, `Snapshot`, the secondary index and the join are Part C and not implemented.
- The safety argument is a manual proof sketch, not a machine-checked proof.

## Correctness test required by the assignment

"Concurrent readers hammering `get` while the writer hammers `put`, showing no reader ever
sees a torn or partially-updated row":

| Test | What it does |
|---|---|
| `concurrent_lock_test_rcu` | 1 writer, 4 readers on the public API; every record carries its generation, the complement of it, its key, its length and a generation-derived fill, so a torn or mixed record is self-evident; the final state must equal the writer's log |
| `reclamation_stress_test` | the same idea with reclamation at batch sizes 1, 8, 64 and 1024, variable-length records including ones larger than a chunk, multi-key commits, long-lived pins and short-lived reader threads; also checks that a read never returns something older than a write that had already completed |
| `versioned_interleaving_test`, `reclamation_test` | deterministic schedules: threads parked at exact steps of read, write and reclamation |

All run under AddressSanitizer+UBSan and ThreadSanitizer in `scripts/verify_all.sh`.

## Build and run

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

build/release/concurrent_lock_test_rcu           # the no-torn-read test
build/release/reclamation_stress_test            # with reclamation, four batch sizes
build/release/reclamation_test                   # deterministic schedules, batch limits
build/release/rcu 8 5                            # supplied workload generator, RCU store

# readers + an active writer, choosing the batch size
build/release/concurrent_bench_rcu --readers 8 --duration 3 \
    --reader-cpus 0,2,4,6,1,3,5,7 --writer on --writer-cpu 15 --reclaim-batch 64

scripts/verify_all.sh                            # all gates, four build modes
git diff part-a-final part-b-final -- src include   # the Part A -> Part B source diff
```
