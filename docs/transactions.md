# Part C.1 design note — transactions, snapshots and what isolation they give

Milestone C1. Code: `src/kvstore.cpp` (public `Transaction`, `Snapshot`, `acquire_snapshot`,
snapshot-scoped `get`, `keys`), `src/staged_writes.{h,cpp}` (a transaction's private write
set), `src/versioned_store.{h,cpp}` (`commit`, `Pin`, `keys_pinned`, the writer mutex, the
stop-the-world comparator), `src/epoch.h` (the reader slot a snapshot owns).

Builds on `docs/versioning.md` (versions, publication, ownership) and `docs/reclamation.md`
(epoch-based reclamation and its safety argument). The secondary index and the join are Part
C.2 and are **not** implemented yet: `create_index` returns `false`, `index_get` finds nothing.

---

## 1. Transaction

```cpp
Transaction t(store);
t.put(k1, v1, len1);      // staged: copied, invisible
t.put(k2, v2, len2);
bool ok = t.commit();     // all of it, in one publication, or none of it
```

### 1.1 Staging

`Transaction::put` copies the caller's bytes immediately into the transaction's own write set
(`nova::StagedWrites`: an array of `{key, offset, length}` and one byte buffer). The caller's
buffer is never referenced again and may be changed or freed at once. Nothing in the write
set is reachable from the store: no reader, snapshot or other transaction can see it.

`put` returns `void` (Appendix A), so it cannot report a problem. Three things can go wrong
while staging: memory for the copy cannot be allocated; `value` is null with a non-zero
length; the length exceeds the value-size limit (1 GiB). In each case the transaction is
marked failed, its write set is released, and **`commit()` returns `false`**. None of its
writes is published, including the valid ones staged before or after the bad one.

### 1.2 Commit

`commit()` turns the write set into one array of operations and hands it to the same engine
function a single `put` uses (`VersionedStore::commit`, `docs/versioning.md` §3):

1. validate; group the operations by hash partition, **one private builder per touched
   partition** (each partition is cloned exactly once, however many of its keys change);
2. apply the operations in staging order to the private copies, appending the value bytes to
   the arena beyond every published reference;
3. allocate the segment headers, the new `Version`, the retirement record;
4. *nothing below can fail* — assemble the new version, take ownership counts;
5. `current.exchange(next)` — **the linearization point of the whole transaction**;
6. hand the replaced version to the retirement queue.

So a transaction of any size is one new immutable root and one atomic pointer swap, "built
the same way as a single-key write in Part B" (PDF C.1). Before step 5 no reader can reach
any of it; after step 5 every reader that loads the root reaches all of it. There is no
intermediate root and no per-key publication.

Measured in `transaction_test` (rcu build): 1, 4, 16 and 100 keys are each 1 publication,
with 1, 4, 15 and 83 segments cloned for 1, 4, 15 and 83 partitions touched; 100 keys that
all fall into one partition clone 1 segment.

### 1.3 Semantics that the assignment leaves open, as decided

| Situation | Behaviour | Why |
|---|---|---|
| Several `put`s to one key | The **last one wins**; the earlier values are never visible | Staging order is the only order a caller can express |
| `commit()` with nothing staged | Returns `true`; **nothing is published** | The supplied generator ends with a `commit()` on a transaction that may be empty |
| `commit()` a second time | Returns `false`; nothing is published | A transaction is one atomic write; repeating it must not publish again |
| `put()` after `commit()` | Ignored | The transaction is finished; the generator constructs a new one per batch |
| A `put` that cannot be staged | `commit()` returns `false`, nothing published | §1.1 |
| Allocation failure during `commit()` | Returns `false`, nothing published, store unchanged | Every allocation precedes step 4 |
| Destroyed without `commit()` | Staged writes are discarded and their memory freed | PDF C.1 |
| No `erase` in a transaction | Not available | Appendix A's `Transaction` has only `put` |

A `Transaction` is not copyable or movable (Appendix A) and is used by one thread.

### 1.4 Failure handling

