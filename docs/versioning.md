# Part B design note — versions, publication and ownership

Status: written at **B1**, updated at **B2**. Sections 1–5 describe publication and ownership
and still hold, with one change of vocabulary: what B1 called a chunk's *pins* is now a single
reference count, `PinnedChunk::refs`, which also counts the writer's and an open commit's
references (`src/pinned_arena.h`, decision D-041). **Sections 6 and 7 are historical:** B1
kept every replaced version and had no cleaner; B2 reclaims them by epoch-based reclamation,
described and argued in `docs/reclamation.md`. The "quiescent" release functions of §6 no
longer exist.

Code: `src/versioned_store.{h,cpp}`, `src/cow_segment.{h,cpp}`, `src/pinned_arena.{h,cpp}`,
`src/epoch.{h,cpp}`, reached through the `rcu` build of `KVStore` (`src/kvstore.cpp`).

## 1. Objects

```text
current ──atomic──> Version { sequence, segments[256] }        immutable once published
                        └──> Segment { capacity, live, tombstones,
                                       slots[capacity],          immutable once published
                                       pins[] = distinct chunks, sorted,
                                       owners (atomic) }         shared between versions
                                 slot = { key, ValueRef{chunk_id, offset, length}, state }

chunk directory (global): blocks[1024] ──atomic──> block[1024] ──atomic──> PinnedChunk
PinnedChunk { id, capacity, pins (atomic), used*, sealed*, bytes... }   * writer-only fields
```

- A **Version** is one complete state of the store. It embeds its directory (256 segment
  pointers), so "every version owns one distinct directory" (blueprint §3.3) holds by
  construction. A null entry is an empty partition.
- A **Segment** is one partition's open-addressed, linearly probed table: same slot format,
  hash, probing rule, load limit (0.70) and tombstone threshold (0.20) as Part A.
- The **chunk directory** is global and not versioned. Blocks are installed once and never
  freed while the store lives.

## 2. Read path

```cpp
h = hash_key(key);
v = current.load(seq_cst);                 // the one and only load of the root
e = find(v->segments[h & 255], key, h);    // immutable
memcpy(out, chunk_directory[e->value.chunk_id]->data() + offset, length);
```

No lock, no allocation, no write to shared memory, no waiting on the writer. Buffer contracts
are those of Part A (D-012).

## 3. Write path: one logical write, one publication

`put`, `erase` and the internal multi-key `commit` are the same code (`VersionedStore::commit`);
`put`/`erase` are batches of one.

| Phase | What happens | Can fail? | Visible to readers? |
|---|---|---|---|
| 0 | Validate every argument | yes → return false | no |
| 1 | Group operations by partition; one `SegmentBuilder` per touched partition, each a private copy of the published segment | yes | no |
| 2 | Apply operations in order to the private copies; value bytes appended to the arena beyond every published reference | yes | no |
| 3 | Allocate segment headers, the new Version, the retirement node | yes | no |
| — | **nothing below can fail** | | |
| 4 | Assemble the Version; new segments take their chunk pins and `owners = 1`; shared segments get `owners += 1`; arena commit | no | no |
| 5 | `current.exchange(next, seq_cst)` — the linearization point | no | **everything, at once** |
| 6 | Old version goes on the retained list using the pre-allocated node | no | — |

Consequences:

- **Atomic visibility.** Before step 5 no reader can reach any new object; after it every
  reader that loads the root reaches all of them. There is no intermediate root.
- **Copy-on-write.** Each touched partition is cloned exactly once per commit, however many of
  its keys the commit changes. Untouched partitions are shared by pointer.
- **Failure atomicity.** A failure in phases 0–3 destroys the builders (which own everything
  they allocated), rolls the arena back to where the commit began, and returns false. No
  ownership counter has been touched and `current` has not been written.
- **Retirement cannot fail or lose a version.** The node is allocated in phase 3. Between the
  exchange and the push the old version is owned by a local variable of the writer.
- Operations inside a batch apply in order; the last one on a key wins. A batch that changes
  nothing (erases of absent keys) publishes nothing.

Cost per publication, by design (blueprint §3.3 "cost acceptance for v1"): the touched
segments' slot arrays are copied, the 2048-byte directory is copied, and one owner reference is
taken on each of the up to 255 shared segments.

## 4. Memory ordering

Every atomic operation uses `memory_order_seq_cst` (blueprint §0: version 1 uses seq_cst for
all protocol atomics; the formal-safety supplement assumes it for B2).

Why a reader never sees a partially built version: all writes that initialize the Version, its
new Segments, their slot arrays, the chunk-directory entries and the value bytes are sequenced
before the writer's `exchange`. A reader's `load` that returns the new pointer is ordered after
that exchange (single total order of seq_cst operations; a release/acquire pair would already
give the required happens-before). Everything the reader then dereferences was written before
the exchange and is never written again.

