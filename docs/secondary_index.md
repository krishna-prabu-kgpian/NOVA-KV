# Part C.2 design note — secondary index and index-nested-loop join

Milestone C2. Code: `src/secondary_index.{h,cpp}` (index segments), `src/versioned_store.{h,cpp}`
(`Version`, the commit path, `create_index`, `index_get`, reclamation), `src/kvstore.cpp` (public
`create_index`, both `index_get` overloads), `benchmarks/join.h` (the join).

Builds on `docs/versioning.md` (versions, publication, ownership), `docs/reclamation.md`
(epoch-based reclamation) and `docs/transactions.md` (transactions, snapshots, isolation).
Everything here is the `rcu` build; the lock-based baselines have no index (§8).

---

## 1. What the index is

One index per store, over one fixed byte range `[field_offset, field_offset + field_len)` of
every stored value (Appendix A: "a secondary index over a byte range within each stored
value"). It maps the bytes found in that range to the primary keys of the values that contain
them.

- The store knows nothing about record types; the field is bytes. Comparison is byte for
  byte: zero bytes are ordinary bytes, and bytes outside the range never matter.
- **A value shorter than `field_offset + field_len` is not in the index** (this includes empty
  values). Nothing is ever read beyond a value's length. Such a value is still stored and
  readable by key; `index_get` just never returns it. If it is later overwritten by a value
  that is long enough, it enters the index; and the reverse.
- Any number of keys may share a field value (the index is a multimap).

## 2. Where it lives: inside the version

```text
current ──atomic──> Version { segments[256]     primary directory
                              index_offset, index_len
                              isegs[64] }        secondary-index directory
                                  └──> IndexSegment (immutable, shared between versions)
```

A `Version` is one complete state of the store **including its index**. There is no separate
index root, no second pointer to swap, and no moment at which one exists without the other.

An `IndexSegment` is one partition of the index (partition = low 6 bits of the field hash) and
is a single allocation, never modified once published:

```text
header | buckets[bucket_capacity] | pairs[pair_count]

pairs    every (field hash, primary key) of the partition, sorted by (hash, key);
         the keys of one field value are contiguous and ascending: its posting list
buckets  open-addressed, linearly probed table over the DISTINCT hashes; a bucket names
         the range of pairs with its hash; load bound 0.70 as for the primary segments
```

A segment contains hashes and keys only: no field bytes and no pointer into the store. That
has two consequences. It owns nothing else, so freeing it is one deallocation. And the hash
alone never decides a match: see §5.

It is a custom structure; no library container is involved.

## 3. Writes: data and index in one publication

A commit (a `put`, an `erase`, or a whole `Transaction`; they are the same code,
`VersionedStore::commit_locked`) does, with the store's writer mutex held:

1. group the operations by primary partition, one private builder per touched partition;
2. apply the operations in order to the private copies; **for each operation, look at the
   key's indexed field before and after it, in the private copy, and record what changes in
   the index** as deltas: *remove (old field, key)* and/or *add (new field, key)*;
3. allocate the primary segment headers, the new `Version`, the retirement record; then sort
   the deltas and, **for each index partition they touch, build the replacement index segment**
   by merging the published segment's pairs with the deltas (one linear merge; the last delta
   for a pair decides whether it is present);
4. *nothing below can fail* — assemble the new version: replaced primary segments, replaced
   index segments, everything else shared with the current version (owner counts +1);
5. **`current.exchange(next)`**;
6. hand the replaced version to the retirement queue.

Step 5 is the only write to `current`, and `next` already contains both the new rows and the
index that matches them. Hence:

- a reader that loads the root before step 5 gets the old rows with the old index, and one
  that loads it afterwards gets the new rows with the new index. There is no third state;
- a transaction of any size changes rows and index entries in the same single swap;
- because step 2 looks at the private copy, several writes to one key in one transaction are
  followed correctly (a → b → c leaves exactly one entry, for c);
- an overwrite that leaves the indexed bytes unchanged produces no delta and does not touch
  the index at all;
- every allocation (the delta array, the merge buffers, the new index segments) happens before
  step 4. On failure the commit frees what it built and returns `false`; no owner count has
  been changed and `current` has not been written. The index segments it built were never
  reachable.

There is no separate index-publication phase and no index update outside a commit.

## 4. `create_index`: backfill, then one publication

`bool create_index(size_t field_offset, size_t field_len)`, with the writer mutex held:

1. reject `field_len == 0` and any range that cannot fit in a value (`field_offset + field_len`
   above the 1 GiB value limit, including arithmetic overflow);
2. if the store already has an index: return `true` if it is over the same range (nothing is
   published), `false` otherwise. **One index per store**, as the assignment says ("you don't
   need to support an index on more than one column");
3. scan the current version's values and collect one (hash, key) pair for every value long
   enough to contain the field (the backfill); sort; build all index segments privately;
4. build a new `Version` that shares **every** primary segment with the current one and points
   at the new index segments; one `current.exchange`.

So the index appears complete, for all the data, in one publication, and the primary data is
not copied. While it is being built nothing of it is reachable: `index_get` keeps returning 0.
A reader or snapshot that loaded the root before the swap has a version without an index and
keeps it. If any allocation fails, everything built is freed, `false` is returned and no index
exists. Being a writer operation, it is serialized with `put`/`erase`/`commit`.

| Situation | Result |
|---|---|
| Empty store | `true`; later writes are indexed |
| Populated store | `true`; existing values backfilled |
| `field_len == 0`; range beyond the value limit; overflow | `false`, nothing published |
| Called again, same range | `true`, nothing published |
| Called again, different range | `false`, nothing published |
| Allocation failure | `false`, no index, store unchanged |
| Value shorter than the range | stored, not indexed |

## 5. `index_get`: one version, and a byte-for-byte check

```cpp
int index_get(const void* field_value, size_t field_len, long* keys_out, int max_keys) const;
int index_get(const Snapshot& snap, const void* field_value, size_t field_len,
              long* keys_out, int max_keys) const;
```

- The plain overload does what `get` does: announce an epoch, validate, **one** load of the
  root, read, clear the announcement. No lock. The snapshot overload uses the version the
  handle is pinned to and never looks at the current one.
- Lookup: hash the probe; find the bucket in the version's index segment for that hash; walk
  its posting list. **For each candidate key, find the key's value in the primary data of the
  same version and compare the field bytes with the probe.** Only matching keys are returned.
  This is what makes two different field values with the same hash harmless, and it means
  that even a hypothetically stale entry could not produce a wrong answer. (The test suite
  cuts the hash down to 0, 1, 3 and 7 bits to exercise it.)
- Returned keys are in **ascending key order**, each at most once. At most `max_keys` are
  written, and if there are more matches the **smallest `max_keys` keys** are returned
  (deterministic truncation). The return value is the number written.
- Returns 0 and writes nothing if: the store has no index; `field_len` is not the index's
  field length; `field_value` or `keys_out` is null; `max_keys <= 0`; or (snapshot overload)
  the handle is moved-from or belongs to another store.
- Only keys are returned (Appendix A: "keep the same copy-out convention for index_get's
  matches"); no pointer into the store escapes.

Cost: one hash of the probe, one bucket probe, and per candidate one primary lookup and one
`memcmp`. It is not a scan: a lookup touches one index bucket and the values of its posting
list only.

## 6. Ownership and reclamation

Exactly the scheme of the primary segments (`docs/versioning.md` §5), applied to one more
kind of object.

- `IndexSegment::owners` = number of versions whose index directory points at the segment.
  A new segment gets `owners = 1` when it is attached to the version being built; a segment
  shared with the current version gets `fetch_add(1)` (never an increment from zero: the
  current version holds a reference and can only be retired by this same commit, later).
- A version's references are dropped only by the cleaner, in `run_job`, after the version has
  passed the epoch test `[C6]` (`docs/reclamation.md`). The decrement that reaches zero frees
  the segment. Releasing a version is resumable in the middle of its index directory, with
  the same work budget.
- **Published index segments are never modified.** A commit builds replacements.
- An index segment references no chunk and no other segment, so there is nothing further to
  release when it is freed, and chunk-ID reuse cannot affect it.

**What a Snapshot protects.** A snapshot pins a version, so by Theorem 1 of
`docs/reclamation.md` that version is not destroyed while the handle lives, and therefore:
its primary segments (owner counts ≥ 1), its index segments (owner counts ≥ 1), and the value
chunks its primary segments list (chunk references ≥ 1), whose IDs can consequently not be
reused. A snapshot-scoped `index_get` reads the pinned version's index segment and then that
same version's values; none of the three can have been reclaimed.

`VersionedStore::validate()` (used throughout the tests) recomputes, for **every version that
exists** (the current one and every retained one), the set of pairs the primary data implies
and requires the version's index to hold exactly that set; it also checks each index segment's
structure and every owner count.

## 7. The index-nested-loop join

`benchmarks/join.h`, written against the public API only.

```cpp
JoinSnapshots snaps = acquire_join_snapshots(trades, instruments);   // the only acquisition
index_nested_loop_join(trades, instruments, snaps, spec, buffers, emit);
```

For the outer table (Trades) and the inner table (Instruments, indexed on `symbol`):

1. `trades.keys(snaps.outer, …)` enumerates the outer keys (growing the buffer and asking
   again if it came back full: `keys` reports how many it wrote, not how many there are);
2. for each key, `trades.get(snaps.outer, key, …)` fetches the complete Trade record;
3. its `symbol` field is the probe;
4. `instruments.index_get(snaps.inner, symbol, …)` returns the matching Instrument keys (again
   growing the buffer if it came back full, so that a probe with many matches is not
   truncated);
5. for each match, `instruments.get(snaps.inner, key, …)` fetches the complete Instrument
   record;
6. `emit(trade_key, trade_row, instrument_key, instrument_row)`.

An outer record may match any number of inner records, including none. Records whose stored
size is not that of the record type are skipped and counted. No call in the join reads a
store's current version, and no further snapshot is acquired.

### 7.1 Snapshots: one per store (decision D-060, "Option A")

The two tables are two independently owned `KVStore` objects, as in the assignment's
benchmark (`workload_run(KVStore& trades, KVStore* instruments, …)`), and a `Snapshot` pins
one version of one store. The join therefore holds **two** handles, both acquired at its very
start and kept until it ends.

Guaranteed:

- the Trades table is read from one immutable version for the whole join;
- the Instruments table, **rows and index**, is read from one immutable version for the whole
  join;
- so each table is internally consistent throughout: no row from two versions of the same
  table, no index entry that disagrees with the row it leads to;
- the writer keeps publishing new versions of either table and is never blocked.

**Not guaranteed:**

- that the two versions belong to one instant. A commit to either store can be published
  between the two acquisitions. If both tables are written while joins run, the pair of
  snapshots is not an atomic cross-store snapshot. (In `join_test`'s concurrent run, 265 of
  400 joins saw the two tables at different generations; each table was of a single
  generation in all 400.)
- **This is two handles, not the "exactly one Snapshot handle" of the assignment's Part C.2
  text. It is a documented deviation from that wording, not a way of satisfying it.**

In the benchmark's shape the Instrument table is loaded once and not modified during the
join, in which case the two instants cannot be told apart by any result. Nothing here
provides or claims a cross-store transaction or a global version.

## 8. What is not there

- **Lock-based baselines:** no index. `create_index` returns `false`; `index_get` returns 0.
  The index is defined as part of a published version and those stores have no versions. The
  supplied generator's `use_join` path therefore finds nothing on them (it ignores
  `create_index`'s result).
- **The supplied generator's probe is not a join** and may miss on this store for a reason of
  its own: `make_trade` leaves the 8th byte of `Trade::symbol` uninitialized (`Trade t;`, then
  `snprintf` writes 7 bytes), while `load_instruments` zero-initializes the Instrument. The
  probe passes 8 bytes and the index compares 8 bytes, so whether it matches depends on what
  the stack held. The comparison is byte for byte by requirement; the generator is not
  modified. The repository's own join benchmark zero-fills its symbols.
- One index per store; no index removal; no range or prefix queries; no index on a field of
  variable position.

## 9. Costs to know about

- **A write that changes the indexed field rebuilds the index segments it touches**, i.e. all
  pairs of one or two of the 64 index partitions. For a small or high-cardinality index that
  is little. For a large table indexed on a low-cardinality field it is a lot: with 100,000
  Trades indexed on 50 symbols a partition holds whole posting lists of 2,000 keys, and one
  symbol change copied about 114 KB of index (measured: `results/c2/`). The assignment's
  indexed table, Instruments, has 50 rows.
- An overwrite that does not change the field costs the index nothing.
- Every commit to an indexed store takes one more owner reference per non-empty index
  partition (up to 64), and the directory copy grows by 512 bytes.
- `index_get` does one primary lookup per candidate key.
- A long-lived snapshot holds back the reclamation of index segments exactly as it does of
  primary segments.