`txn_failure_test` makes each allocation between constructing a `Transaction` and the end of
`commit()` fail in turn: the transaction's own state, the staged arrays and their growth, the
operation array, the private segment copies and their growth, the chunk lists, value chunks,
the chunk-directory block, segment headers, the version, the retirement record. After each:
the store holds exactly what it held before, the root has not been replaced, ownership counts
validate, a snapshot taken beforehand still returns the original data, the same transaction
then commits, and no object is left over (366 failure points on the rcu build; 108 on the
`single` build).

### 1.5 On the lock-based baselines

The Part A stores (`single`, `global_lock`, `rwlock`) also implement `Transaction`: the
supplied workload uses it against whichever store it is linked with. There the staged writes
are applied **under one exclusive hold of the store lock**, so a concurrent reader sees none
or all of them; if a step fails, the writes already applied are undone before the lock is
released (values that were replaced are kept allocated until the whole batch has succeeded).
That is atomic and all-or-nothing, but it is *not* a pointer swap, and readers wait for the
lock for the duration of the commit. It exists so that the baselines can run the same
workload, not as an implementation of Part C.

## 2. Who may write: the writer contract

The assignment assumes a single writer thread. The engine's writer state (the writable value
chunk, the tail of the retirement queue, the chunk-ID pool's pop side, counters) is not
shareable, so two commits running at once would corrupt it.

**Decision.** `put`, `erase` and `Transaction::commit` take one internal **writer mutex** for
their whole duration (`VersionedStore::writer_mutex_`). Consequently:

- calls from several threads are *safe*: they are executed one at a time, each as one atomic
  publication, in the order they acquire the mutex;
- this is serialization, **not concurrent writing**: writers never run in parallel and there
  is no throughput to gain from more writer threads;
- **no reader ever takes this mutex.** `get`, `acquire_snapshot`, snapshot-scoped `get` and
  `keys` are exactly as lock-free as before; an uncontended mutex acquisition was added to a
  write path that already copies a segment;
- staging (`Transaction::put`) takes no lock at all: a write set belongs to one thread.

Tested by `txn_concurrency_test` (four threads issuing `put`, `erase` and `Transaction::commit`
at once, about 14,000 publications, each atomic; also run under TSan). Removing the
mutex is caught (mutation C19).

## 3. Snapshot

```cpp
KVStore::Snapshot snap = store.acquire_snapshot();   // pins the current version
store.get(snap, key, buf, len);                      // always that version
size_t n = store.keys(snap, keys, max);              // its keys
// ~Snapshot releases the pin
```

### 3.1 What a snapshot is

One **long-lived reclamation pin**: a reader slot of its own in the epoch domain, which keeps
announcing the epoch validated at acquisition, plus the `Version*` loaded at that moment.

- `acquire_snapshot()` takes a dedicated slot, then performs the reader protocol once:
  announce `[P2]`, validate `[P3]`, and only then load the root, exactly once `[P4]`
  (`docs/reclamation.md` §2). The version is stored in the slot's owner-private word.
- The public handle is `{const KVStore* owner_; void* pin_}` (unchanged since A0). `pin_` is
  the slot's address; the slot remembers the version. No further state, no allocation per
  snapshot beyond the slot, and slots are recycled.
- `get(snap, …)` and `keys(snap, …)` use the remembered version. They do **not** load the
  root, do not announce anything and do not release the pin.
- The destructor clears the announcement `[P5]` and returns the slot.

### 3.2 Lifetime under epoch-based reclamation

A snapshot is, to the reclamation scheme, a reader that has not finished: "exactly as if a
reader were still inside a call against that version" (Appendix A.1). Theorem 1 of
`docs/reclamation.md` therefore applies unchanged: while the slot announces epoch *e*, the
epoch cannot advance past *e*+1, so no version retired at or after *e* becomes eligible, and
the pinned version is one of those or is still current. Everything reachable from it stays:
its segments (owner counts), the value chunks they list (chunk references), and hence the
chunk IDs, which are recycled only after a chunk's last reference is gone. A `ValueRef` read
through a snapshot can therefore never resolve to another allocation.

The cost is the one stated for any stalled reader: **while a snapshot lives, nothing retired
since its acquisition is reclaimed**, in any mode. A snapshot held for a long time under a
busy writer makes memory grow without bound until it is released.

This holds in all three reclamation modes:

| Mode | With a live snapshot | After it is destroyed |
|---|---|---|
| Epoch-based (the store) | versions retired since acquisition are kept | reclaimed in bounded passes |
| Stop-the-world (Part D.2 comparator) | the sweep frees only what the epoch rule allows, so the same versions are kept | the next sweep frees the whole list |
| Disabled (Part D.2 comparator) | nothing is ever freed | nothing is freed until the store is destroyed |

The stop-the-world comparator used to free the entire retire list once it had excluded all
`get` calls by a lock; a pin holds no lock between calls and was not consulted (B2 audit,
D-049). It now performs two gated epoch advances under the freeze and frees every entry the
eligibility rule `[C6]` allows, with no batch limit and no work budget: still an unbounded
sweep that stops all readers, but one that cannot free what a snapshot protects. In that mode
snapshot-scoped calls take the same shared lock as `get` for their own duration, so the
comparator really does freeze every read.

`snapshot_test` holds one snapshot across 3000 publications of 16 KB values in each mode:
0 versions reclaimed, 0 chunks freed, 0 chunk IDs reused while it lives; about 1495 chunks
freed and IDs reused after it is destroyed (not in the disabled mode).

### 3.3 Move-only ownership

A move copies the two words of the handle and clears the source. The slot is not written, so
the protection is neither dropped (there is no moment without it) nor duplicated. Move
assignment first releases the pin the target held. A moved-from handle is inert: its
destructor does nothing, and `get`/`keys` with it return `false`/`0`. Each acquisition is
released exactly once (`snapshot_test` checks the number of owned slots after every step).

A handle may be moved to another thread and used or destroyed there; the acquiring thread may
have exited. One thread at a time may use a given handle (it is not synchronized, like any
move-only object).

### 3.4 Owner validation and invalid handles

| Handle | `get(snap, …)` | `keys(snap, …)` |
|---|---|---|
| Acquired from this store, live | the pinned version | the pinned version |
| Moved-from | `false`, buffer untouched | `0`, buffer untouched |
| Acquired from **another** store | `false`, buffer untouched | `0`, buffer untouched |

The check is `snap.owner_ == this` before anything is dereferenced. A snapshot must be
destroyed before its store (shutdown contract, blueprint §2.2); a handle that outlives its
store is a use-after-free in the caller and is not detected.

### 3.5 When a snapshot cannot be provided

- **No reader slot can be allocated:** `acquire_snapshot()` throws `std::bad_alloc`. It never
  returns a handle that does not protect its version.
- **Lock-based baselines:** `acquire_snapshot()` throws `std::logic_error`. Those stores have
  one mutable state and no versions; the only way to hold a view still would be to hold the
  lock for the snapshot's lifetime, which blocks the writer (and deadlocks with a mutex as
  soon as the holder calls the store again). An inert handle whose reads report "absent" would
  be a silent wrong answer, so acquisition fails loudly. **The intended build for anything
  using `Snapshot` is `nova_kv_rcu`.**

### 3.6 `keys(const Snapshot&, long* keys_out, size_t max_keys)`