Why the writer may keep appending to a chunk readers are reading: a reader touches only byte
ranges named by a `ValueRef` in a published segment; the writer writes only at offsets at or
beyond the chunk's `used`, which is past every such range. Disjoint memory locations, so no
data race. The chunk header fields the writer changes (`used`, `sealed`) are never read by
readers; `pins` is atomic.

The directory's block pointers and slots are atomics because readers load them while the
writer installs new blocks and chunks.

## 5. Ownership (replaces the Part A scheme, decision D-023)

Part A counted index entries per chunk and freed or rewound a chunk under the store lock. That
is unusable once several versions exist. B1 uses the structural model of blueprint §3.3–3.4:

- **Version → Segment.** `owners` = number of versions whose directory points at the segment.
- **Segment → Chunk.** Each segment holds one arena pin per *distinct* chunk its occupied
  slots reference (`pins[]`, with a per-chunk entry count so a builder can keep the set exact
  as entries are replaced or erased). Pins are taken before the segment is published and
  dropped only when the segment is destroyed.
- **Writer → Chunk.** The writable chunk belongs to the writer; it is never freed and never
  rewound while it is the writable one.

A chunk is freed, its directory slot nulled and then its ID made reusable only when it has no
pin, is not the writable chunk, and is not referenced by the commit in progress.

Invariants (checked by `VersionedStore::validate()`, which recomputes them from scratch):

| # | Invariant |
|---|---|
| O1 | `segment.owners` == number of existing versions (current + retained) pointing at it |
| O2 | `segment.pins` == exactly the distinct chunks referenced by its occupied slots, with correct per-chunk entry counts, sorted |
| O3 | `chunk.pins` == number of existing segments listing the chunk |
| O4 | an unpinned chunk exists only if it is the writable chunk |
| O5 | every `ValueRef` in every existing segment lies inside the written part of an existing chunk with that ID |
| O6 | retained versions == publications − releases; bytes held match the objects that exist |

Why a reader can never dereference freed memory or a reused chunk ID: a reader reaches a chunk
only through a slot of a segment of a version it loaded. While that version exists it owns the
segment (O1), the segment pins the chunk (O2, O3), and a pinned chunk is never freed nor its
slot reassigned (O4 and the freeing rule). So the argument reduces to: **the version a reader
loaded still exists.** In B1 that is true trivially, see §6. In B2 it becomes the EBR
guarantee.

## 6. Temporary lifetime strategy (B1 only; superseded in B2)

**Every replaced version is retained until the store is destroyed.** Nothing a reader can reach
is freed while the store is in use, so readers need no protection of their own. The cost is
unbounded memory growth: each publication keeps the replaced Version (about 2 KiB) and the
segments it alone owned. With the workload's 100,000 keys that is roughly 26 KiB per write.
This is deliberate for B1 (blueprint §8.5: "old versions retained intentionally") and makes the
store unsuitable for long write-heavy runs until B2.

Two internal, test-and-benchmark-only escape hatches exist. Both require the **caller's
promise that no other thread is reading the store**:

- `release_retained_quiescent()`: release everything retained so far (used after a
  single-threaded load, and at destruction).
- `set_quiescent_reclaim(true)`: release each replaced version immediately.

They are not a reclamation protocol: nothing in the store checks the promise, and they must
never be used with concurrent readers. They exist so that single-threaded tests exercise the
whole ownership graph (owner counts, pins, chunk freeing, chunk-ID reuse) on every write under
AddressSanitizer, and so benchmarks can start from a store that holds only its current state.
They are not reachable through the public API.

At destruction all versions are released through the same ownership graph; hook builds count
live Versions, Segments and Chunks and the tests assert all three return to zero.

## 7. What B2 had to add (done; see docs/reclamation.md)

1. Per-reader epoch slots; announce + validate **before** the root load (the marked line in
   `VersionedStore::get`).
2. Retirement epoch sampled after the exchange and stored in the node that already exists.
3. A cleaner with gated epoch advancement that calls `release_version` only for versions
   whose grace period has passed, with batch and work budgets; `set_reclaim_batch`.
4. Concurrency of ownership updates: `owners` and `pins` are already atomic, but
   `release_version`, the free-ID stack and the arena statistics assume one thread today.
   Writer/cleaner hand-over of freed chunk IDs needs a defined protocol.
5. Removal or strict gating of the quiescent hatches.
6. The proof gate of the formal-safety supplement and its deterministic schedules.

## 8. Known costs and limits at B1

- A write copies the touched segment (24 KiB at the workload's fill) and the 2 KiB directory
  and touches about 255 owner counters; measured figures are in `reports/milestones/B1.md`.
- `erase` now needs memory for a replacement version, so it returns `false` both for an absent
  key and for an allocation failure (with the store unchanged in either case).
- (B1 state; superseded.) Exactly one writer thread was assumed. Since C1 writes are
  serialized by a writer mutex, and `Transaction`, `Snapshot` and `keys` are implemented
  (`docs/transactions.md`); `create_index` and `index_get` remain for C2.
