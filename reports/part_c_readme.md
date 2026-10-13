# NOVA-KV — Part C: atomic multi-key writes, snapshots, secondary index and join

This is the README / design note the assignment asks for in Part C ("a short README.md /
design note covering both the isolation-level argument from C.1 and the index/join design
from C.2"). It summarizes; the full notes are `docs/transactions.md` (C.1) and
`docs/secondary_index.md` (C.2). Requirement-by-requirement status:
`docs/part_c_compliance.md`.

Everything below is the `rcu` build of `KVStore` (`nova_kv_rcu`), built on the Part B
mechanism: immutable versions, one atomic root pointer, epoch-based reclamation.

## What was added to Part B

| | |
|---|---|
| `Transaction` | `put()` stages a private copy; `commit()` publishes every staged write as **one** new version with **one** pointer swap |
| `KVStore::Snapshot`, `acquire_snapshot()` | a move-only handle that pins one published version for as long as it lives |
| `get(const Snapshot&, …)`, `keys(const Snapshot&, …)`, `index_get(const Snapshot&, …)` | reads of that pinned version |
| `create_index`, `index_get` | one secondary index per store over a byte range of each value, stored **inside the version** |
| Index-nested-loop join | `benchmarks/join.h`, over the public API |

No public signature differs from Appendix A (`tests/api_conformance_test.cpp`).
Source diff against Part B: `results/diffs/part_b_to_part_c.diff`
(`git diff part-b-audited part-c-final -- src include`).

## C.1 — Atomic, isolated multi-key writes

**Staging.** `Transaction::put` copies the value into the transaction's own write set at once.
Nothing staged is visible to anyone. A put that cannot be staged makes `commit()` return
`false`.

**Commit.** The write set goes through the same path as a single `put`: each touched hash
partition is cloned once, privately; a new `Version` is assembled; `current.exchange(next)`
publishes it. That exchange is the linearization point of the whole transaction. A failed or
abandoned transaction publishes nothing. Several puts to one key: the last one wins. An empty
commit succeeds without publishing. A transaction commits once.

**Atomicity.** Before the exchange no reader can reach any staged write; after it every reader
reaches all of them. There is no intermediate root.

**Isolation of a read.** A single `get` loads the root once and reads only immutable objects.
A `Snapshot` does the same once and keeps the version pinned, so any number of reads through
the one handle see the same state, whatever the writer commits meanwhile.

**Who may write.** `put`, `erase` and `Transaction::commit` are serialized by an internal
writer mutex. Readers never take it. This is serialization, not concurrent writing.

### The isolation level

> **Atomic, serialized commits with snapshot-consistent reads.**

- Commits are serialized and each is published atomically.
- A `Snapshot` gives consistent reads of one version of one store.
- Independent ordinary `get` calls may observe different versions.
- A `Transaction` has no reads. There is no read set and no validation at commit.
- Therefore **a client's reads followed by a commit are not one transaction**, and
  serializing the writers does not make them one.

Relative to the assignment's "it is snapshot isolation, not serializability": a read-only
operation pinned to one snapshot (the join) gets exactly what snapshot isolation gives a
reader. Clients that read and then write get *less* than textbook snapshot isolation, because
first-committer-wins is absent (two clients can both read 5 and both write 6: a lost update).
It is not serializable.

**Write skew.** Two doctors are on call; the rule is that at least one must be. Each reads
"the other is on call" from a snapshot, and each commits "I go off duty". Each transaction is
atomic, each read was consistent, each change is fine alone, they touch different keys, and
together they leave nobody on call. Snapshot isolation does not prevent this (the write sets
do not overlap); serializability would (one must see the other's write). It can happen in this
store exactly so, whether the two requests come from two threads or from one thread working
from two earlier snapshots; preventing it would need read sets and validation, which the
Appendix A interface has no place for.

**Durability: none.** Everything is in memory. If the process dies, an in-flight transaction
was never visible and leaves nothing behind; **a transaction whose `commit()` returned `true`
is lost as well**. There is nothing to recover. (Part E would add a write-ahead log.)

## C.2 — Secondary index and index-nested-loop join

**The index.** `create_index(offset, len)` builds one index per store over value bytes
`[offset, offset + len)`. A value too short to contain the range is not indexed. The index is
64 immutable segments referenced from the `Version`, each holding the (field hash, key) pairs
of one hash partition, sorted, with a small probing table over the distinct hashes. It is a
custom structure.

**Atomic with the data.** The index is part of the version. A commit works out, while applying
its operations to private copies, what each one adds to and removes from the index, builds
replacement index segments, and publishes rows and index in the same single pointer swap.
`create_index` on a populated store backfills privately and publishes the complete index in
one swap. A reader never sees a row whose index entry is missing or stale, a deleted row still
indexed, or half an index.

**Lookup.** `index_get` reads one version (the current one, loaded once; or the snapshot's),
finds the posting list for the probe's hash, and checks each candidate key's field bytes
against that same version's value before returning it. Keys come back in ascending order; if
there are more than `max_keys`, the smallest are returned.

**Reclamation.** Index segments are owner-counted like primary segments and are freed by the
cleaner only when the last version referencing them has passed its grace period. A snapshot
therefore protects its version's index along with its rows and value chunks.

**The join.** For each key from `keys()` on the outer table: fetch the row with the
snapshot-scoped `get`, probe the inner table's index with the snapshot-scoped `index_get`, and
fetch each matching inner row with the snapshot-scoped `get`. It handles any number of matches
per probe, including none.

### Snapshots in the join: one per store — a documented deviation

The assignment says: "Acquire exactly one Snapshot handle … and pass that same handle to every
keys(), index_get(), and get() call the join makes." The assignment's benchmark, however,
passes two independently owned `KVStore` objects (Trades, Instruments), and a `Snapshot` pins
one version of one `KVStore`.

**This implementation acquires one Snapshot from each store at the start of the join (two
handles) and uses each for every call on its own store.**

- Each table is read from one immutable version for the whole join, rows and index alike.
- Each table is therefore internally consistent throughout; the writer is never blocked.
- **The two snapshots are not guaranteed to be one globally atomic point in time.** If both
  tables are being written, a join can pair a newer Trades version with an older Instruments
  version or the reverse.
- **It uses two handles, not the literal single handle of the assignment's wording.** This is
  a deviation from that wording and is recorded as such, not claimed as compliance.

No global cross-store version was introduced and the tables were not merged into one store.

## Limits

- No conflict detection, no serializability, no durability (above).
- A snapshot held for a long time stops the reclamation of everything retired after it.
- One index per store. A write that changes the indexed field rebuilds the index partitions it
  touches; on a large table indexed on a low-cardinality field that is expensive.
- `Snapshot` and the index exist on the `rcu` build only. The lock-based Part A baselines
  implement `Transaction` (one lock hold, with undo) but refuse snapshots and report no index.
- Writers are serialized.

## Tests

| Program | What it establishes |
|---|---|
| `transaction_test_*` | staging, last-write-wins, 1/4/16/100 keys in one publication, empty/abandoned/repeated commit |
| `snapshot_test_*` | state at acquisition, move semantics, wrong store, `keys()`, lifetime in all reclamation modes |
| `txn_failure_test_*` | every allocation of a commit failing in turn: all or nothing |
| `txn_concurrency_test` | none-then-all at the publication point; atomicity stress; serialized writers |
| `index_test_*` | the index against an independent reference model: creation, backfill, duplicates, zero bytes, short values, overwrite/delete/reuse, transactions, truncation, snapshots |
| `index_failure_test` | `create_index` and indexed commits under allocation failure; forced hash collisions |
| `index_concurrency_test` | rows and index change in the same swap; `create_index` invisible until complete; stress |
| `join_test` | the join against two index-free oracles; writes during the join; long-lived snapshots; concurrent writer |

## Build and run

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j
ctest --test-dir build/release --output-on-failure          # run serially

build/release/index_test_rcu
build/release/join_test
build/release/index_concurrency_test
build/release/join_bench --experiment join --threads 8 --cpus 0,2,4,6,1,3,5,7 --writer on --writer-cpu 15
scripts/run_c2_observation.sh                               # preliminary measurements -> results/c2/
scripts/verify_all.sh                                       # every gate, four build modes
```