Writes up to `max_keys` of the keys present in the snapshot's version and returns the number
written (Appendix A.1: "enumerates up to max_keys primary keys present in the table under
snap; returns the number written to keys_out").

- Exactly the keys of that version: later inserts are not included, keys erased later are
  still included, a key updated later appears once.
- No duplicates: a key occupies one slot of one segment of a version; tombstones and empty
  slots are skipped.
- **Order: unspecified** (partition by partition, slot by slot, i.e. the hash layout), but
  the same on every call for the same snapshot.
- If the version has more than `max_keys` keys, the first `max_keys` in that order are
  written; the API offers no way to ask for the total or to continue, so a caller that needs
  every key must pass a large enough buffer.
- `max_keys == 0` or `keys_out == nullptr` returns 0 and writes nothing. Nothing is ever
  written beyond `keys_out[max_keys - 1]`.
- Cost: one pass over the version's slot arrays; no lock, no allocation.

## 4. Three guarantees that must not be confused

| | Guarantee | Holds for | Tested by |
|---|---|---|---|
| A | **Point-read linearizability.** One `get(key)` returns one complete value that was the key's value at a moment during the call | every ordinary `get` | no torn value in any test; `concurrent_lock_test_rcu` |
| B | **Atomic multi-key publication.** All writes of one transaction become visible in one step | any observer that looks at one version | `test_none_then_all` for 1/4/16/100 keys; stress with 0 partial snapshots |
| C | **Stable snapshot.** Every read through one `Snapshot` sees the same version | reads through that handle | 0 unstable snapshots; 3000 commits under one handle |

**Two ordinary `get` calls are two versions.** Reading keys *x* and *y* of one transaction
with two plain `get`s may return the old *x* and the new *y*: each call is linearizable on
its own (A), and nothing more is promised. `txn_concurrency_test` counts this happening
(in one run, 1891 times in 190,135 pairs with 4-key transactions) and does not treat it as an
error. Seeing a transaction's writes together (B) requires one `Snapshot` used for every read
(C). That is what the Part C.2 join will do.

## 5. The isolation level, precisely

### What is implemented

1. **Commits are serialized and each is published atomically.** Every successful `put`,
   `erase` and `Transaction::commit` is one root swap, and the writer mutex orders them. The
   store's history is a sequence of versions V₀, V₁, V₂, …, each differing from its
   predecessor by exactly one commit.
2. **A `Snapshot` gives consistent reads of one version of one store:** the Vᵢ that was
   current at acquisition. It contains every earlier commit completely and nothing of any
   later one, for as long as the handle lives.
3. **An ordinary `get` reads the latest version at one instant.** Independent ordinary gets
   are independent instants and may observe different versions (§4).

### What is not implemented

- **No read operations inside a `Transaction`.** The Appendix A class has `put` and `commit`
  only. A transaction is a *write set*; whatever the client read in order to decide what to
  write was read outside it, earlier, through `get` or a `Snapshot`.
- **No read set and no validation.** The store does not know what a client read, and
  `commit()` checks nothing: not that the values read are still current (serializable
  validation), and not even that the keys being written were left alone since the client
  looked (snapshot isolation's first-committer-wins).
- **No crash durability** (§6).

### What follows, and what does not

**A sequence of client-side reads followed by a `commit()` is not one transaction.** It is
some reads, then, later, an atomic write. Between the two, other commits may be published,
and the store will neither notice nor refuse. The commit is atomic; the read-then-write
sequence as a whole has no isolation guarantee at all.

In particular **serializing the writers does not make client transactions serializable.**
The writer mutex orders the *commits*. It does not extend back over the reads that led to
them. Two clients (or one client with two pending decisions) can each read, each decide, and
then have their commits serialized one after the other, the second one based on a state the
first has already changed. Nor does a single writer thread change this in general: one
thread that takes a `Snapshot`, computes for a while, and commits something derived from the
snapshot after having committed something else in between has made a stale decision all by
itself. The store offers no mechanism that would detect or prevent any of these; avoiding
them is entirely up to how the application structures its reads and writes.

So:

| Property | Provided? |
|---|---|
| A transaction's writes become visible all at once | **Yes** |
| A failed or abandoned transaction leaves no trace | **Yes** |
| Commits take effect in one total order | **Yes** |
| Reads through one `Snapshot` see one committed state of that store | **Yes** |
| Nothing uncommitted is ever readable (no dirty reads) | **Yes** |
| Two ordinary `get` calls see the same version | No |
| A read followed by a commit behaves as one isolated transaction | **No** |
| Lost updates prevented (snapshot isolation's write–write rule) | **No** |
| Write skew prevented (serializability) | **No** |
| A committed transaction survives a crash | **No** |

**The accurate name is: atomic, serialized commits with snapshot-consistent reads.**

How this relates to the assignment's sentence "It is snapshot isolation, not
serializability": for a **read-only** multi-step operation (the join of Part C.2, pinned to
one `Snapshot`) the store gives exactly what snapshot isolation gives a reader: one
consistent, unchanging committed state. For clients that **read and then write**, it gives
*less* than textbook snapshot isolation, because first-committer-wins is absent: two clients
can both read *x* = 5 and both commit *x* = 6, and one increment is lost. Snapshot isolation
would abort one of them. And like snapshot isolation, it does not prevent write skew. It is
not serializable in any configuration that involves decisions based on earlier reads.

### Why multi-key atomicity is not serializability

Atomicity says a transaction's writes appear together. Serializability says the *outcome* of
running transactions concurrently equals the outcome of running them one after another in
some order, **including the reads their decisions were based on**. A system can publish every
transaction atomically and still let two transactions each decide on the basis of a state
that the other is about to invalidate.

### Write skew

The classic example. Rule: at least one doctor must be on call. Alice and Bob are both on
call. Each wants to go off duty.

```text
Alice: snapshot S_A  -> reads {alice: on, bob: on}   "Bob is on call, I may leave"
Bob:   snapshot S_B  -> reads {alice: on, bob: on}   "Alice is on call, I may leave"
Alice: Transaction { put(alice, off) }  commit -> true
Bob:   Transaction { put(bob,   off) }  commit -> true
Result: {alice: off, bob: off}          nobody is on call
```

Each transaction is atomic. Each read a consistent snapshot. Each change is fine on its own.
They wrote **different keys**, so even a snapshot-isolation system with first-committer-wins
would let both commit: that is why snapshot isolation does not prevent write skew. In any
serial order the second doctor would have seen the first one off duty and stayed; a
serializable system must abort one of them (by tracking read sets, or by locking what was
read).

In this store the anomaly can occur exactly as drawn. The two commits are serialized by the
writer mutex and each is atomic; that changes nothing, because each decision was taken from a
snapshot before either commit. It makes no difference whether Alice's and Bob's requests are
issued by two threads or by one thread working from two snapshots it took earlier. Preventing
it would need read sets and validation at commit (or locking what was read), which the
Appendix A interface has no place for.

Other anomalies possible for read-then-write sequences: **lost update** (above; prevented by
snapshot isolation, not prevented here) and stale decisions in general. Not possible: dirty
reads (nothing uncommitted is reachable), non-repeatable reads *within one snapshot*, and
partially visible transactions.

## 6. Durability: none

Everything is in memory. If the process dies:

- a transaction that was being staged, or whose `commit()` had not yet swapped the root, is
  gone and was never visible to anyone: no cleanup is needed and no partial state exists;
- a transaction whose `commit()` **returned `true`** is **also gone**. The acknowledgement
  meant "visible to every reader of this process", not "will survive". A client that reported
  success onward on the strength of it has reported something that no longer exists;
- there is nothing to recover and nothing to repair after a restart: the store starts empty.
  In particular there is no torn or half-applied state to detect, because there is no
  persistent state at all.

What is given up, then, is the "D" of ACID entirely, and with it any guarantee across a crash
about which transactions happened. Part E (optional) would add a write-ahead log so that a
commit is acknowledged only after it is durable, without touching the read path.

## 7. Two stores, one join (unresolved, for the instructor)

A `Snapshot` pins one version of **one** `KVStore`. Two stores (`trades`, `instruments`) have
two independent roots, so two snapshots are two instants, not one database-wide instant. The
PDF's wording for C.2 ("acquire exactly one Snapshot handle … pass that same handle to every
keys(), index_get(), and get() call") against a two-table shape with two `KVStore` objects is
an open question for the instructor (O-1). Nothing in C1 claims a cross-store snapshot, and
C1 does not invent one. The analysis, the interpretation C2 will use unless told otherwise
(one snapshot per store, labelled as per-store consistency) and the question to put to the
instructor are in `docs/decisions.md` D-059.

## 8. Limits

- No read set, no validation, no conflict detection: a read followed by a commit is not an
  isolated transaction; lost updates and write skew are possible (§5).
- No durability (§6).
- A long-lived snapshot stops reclamation of everything retired after it (§3.2).
- `keys()` cannot report the total or continue (§3.6).
- A snapshot must not outlive its store; a handle is not synchronized.
- `acquire_snapshot()` registers a slot under a mutex with a linear scan of the slot list:
  negligible for a join that takes one snapshot, measurable if snapshots are taken at very
  high rates. Not measured yet (Part D.3).
- Each commit copies the touched segments and the 2 KiB directory and updates up to 255 owner
  counts, whatever its size; large transactions that touch most partitions copy most of the
  index (the copy-on-write cost Part D.3 asks about).
- Writers are serialized; there is no parallel write throughput.
