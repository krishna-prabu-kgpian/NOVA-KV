# NOVA-KV — Engineering Blueprint and Implementation Contract

**Status:** Final coding-agent handoff specification, revision 2 (2026-10-12); gated implementation; external assignment ambiguities explicitly recorded  
**Target language:** C++17 or later, Linux, one node, one application writer per store  
**Assignment:** Design Optimization of Computing Systems, Assignment 2, due **25 October 2026, 11:59 PM**, 200 marks + 20 optional bonus; team size 2–4.  
**Source basis:** `assignment2(2).pdf` (11 pages) and `workload_gen(2).cpp` (151 lines) supplied by the user.  
**Reading guide:** `REQUIRED` = in assignment; `DESIGN` = selected project decision; `EXTRA` = beyond rubric; `OPEN / INSTRUCTOR` = cannot honestly infer from supplied material. This document explicitly distinguishes the latter rather than silently inventing professor requirements.

---

> **Instructions to implementation agent:** This is a binding technical plan, not a request to generate the full project in one shot. Follow the milestone gate in §16 and the separately supplied `NOVA_KV_Coding_Agent_System_Prompt.md`. Never claim a gate passed based on compilation alone. Report unresolved source incompatibilities instead of silently changing the provided reference generator.

> **Source-of-truth order:** assignment PDF and actually provided reference files for graded API/requirements; this specification for chosen engineering details; explicit user decisions for scope; measurements for tuning. If they conflict, identify the conflict, preserve the assignment's external contract, and log the decision. A hypothetical assumption is never a measured result.

## 0. Executive design specification

We will build a **generic, in-process, byte-value KV store** with a stable external API and independently benchmarkable synchronization policies. The RCU engine will use:

1. A **segmented, open-addressed hash index**. Each segment is a contiguous array using linear probing. Segments may resize independently and rebuild to remove tombstones.
2. A **copy-on-write (COW) directory** of segment pointers: writer groups updates by segment, clones affected segments once each, constructs a new directory/root, and publishes **one atomic root pointer** for every logical write (including single-key writes).
3. A **chunked append-only byte arena**. Hash entries contain `{chunk_id, offset, length}` rather than inline variable-length values. New values are appended; previously published bytes are never overwritten. Chunks have stable allocations; IDs may be recycled **only after all references to the prior allocation are gone**, and only with proper directory synchronization.
4. A **global chunk directory** mapping chunk IDs to chunk objects; it is not version-specific. Published entries are immutable for an allocation's lifetime, and chunk objects are pinned structurally by segments that reference them. Reuse of IDs requires safe withdrawal/republication of slots; no active reader may resolve an old reference to a new allocation.
5. **Per-participant epoch-based reclamation (EBR)** for readers, a bounded nonblocking retirement channel from the writer to a background cleaner, and internal non-read-path ownership accounting for shared directories/segments/chunks.
6. **One writer** which is the only publisher of database roots, including any later compaction relocation. Cleaner can identify or prepare work but cannot independently publish an index version.
7. `get()` **copies** bytes into the caller's buffer before releasing its protected version; internal pointers never escape.
8. Secondary-index joins, WAL, hot-key tier, small-value inlining, and arena compaction are separate later milestones; their integration points are defined now.

**Critical scope boundary:** The design gives atomic publication of a logical database version. It does **not** assume that separate atomically-updated key entries provide transaction atomicity. It also does **not** claim lock-free progress for memory allocation, OS scheduling, or transaction writers. The required point-read path uses no mutex and does not wait for the writer/cleaner.

### Selected defaults (tunable, not assignment mandates)

| Parameter | Initial value | Rationale / experiment |
|---|---:|---|
| Logical partition count | 256 | Compact single-level directory: 2048 bytes of pointers on 64-bit |
| Initial segment capacity | 1024 buckets | 100k keys / 256 ≈391 live entries per segment on average; 512 would exceed a 0.70 threshold in average-loaded segments. Benchmark 512/1024/2048. |
| Max occupied load factor | 0.70 | Favor short probes over memory density; tune 0.60/0.70/0.80 |
| Tombstone rebuild trigger | tombstones ≥20% of bucket capacity | Rebuild privately into an equal or larger segment |
| Arena chunk target | 64 KiB | Experiment with 16/64/256 KiB and oversized-value policy |
| Reclaim batch | 64 retired **versions per pass** | Test 8/64/256/1024 plus disabled and stop-the-world comparator |
| Epoch synchronisation, version 1 | `memory_order_seq_cst` for *all protocol atomics* | Conservative correctness baseline before optimizing |
| Chunk compaction trigger | ≥80% obsolete **bytes** | EXTRA, disabled until base reclamation stable |
| Consumer/writer topology | one writer; one cleaner; multiple readers | Matches assignment's single-writer assumption |

These are provisional engineering defaults, not measured optima. Final parameter choices require profiling; do not report hypothetical numbers as experiments.

---

## 1. Requirements and differences from implementation choices

### 1.1 Mandatory parts

| Part | Marks | Exact assignment requirement | Evidence |
|---|---:|---|---|
| A | 50 | `KVStore` `put/get/erase`, own hash table, sanity test; `single_thread.cpp`, `global_lock.cpp`, `rwlock.cpp`; mutex and shared mutex baselines; NSM/DSM/PAX note with ≥2-layout actual microbenchmark; profiling at 1/2/4/8 readers | Sources, test, perf stat, perf record/flame graphs, ≤5-page Part A report |
| B | 40 | RCU version construction off-side; one atomic current pointer publication; one atomic current pointer load per read; no writer-held reader locks; EBR or hazard pointers; public `set_reclaim_batch(int)` and bounded per-call reclamation; no torn row test | Modified sources, A→B diff, README, stress test |
| C | 60 | `Transaction` staged `put` and one-root `commit`, abort on destruction; snapshot-consistent reads; secondary index; one pinned snapshot for outer `keys` and every join index probe/row fetch; isolation/durability explanation | B→C diff, functional and concurrency tests, design notes |
| D | 50 | RPS and p50/p99/p999 for specified baselines and RCU at 1/2/4/8 readers; 100k+ 8-reader target; batch sizes and no-reclaim/STW comparators; joins with/without writer; commits of 1/4/16 keys; raw data | CSV and raw traces, profiling and plots, explanation |
| E (optional) | +20 | WAL before/as part of commit publication, restart replay, no added per-read overhead; fsync/group commit reasoning and write-side comparisons | WAL, recovery tests, diff, costs, production-gap note |

`KVStore` and `Transaction` are noncopyable and nonmovable; `Snapshot` is move-only. The public signatures from Appendix A are a compatibility contract, not suggestions. Do not introduce mandatory arguments or change return types.

### 1.2 Important contradictions or underspecified areas

1. **Provided `workload_gen.cpp` performs `run_writer()` to completion before starting reader threads.** Yet Parts A/D demand one active background writer throughout. Keep supplied generator unchanged; use an additional benchmark harness for actual concurrency. Label these as distinct experiments.
2. **Random lookup keys mismatch inserted key space.** Inserted `trade_id = i*100000 + j`, for `i in [0,999]`, `j in [0,99]`, but random lookups use `rng()%(m*n)`. Default random branch mostly misses; log hit ratio and run extra true-uniform-hit/miss workloads. Preserve supplied behavior in reference runs.
3. **Reference `workload_gen.cpp` mentions `Transaction` as a complete `std::optional<Transaction>` type, even when `use_txn=false`.** A Part-A-only header per the progressive API may not compile that file unchanged. The build must either provide the future declarations/implementation for compatibility from the beginning or use an unmodified reference source against a complete header at later milestones. Record build convention clearly; do not edit supplied generator.
4. **`use_join=true` in supplied generator is only an instrument `index_get`, not a complete snapshot-scoped index-nested-loop join** using `keys`, full inner-row `get`, and one shared snapshot. Implement a separate compliant join benchmark.
5. **Two distinct KVStore instances (`trades`, `instruments`) versus one snapshot spanning an entire join:** One independently acquired snapshot per store guarantees each table is internally consistent, which can be sufficient when the Instrument table is loaded once and never modified during joins. However, two acquisitions are not one atomic cross-store snapshot and do not guarantee a consistent database-wide instant if BOTH stores are being updated. Baseline Part C strategy: acquire and retain one snapshot for Trades and one for Instruments during the complete join, pass the appropriate snapshot to all operations on that store, and explicitly state the per-store guarantee. For the stronger assignment wording of exactly one Snapshot total across both tables, seek instructor clarification or implement shared internal publication context without changing the public API. Never claim cross-store atomic consistency merely because two handles exist.
6. **Part D calls for longest reader stalls, but provided `WorkloadResults` exposes only total operations, RPS, p50, p99, p999.** Custom harness must preserve full latency samples/max.
7. **The wording “exactly one atomic load of current pointer” competes with textbook hazard-pointer validation loops.** Proposed EBR read path loads current exactly once after announcing protection; epoch protocol uses other atomic operations.
8. **Snapshot isolation terminology:** The supplied `Transaction` API has staged `put` and `commit`, but no transactional read set/conflict validation. Precisely claim what is implemented: atomic single-writer commits and pinned snapshot reads; explain textbook SI/write-skew without falsely asserting full SI for arbitrary read-write transactions.
9. **Buffer-too-short behavior, zero-byte values, repeated `commit`, and `set_reclaim_batch(n<=0)` are not specified.** Selected contract defined below. State these as design policies, not professor demands.
10. **“Batch reclamation never blocks writer” may conflict with bounded queue capacities under unbounded sustained production.** We need a nonblocking handoff protocol and explicit resource-growth limitations; no finite-memory algorithm guarantees unlimited progress if producer permanently outpaces cleaner.

### 1.3 Workload generator: exact supplied behavior

The provided source declares:

```cpp
struct Trade      { long trade_id; char symbol[8]; double price; long ts; };
struct Instrument { char symbol[8]; char venue[8]; int tick_size; };
```

`make_trade(i,j)` assigns `trade_id = i*100000 + j`, `symbol="SYM%03d"` with `(i+j)%50`, price `100.0+(i%97)*0.37`, and timestamp from `steady_clock`. `run_writer()` inserts `m*n` Trade values (`m=1000,n=100`) via `put()` or staged `Transaction` commits of `txn_size` values; every `hot_every=100`th inserted key enters `hot_ids` (1000 hot IDs). It calls `txn->commit()` again at the end even if the last transaction was just committed (empty commit edge case). `load_instruments()` creates a secondary index on `Instrument.symbol` and inserts 50 instruments keyed 0..49; it is called **after** `run_writer()` when requested. `run_reader()` independently seeds one `mt19937` per thread, chooses 80% from hot IDs and 20% by `rng()%(m*n)`, times `trades.get()` plus optional `instruments->index_get()` (up to 8 matches), and appends each sample into a private vector. `workload_run()` starts readers after loading, sleeps nominal `duration_sec=5`, stops/joins, merges/sorts samples and computes `rps = number_of_samples / duration_sec` and p50/p99/p999 indexed by `floor(N*p)` clamped at N-1.

Reference results must retain these semantics. The auxiliary harness explicitly corrects only its own scenarios (writer concurrency, valid random hit keys, joins, commit measurements), not the supplied source.

---

## 2. Public API contracts and errors

### 2.1 Assignment-mandated signatures

```cpp
class KVStore {
public:
    KVStore();
    ~KVStore();
    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    bool put(long key, const void* value, size_t value_len);
    bool get(long key, void* value_out, size_t value_out_len) const;
    bool erase(long key);

    // Part B
    void set_reclaim_batch(int n);

    // Part C
    bool create_index(size_t field_offset, size_t field_len);
    int index_get(const void* field_value, size_t field_len,
                  long* keys_out, int max_keys) const;

    class Snapshot; // final nesting/namespace chosen to match exact Appendix A spelling
    Snapshot acquire_snapshot() const;
    bool get(const Snapshot&, long, void*, size_t) const;
    int index_get(const Snapshot&, const void*, size_t, long*, int) const;
    size_t keys(const Snapshot&, long*, size_t) const;
};

class Transaction {
public:
    explicit Transaction(KVStore&);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void put(long, const void*, size_t);
    bool commit();
};
```

**Header caution:** Appendix A renders `class Snapshot` under “added to KVStore”; implement its final qualified location consistently. Tests must compile the actual intended API; don't invent an incompatible namespace. `Transaction` is a distinct class referring to a `KVStore`.

### 2.2 Chosen error contracts (not specified in PDF)

- `put(key, value, len)` stores exactly `len` bytes, copying before returning; `len==0` is permitted and represents an existing empty value; for `len>0`, `value==nullptr` returns false; size/allocation failure returns false and publishes nothing. Reject unreasonably large lengths exceeding supported arena capacity/offset encoding.
- `get(key, out, capacity)` returns false for absent key **or insufficient buffer**; for an existing empty value with `capacity==0`, return true. For `len>0`, null destination fails. `get` never copies a truncated record. Document that required length is not discoverable via the assignment API; use typed fixed-size records in benchmark.
- `erase(key)` returns false if missing, true if removal commits. Deletion uses a tombstone in a private cloned segment; it never edits a published segment.
- `set_reclaim_batch(n)`: if `n <= 0`, clamp to 1 (chosen policy); read batch atomically to avoid a writer-cleaner data race; document exact behavior. A setter call isn't part of the reader hot path.
- `Transaction::put` copies staged bytes immediately. If preparation fails, record failure internally because `put` returns void. `commit` returns false without publication. Successful `commit()` finalizes the transaction; another `commit()` returns false (except optional explicit no-op policy). **Reference generator creates and commits an empty transaction at the end; choose success for a first empty commit with no new root** to preserve compatibility; document that separately from repeated commit.
- `create_index(offset,len)` fails on invalid range/schema incompatibility; validate values before indexing. Exact API has no schema descriptor, so one fixed indexed byte range per store instance is assumed, and values inserted under indexed configuration must cover that range.
- A client must stop/join external readers and writer and destroy explicit Snapshot handles before destroying a store. Destroying concurrently with an active public call is unsupported.
- Neither `KVStore` nor `Transaction` is copyable/movable. `Snapshot` is move-only; its moved-from state is inert.

All public operations remain `noexcept` only if the chosen allocator and implementation can actually uphold that guarantee; otherwise catch allocation failures at the public boundary and translate to the specified return without corrupting the current version.

---

## 3. Physical representation and lifetime ownership

### 3.1 Conceptual data structures

```cpp
struct ValueRef {
    uint32_t chunk_id;
    uint32_t offset;
    uint32_t length;
};

enum class SlotState : uint8_t { Empty, Occupied, Tombstone };
struct Entry {
    long key;
    ValueRef value;
    SlotState state;
};

struct Segment {
    // private mutable during construction, immutable after publication
    uint32_t capacity;  // power of two
    uint32_t live_count;
    uint32_t tombstone_count;
    Entry* slots;       // aligned contiguous allocation
    // ownership metadata for unique chunk IDs referenced by this segment
    // intrusive atomic owner count or equivalent independently managed control block
};

struct Directory {
    uint32_t partition_count;
    Segment** segments; // dense immutable array, partition IDs are implicit
};

struct Version {
    uint64_t commit_sequence;     // diagnostic: NOT reclamation epoch
    Directory* primary;
    // Part C: SecondaryIndexRoot* secondary;
};

struct Chunk {
    uint32_t id;
    size_t capacity;
    size_t used_bytes;
    std::byte* data;              // fixed address for allocation lifetime
    // writer/cleaner-side lifetime and fragmentation metadata
};

std::atomic<Version*> current;
```

Actual C++ source may wrap raw pointers with unpublished-stage RAII builders. Readers only dereference fully published immutable objects. Metadata fields that the cleaner/writer update concurrently **must be atomic or externally synchronized** and never be treated as reader-visible mutable state.

### 3.2 Hash partition and probing

Compute a well-mixed 64-bit hash of signed `long` key. On 64-bit test platforms use `uint64_t(key)` and a fixed, documented avalanche mixer; ensure all collision decisions use the full key for equality. With `P=2^p` partitions, `segment_id=h & (P-1)`. For segment with `C=2^c` slots, choose start bucket `(h>>p)&(C-1)`, then probe `(start+i)&(C-1)`. There is no cross-segment probing. Hash collision is resolved within the chosen segment by full-key comparison. Avoid `std::hash` performance assumptions without measurement.

Lookup: stop at `Empty`, skip `Tombstone`, match `Occupied && entry.key==key`. Put: probe for existing key or first tombstone; if no existing key, place in remembered tombstone or empty. Erase: replace occupied entry with tombstone in **private copy**. Rebuild: reinsert only live entries; zero tombstones. Independently resize partition segment when `(live+tombstones+additional)/capacity > 0.70`. If full, double capacity. Trigger equal-capacity rebuild if `tombstones/capacity >= .20`. Never rehash or modify a published segment in place. Instrument probe lengths and collision counts.

**No duplicate key may appear in one segment.** Multiple staged writes to the same key follow caller order; the final staged operation wins. Group by segment before clone, apply all its changes once. Transaction segmentation must use the *same* hash partition function used by readers.

### 3.3 Directory ownership

Every published Version owns one distinct Directory, which owns one reference to each Segment pointer it contains. Multiple directories may share a Segment object. Maintain a control count of **directory references to each segment**. At construction, before publishing the new root, increment counts on reused segments and set ownership on newly allocated segments; release on rollback. After a retired version is safely reclaimed, destroy its directory and decrement each referenced segment count. When a segment reaches zero, it is safe for the cleaner to destruct and release its chunk pins. Thus there is no per-read count.

**Cost acceptance for v1:** A 256-entry directory is copied and 256 ownership references processed per transaction (or fewer if using an optimized internal directory-group representation later). This costs O(P) per publication and O(P) per retired root destruction, where P=partition count. Benchmark and cap cleanup work; do not pretend a free of one version always costs O(1). Future optimization: two-level COW directory and refcount groups.

### 3.4 Chunk ownership: exact conservative v1 model

For every Segment, during construction compute the **set of unique chunk IDs** referenced by its occupied entries. The Segment owns **one pin per unique chunk ID** (not one per value), held from before publication until the Segment is physically destroyed. If a published segment S references chunk C, C cannot be freed. A newly cloned segment pins its referenced chunks before it is published; when old S is reclaimed, it releases its chunk pins. This can be O(entries in affected segment), acceptable as an explicit correctness-first cost. A chunk also has an **arena-manager/writer allocation owner** while it is current/writable and while unpublished allocations are being committed/rolled back.

The effective condition for physically freeing a sealed chunk is: no writer allocation owner, no segment pins, and no pending unpublished references. Since readers can dereference ValueRefs only through protected published segments, once no segments (including old retained ones) reference it, old readers cannot validly access it. **Careful:** a segment's count reaching zero must be established only after its owning retired directory is safe to destruct (EBR grace period). This links segment safety to reader safety.

`obsolete_bytes` used for future compaction is *not* a lifetime count. Bytes may be obsolete in latest root while still pinned by older segments. For v1, derive or update latest-root live-byte statistics conservatively and only free **entire unreachable chunks**; moving live values is a later milestone.

### 3.5 Global chunk directory and reusable IDs

This directory is unrelated to the versioned hash-segment directory:

- **Hash-segment directory:** immutable, per-Version, maps partition ID → hash segment.
- **Chunk directory:** global, maps `chunk_id` → chunk base address; all snapshots share it.

We choose a **stable two-level directory**: preallocated top-level pointer array to fixed-size blocks (for example 1024 chunk slots/block), with each block installed atomically once; each slot is an atomic `Chunk*`. Readers first use the stable top-level pointer for `block_id`, then `slot.load(seq_cst or acquire)`. The top-level bounds/capacity must never be concurrently mutated as an ordinary vector. Choose a documented upper bound on number of directory blocks, or introduce an atomic top-level-table swap with its own safe lifetime management. **Initial build:** reserve a fixed capacity sufficient for measured workload plus stress tests; exceeding it returns allocation failure without publishing anything. This avoids implementing a second independent RCU resize protocol prematurely.

Allocating a new chunk: claim unused or safely recycled numeric ID from writer-owned free list, allocate and initialize Chunk/data, then publish slot pointer. **Publish chunk before publishing any version whose ValueRef uses it.** On retirement: drop all pins and allocation owner, wait for structural safety as above; set slot to null and reclaim object; *only then* return numeric ID to the free-ID pool. A new chunk with reused ID may subsequently be installed in the null slot. IDs need not increase forever. An optional `(id,generation)` debug tag can detect accidental stale identities but does not replace safe lifetime management.

**Proof obligation for ID reuse:** A reader's protected root points to a segment; that segment pins all its chunks. Consequently, if a reader can still traverse an old ValueRef, the old segment and corresponding chunk pin still exist, so the chunk slot cannot have been cleared/reused. The global directory is therefore safe without a separate per-Version copy. The directory block itself is never freed while the store runs.

### 3.6 Arena allocation and chunk states

States: `Writable` (append only, from single writer), `Sealed` (will never accept more appends; may contain live bytes), `RetirementEligible` (no writer owner and no pins), `Reclaimed` (allocation freed, ID slot reusable). A chunk can be sealed and live indefinitely; sealing is **not** retirement. The writer never modifies byte ranges already published. It can continue appending into unused space of a writable chunk provided allocated address is stable and publication correctly orders byte initialization.

Chunk size v1 64 KiB. For value longer than ordinary chunk capacity, allocate one dedicated large chunk of suitable size, store offset 0, and account it identically. Handle 0-byte values with `length=0` and a documented sentinel representation that does not dereference a chunk. Check arithmetic overflow, bounds, alignment requirements and `uint32_t` limits. `get()` handles bytes, not typed `Trade*` casts, so it may use `std::memcpy` and is not required to return arena-backed pointers.

Unpublished transaction allocations: use per-commit staging reservation/allocator marks where possible; on failure, roll back active tail only if safe; otherwise mark abandoned bytes for later waste accounting. Never publish stale references to partially initialized values.

---

## 4. Exact operation algorithms

### 4.1 Normal `get(key,out,capacity)`

**Precondition:** Store object alive; caller's destination remains valid for call duration; snapshot acquisition succeeds. **No malloc, no mutex, no writer/cleaner wait in steady-state read path.** Thread registration is amortized before timed benchmark, not done on every read.

1. Enter EBR protected region using caller's registered reader slot; announce protection before root load.
2. Load `Version* v = current.load(std::memory_order_seq_cst)` **once**.
3. Hash key; compute partition index; read immutable `v->primary->segments[partition]`.
4. Linear-probe segment. If missing, exit EBR and return false.
5. Read copied `ValueRef` from immutable entry; verify `capacity>=length`, pointer nonnull when `length>0`, and chunk/offset range valid. If invalid input, exit and false.
6. Load immutable/stable global directory block and atomic chunk pointer by ID. Because Segment pins chunk, it cannot be null for a valid published ValueRef.
7. `memcpy(out, chunk->data+offset, length)` while still EBR-protected; no arena pointer leaves API.
8. Exit EBR and return true.

On compiler path: RAII guard ensures exit on every early return. No `std::shared_ptr` acquisition on the read path, no per-key/chunk reference-count increment, no logging, no mutable hot-key metrics. Runtime debugging may assert invariants; optimized build must not introduce synchronization in an unchecked fast path.

### 4.2 `put(key,value,len)`

The application writer alone executes the publication path; no competing writers. Treat this as a one-operation transaction:

1. Validate inputs; snapshot/copy bytes into staging while caller still owns buffer; allocate new arena range and copy value there.
2. Load latest root (writer's own view); select partition by hash.
3. Clone affected Segment. Apply insert/overwrite; rebuild or grow private segment if needed. Build unique chunk pin set; establish ownership of chunk references for new segment.
4. Allocate/copy new Directory (256 pointers), replacing only chosen slot with new Segment; adjust shared segment reference counts; construct Version with next monotonic `commit_sequence`.
5. After all allocations and bookkeeping succeed, execute one atomic current pointer store/exchange with `seq_cst` for initial implementation. This is linearization / visibility point.
6. Enqueue old root on nonblocking retirement channel (reserve retirement descriptor/channel capacity **before** publication, so enqueue cannot fail afterwards). Transfer ownership of old root to collector. Return true.
7. Failure before step 5: release private structures and return false; old root remains published. Avoid failure-prone work after step 5.

**Critical practical detail:** A writer-owned current root cannot be destroyed while reader/cleaner lifecycle permits old readers. The retiring root enters collector ownership after exchange; any transition gap must not lose its memory or make it inaccessible to reclaim safely. Build a preallocated retirement node before commit and publish it with proper ordering after swapping current.

### 4.3 `erase(key)`

Confirm key exists in latest root first; if absent, return false without allocating a new version. Otherwise clone affected segment, place tombstone, rebuild when threshold crossed, clone directory, publish atomically, retire old root. Old Snapshot continues to see original key. New lookup sees absent key. When old retained segments holding old ValueRef are eventually destroyed, chunk pins may be released.

### 4.4 `Transaction::commit()`

1. Transaction stages **deep copies** of `(key,value,len)` in order; it never stores caller's transient input pointer.
2. Consolidate repeated keys to last staged write; group keys by primary partition.
3. Preflight all lengths, memory capacity, retirement node capacity, and resource limits to the extent possible; all mutations are to private or unpublished structures.
4. Append new bytes to arena; copy each touched hash Segment exactly once; apply all updates in its group.
5. Finalize chunk pins and shared segment ownership, build one new Directory replacing **all** touched segment slots, build Version.
6. Publish **one** atomic current root pointer swap. Nothing becomes visible before step 6, everything becomes visible at step 6.
7. Retire old root, finalize committed transaction, return true. On any preparation failure, publish nothing and return false.

A transaction updating A and B in different segments never performs `current[A].store` and `current[B].store`; only the root changes. Multi-key read consistency across separate bare `get()` calls is not promised; to read a coherent multi-step state, caller must hold one explicit snapshot (Part C).

### 4.5 Snapshot lifecycle (Part C)

Normal `get` owns an EBR guard only for the call. Explicit `Snapshot` owns one independent **pin registration token** for its entire lifetime and a pointer to one protected Version root. `acquire_snapshot()` protects before acquiring root, loads current exactly once under that protection, and returns the opaque move-only handle. `get(snap,...)`, `keys(snap,...)`, and `index_get(snap,...)` do not reacquire current and do not release the handle's pin. Destructor releases pin. Move transfers pin-token ownership without copying it. Moved-from snapshot is inert.

**Implementation decision for cross-thread move:** The pin token owns a dynamically/pooled stable announcement slot, **not a thread-local slot**. A Snapshot may transfer between threads sequentially; concurrent use/mutation/destruction of the same handle is not supported. Slot remains active until final handle destruction. Slot allocations can occur during explicit snapshot acquisition; ordinary `get` stays on cached per-thread slots. Reclamation scans both short-read and Snapshot token slots. Document max concurrent tokens and registration exhaustion policy. Snapshot slot should record a protected epoch without changing the root visible to that snapshot, even as global epoch moves forward. On move, transfer ownership of the same registered slot; do not temporarily unpin/re-pin.

### 4.6 Secondary index and join (Part C, deferred implementation)

Extend Version with secondary index root so primary and secondary mappings for a **single KVStore** publish together. Use a hash index keyed by canonical copied field byte sequence `(field_offset,field_len)`, mapping to matching primary keys. Maintain membership correctly on insert/update/erase and on changes to the indexed bytes. Build `create_index` privately and publish new root; no public schema changes concurrently. `index_get(snap,...)` returns up to `max_keys` IDs from same Version. `keys(snap,...)` enumerates keys actually present in the pinned index; support deterministic test ordering if necessary by sorting benchmark output outside read-critical section.

Cross-Trade/Instrument join: hold one Trade snapshot and one Instrument snapshot for the full join as an explicitly labeled **per-store consistency** solution. This gives coherent results per table and is often sufficient for the provided workload, where Instrument metadata is preloaded and not subsequently changed. If both tables change, the pair is not an atomic database-wide snapshot. The PDF's stronger demand of **exactly one Snapshot handle for the entire cross-store join** remains `OPEN / INSTRUCTOR`; it would require a shared publication context or combined immutable root. Do not describe the two-handle solution as fulfilling that literal condition.

---

> **Mandatory correctness supplement:** Read `NOVA_KV_Formal_Safety_and_Agent_Audit.md` before implementing any EBR, retirement, chunk-ID reuse, or reclaimer code. The supplement gives the explicit SC proof assumptions and adversarial checks; in any disagreement, its stricter safety gates govern.

## 5. Epoch-based reclamation (EBR): chosen algorithm and memory-order contract

### 5.1 Epochs versus commit versions

`commit_sequence` increments once per published database state. `global_epoch` is an independent reclamation coordination generation; it may advance with no commit, and thousands of commits may retire while it remains unchanged. Neither a fixed number of commits nor “30 generations old” proves safety: a reader may be stalled for arbitrarily many commits. Age threshold may defer scans or select candidates **only after** correctness is established by reader announcements/grace period.

### 5.2 Specific selected protocol: conservative SC epoch announcements and scan

This design uses *monotonically increasing epochs*, pinned reader slots, and a single cleaner that advances reclamation epoch and scans them. It follows the classical EBR principle, with conservative global `seq_cst` ordering for the first version. It is **not** safe to copy only the pseudocode fragments without the entire ordered protocol.

**Reader slot:** one atomic 64-bit state (`INACTIVE` sentinel or active epoch), cache-line isolated (e.g. alignas(64), benchmark actual false sharing). Slots are allocated/registered before use and remain address-stable while cleanup scans them. A short-read thread owns a cached slot and can use it for one non-nested get at a time; nesting requires depth management or separate slot. Snapshot tokens own independent stable slots. Registry mutation and token retirement must not race with scans; use immutable/RCU-protected published slot registry or a coordinator synchronization design that does not affect steady-state reads. For initial implementation, pre-register a fixed maximum number of short readers and snapshot slots, avoid registry resize during active operation; reject exhaustion explicitly.

**Enter:**

```cpp
for (;;) {
    uint64_t e = global_epoch.load(std::memory_order_seq_cst);
    my_slot.store(e, std::memory_order_seq_cst); // announces BEFORE acquiring root
    if (global_epoch.load(std::memory_order_seq_cst) == e) break;
    // epoch moved while entering: retry until announcement validates
}
Version* v = current.load(std::memory_order_seq_cst);  // EXACTLY ONE current load
```

Do **not** dereference any pointer before the epoch-validation loop succeeds. The loop reloads `global_epoch`, **not** `current`; therefore this can satisfy the assignment's one-load-current requirement. The read section ends with `my_slot.store(INACTIVE, seq_cst)`. A Snapshot leaves its dedicated slot active until destructor. Nested acquisitions using the *same* slot are not allowed without a depth counter; external snapshots have distinct slots.

**Publish and retire (writer):** initialize all new immutable structures; `old = current.exchange(next, seq_cst)`; obtain `retire_epoch = global_epoch.load(seq_cst)` **after publication**, and enqueue `(old, retire_epoch)` into ordered retirement channel. Retiring with an epoch equal to or newer than the true publication epoch is conservative. Reserve node before exchange; queue visibility to cleaner has release/acquire/SC publication ordering. A retire record is not eligible while it is still owned by writer/unpublished to queue.

**Cleaner epoch advance + scan — classical three-epoch gate (revised):**

```cpp
uint64_t e = global_epoch.load(std::memory_order_seq_cst);
// Examine every stable registered reader and Snapshot slot with seq_cst loads.
// If any active slot announces an epoch different from e, do NOT advance.
// If every active slot announces e (or is INACTIVE), attempt exactly one
// seq_cst CAS of global_epoch from e to e + 1.
// Do not wait/spin for a lagging participant; retry on a later cleanup cycle.
```

This follows the standard *advancement gate* of classical three-epoch EBR: pinned participants can witness at most one successful advancement while remaining pinned. An object retired and correctly tagged in epoch `r` becomes eligible only when the global epoch has advanced at least twice beyond `r` under this gate **and** retirement publication/registration ordering is valid. A conservative final participant scan may also be retained. **Do not unconditionally `fetch_add` on every cleaner pass**; that is a different epoch scheme, not the chosen classical protocol. A stalled participant prevents the next advancement, causing deferred reclamation without blocking reads or writer publication. Implement modular/wrap-safe epoch comparisons or explicitly prove the counter cannot wrap during process lifetime.

Proof obligations: each short reader and long-lived Snapshot announces before its single `current` root load and cannot be missed by a cleaner deciding the epoch advance; an entry that races with advancement validates/retries, with no dereference before a valid announcement. Once a retired version has survived two completed, properly gated epochs, no reader pinned early enough to access it can remain active. SC orders are a deliberately conservative first implementation, not permission to skip a slot-registration or retirement-publication proof. Use the source references in the runbook and make the implementation reviewable at each atomic operation.

**Critical audit requirement:** A single SC collector scan is safe only with correctly stable slot registration and publication. New slots cannot appear unseen after registry scan but before joining the critical section. Before enabling reclamation, write a protocol proof and implement deterministic forced interleaving tests, ThreadSanitizer-safe non-reclamation portions, and ASan/UBSan stress. For higher assurance, compare behavior with a known EBR implementation's protocol and review the implementation against the C++ memory model. A production-quality EBR may use an explicit grace-period handshake if any ambiguity in this proof remains. There is no license to free solely by age.

**Advancement policy:** cleaner attempts at most one **gated** epoch advance per cleanup cycle, without waiting for active readers. If any active reader is pinned in an earlier epoch, it does not advance. Do not spin until readers exit; that would make cleaner unbounded. A stalled reader may delay many retirements: document as unavoidable EBR limitation.

### 5.3 What memory ordering means

Atomicity answers “can two threads partially observe the same atomic word?” Memory ordering answers “which other writes become visible in relation to that atomic word?” Example: writer allocates and fully initializes Segment S, installs S in new Directory V, then publishes `current=V`. A reader must not see `current==V` while still seeing uninitialized S. A **release** pointer store and **acquire** pointer load are the standard publication mechanism. `seq_cst` also provides those visibility guarantees plus a single total order over all SC atomic operations, which simplifies reasoning about reader announcements versus the cleaner scan. `memory_order_relaxed` for epoch flags without a robust protocol may allow collector to miss a reader and free in-use memory. Compiler and CPU may reorder independent operations unless constrained; `volatile` does not make them safe. The first implementation uses SC; instrument and replace selectively only with a tested proof.

### 5.4 Cleaner and batch semantics

- Writer is producer; cleaner is consumer of retired Version descriptors. Use a preallocated, bounded-duration enqueue with no mutex or condition-variable wait in writer publication path. One practical choice is an SPSC linked retirement queue whose next node is reserved before publishing the new root, with atomic next pointer and memory lifetime managed by single consumer; a fixed ring is only valid if full-ring overflow is handled without blocking or losing retirement records. For v1, preallocate headroom based on benchmark and allocate additional nodes **before** publication; if out of memory, fail commit rather than publish and lose a retirement descriptor. This can limit write availability under resource exhaustion, an explicit deviation from an ideal unconditional nonblocking writer; do not claim mathematically unbounded nonblocking progress.
- Cleaner reads `reclaim_batch` atomically. In one pass, it physically destroys at most `n` retired **Version** objects (assignment's unit). **Additionally bound scan count/time per pass** by a separate work budget to avoid walking an unlimited backlog. It can store a cursor over retirement queue and carry work forward.
- Destroying one retired Version might decrement up to P segment references and cause several Segment destructions and chunk-pin releases. For strict wall-clock bounded passes, break destruction into resumable work units and carry pending tasks across passes; count a version fully freed only when cleanup of its ownership graph completes. `n` is an upper bound, not a latency guarantee.
- Deallocation should occur on cleaner, never on active reader. Allocator contention/cache interference remains possible; measure reader tail latency.
- If cleaner falls behind, monitor queue length, retained bytes, oldest pin and memory pressure; no algorithm with finite physical memory can sustain infinite nonblocking writes into a permanently blocked reclaimer. Explicitly report pressure/failure behavior rather than hiding it.
- **Benchmark comparators:** reclamation disabled (retirement-only, memory growth deliberately allowed for bounded tests); naive stop-the-world (test-only comparator synchronizing/blocking readers while freeing entire retire list); regular bounded cleaner. Never use STW in production benchmark mode.

### 5.5 Reclamation test obligations

Run forced race tests around (a) reader enter vs epoch bump, (b) root load vs retirement, (c) reader exit vs scan, (d) snapshot token moved between threads, (e) chunk ID reuse after full retirement, (f) continuous readers keeping system non-quiescent, (g) deliberately stalled reader holding an old snapshot across thousands of commits. A test that only checks for no torn rows is insufficient to establish no use-after-free: use sanitizer builds, forced preemption/fault injection, ownership assertions and long stress campaigns. Never “solve” test crashes by disabling reclamation in compliant runs.

---

## 6. Index rebuilds, chunk reclamation, and future compaction

### 6.1 Hash-segment garbage (required for functional erase)

Tombstone = deleted bucket marker preserving probe-chain continuity. `Empty` stops lookup, `Tombstone` does not. Rebuild a **private** segment when tombstone ratio exceeds threshold or effective occupancy causes long probes. Reinsert occupied entries only; recompute chunk pins; publish rebuilt segment in next root. Older segments remain valid for old readers. Segment rebuild is not the same as arena chunk compaction.

### 6.2 Whole-chunk reclamation (core)

When no segment pins and no arena manager owner remain, cleaner can safely free sealed chunk allocation, clear global directory slot, and return its numeric ID to free list. A sealed chunk with one still-referenced value **cannot** be freed as a whole. Chunk memory reclamation and Version reclamation both draw on cleaner work budget but `set_reclaim_batch(n)` legally constrains only retired **versions** per pass; document separate chunk work cap.

### 6.3 80% obsolete-byte compaction (EXTRA, after Parts A/B correct)

Compute candidate ratio `obsolete_current_bytes / used_bytes ≥ 0.80`. Define *current-live* using current primary index reachability, not “number of historical append records” without verification. Track metadata cheaply in writer and use a periodic full liveness audit only in off-hot-path tests. A chunk with 80% obsolete bytes still contains 20% live bytes and cannot simply be freed. Cleaner can identify candidate, snapshot candidate's live mappings, and optionally copy live bytes privately. Writer validates each mapping still matches original `(chunk_id,offset,length)` before incorporating relocation in a new root. Writer publishes new index references atomically; old chunk retires only after no retained segment pins it. Metrics: bytes copied, bytes freed, false candidate rate, writer publication overhead, read p999 impact. This is a separate tunable from tombstones and reclaim batch size. **Do not implement before baseline root/EBR correctness.**

### 6.4 Cleaner/writer communication channels

| Event | Producer | Consumer | Data | When |
|---|---|---|---|---|
| New published value | Writer | Readers indirectly | New immutable current root | Atomic publication; no per-reader notification |
| Retired version | Writer | Cleaner | `(Version*, retire_epoch)` | Every successful root replacement |
| Epoch protection | Reader/Snapshot | Cleaner | Per-slot announced epoch or INACTIVE | Before/during/after read |
| Segment lifetime change | Writer/cleaner | Ownership counters | New directory ref vs reclaimed directory release | Construct/destroy only; **not per get** |
| Chunk pin change | Segment owner | Chunk manager/cleaner | Unique chunk ID pin count | Segment construct/destroy |
| Candidate dead chunk | Cleaner/metrics | Writer | Candidate ID and relocation proposal | Optional compaction stage |
| Validated relocated ValueRefs | Writer | Readers indirectly | New root; never mutate old refs | Atomic publication |
| Free chunk ID | Cleaner | Writer free-ID pool | Reusable ID only after nulling safely | Physical chunk retirement |

---

## 7. Concurrency invariants and correctness checklist

These are non-negotiable and should appear as comments/assertions in source and tests:

- **INV-01** Every published Version, Directory, Segment, secondary-index node and referenced value bytes are immutable for their published lifetime.
- **INV-02** At most one writer publishes `current` for a store. Cleaner does not independently publish.
- **INV-03** A successful `put`, `erase`, or `Transaction::commit` has exactly one root-publication visibility point.
- **INV-04** Reader announces valid protection **before** its one load of `current`; protections cover all dereferences and the final copy-out.
- **INV-05** No published Segment points to a chunk that lacks a structural lifetime pin.
- **INV-06** Chunk directory entry for ID `x` cannot change from allocation A to B while any retained published Segment can contain a ValueRef intended for A.
- **INV-07** Reclaim old root only after EBR proves no current/old reader or explicit snapshot can dereference it.
- **INV-08** Shared Segment destroyed only when directory owner count zero **after safe parent retirement**; Segment releases its chunk pins exactly once.
- **INV-09** No per-get writer lock, allocator, WAL logging, or reference counting on shared control block.
- **INV-10** Failed commits publish nothing; staged allocations and new structures are either rolled back or safely orphaned and eventually reclaimed.
- **INV-11** Erase/tombstone handling never prematurely terminates a valid linear-probe chain.
- **INV-12** Published index root and relevant secondary index root belong to same logical state (after Part C).
- **INV-13** A specific Snapshot always resolves all lookups through its original root; it cannot silently switch to current state.
- **INV-14** Every Reclaimer pass frees ≤ configured count of retired versions; additionally cap scan/destruction work for latency targets.
- **INV-15** No `Chunk*` or `Entry*` escapes publicly; results are copies in caller buffers.

---

## 8. Implementation staging and Git checkpoints

### 8.1 Repository structure (proposed)

```text
nova-kv/
  CMakeLists.txt
  README.md
  docs/
    BLUEPRINT.md
    assignment_contract.md
    decisions.md
    isolation.md
    reclamation.md
    wal.md
  include/
    kvstore.h                # public Appendix A API
  src/
    kvstore.cpp              # public façade and dispatch
    hash_segment.{h,cpp}
    version_root.{h,cpp}
    arena.{h,cpp}
    chunk_directory.{h,cpp}
    epoch_manager.{h,cpp}
    reclaimer.{h,cpp}
    transaction.cpp
    secondary_index.{h,cpp}
    join.cpp
    wal.{h,cpp}
  supplied/
    workload_gen.h           # exact instructor file, when provided
    workload_gen.cpp         # unchanged supplied original
  drivers/
    single_thread.cpp
    global_lock.cpp
    rwlock.cpp
    rcu.cpp
  benchmarks/
    layout_bench.cpp
    concurrent_bench.cpp
    reclamation_bench.cpp
    txn_bench.cpp
    join_bench.cpp
    latency_histogram.cpp
  tests/
    kvstore_test.cpp
    hash_segment_test.cpp
    arena_test.cpp
    ebr_test.cpp
    rcu_stress_test.cpp
    snapshot_test.cpp
    txn_test.cpp
    join_test.cpp
    wal_recovery_test.cpp
  scripts/
    run_matrix.sh
    perf_capture.sh
    make_flamegraphs.sh
    aggregate_results.py
    verify_submission.sh
  results/
    raw/                    # timestamped CSV/JSON/raw perf
    figures/                # SVG/PNG charts and flame graphs
  reports/
    part_a_report.md
    part_b_readme.md
    part_c_design.md
    part_d_report.md
    part_e_note.md
```

For submission, provide any mandated top-level `kvstore.h/.cpp`, `kvstore_test.cpp`, and driver names, via simple forwarding files or packaging scripts if source is organized in subdirectories. Exact user-provided `workload_gen.h` not uploaded in conversation: recreate only from PDF appendix for local prototyping; mark its provenance and later replace with instructor original **unchanged** when available.

### 8.2 Milestone A0 — build, CI, API and reference workload

**Implement:** CMake C++17 targets, public API, structural source directories, deterministic test runner, baseline build configuration, reference workload import. Verify no external concurrent hash-map dependency. Add debug sanitizer targets and release perf targets. Decide compatibility strategy for `Transaction` in supplied generator when compiling Part A: forward-complete lightweight Transaction declaration/implementation in compatibility header or use final header even for historical baseline; explicitly disclose. Check perf permissions and actual core count.

**Done when:** clean `cmake` build, smoke test and reproducible source-version manifest; record compiler/OS/CPU info. Git commit `chore/bootstrap`, tag `a0-bootstrap`.

### 8.3 Milestone A1 — correct generic baseline KVStore

**Implement:** generic byte ownership, flat or segmented hash-index baseline (same storage algorithm under three synchronization variants), `put/get/erase`; simple buffer error contract; tests: insert, overwrite, key collision, misses, empty values, too-small buffers, tombstone probe chains, segment growth, all-long negative keys, large values.

**Synchronizations:** single-thread (all calls serialized by driver); global `std::mutex` around operations; `std::shared_mutex` (`get` shared; `put/erase` unique). Same data structure and hash function all variants, only synchronization changes. Reference generator's writer/read split is documented; for actual active-writer comparisons, use concurrent harness. Do not introduce RCU yet; show the simpler data structure deliberately.

**Done when:** all baseline tests pass in each variant; no data races under concurrent legal global/RW modes; build reproducible. Tag `part-a-core`.

### 8.4 Milestone A2 — layouts and profiles

**Layout benchmark:** Use known Trade schema to compare NSM (whole Trade copied contiguously) with PAX or DSM (field arrays/page mini-columns) using identical logical records, same lookups, preinitialized dataset, compiler flags, and recorded cache effects. Generic API stays schema-agnostic; do not claim a generic byte-value KV engine knows columns. Record code, Ns/op, p50/p99 or RPS, cache misses, byte footprint. Argue point whole-row gets and upcoming multi-field join. Part A report ≤5 pages total, layout note ≤3 pages of that.

**Profile matrix:** global-lock and RW-lock each with 1/2/4/8 reader threads, writer actually active in custom harness. `perf stat` (elapsed, instructions, cycles, cache misses; IPC=instructions/cycles) and `perf record -g`, `perf script`, Brendan Gregg folded stacks + flamegraph SVG for all 8. Repeat ≥5 times, preserve raw files and median/spread. An aggregate cache-miss rate plus flame graph is suggestive, not conclusive evidence of cache-line bouncing; if available, add perf c2c/HITM or appropriate vendor coherence counters.

**Done when:** required driver source names, tests, data, images, ≤5-page report, and reproducible commands exist. Tag `part-a-final`; export baseline patch/history.

### 8.5 Milestone B1 — immutable root and COW directory, reclamation disabled

**Implement:** hash partitions, immutable published segment clones, private version builders, global stable chunk directory, append-only arena, root publication for individual `put/erase`; old versions retained intentionally. One atomic current load per `get`, no mutex in read path (but initial temporary never-reclaim test mode). Test transaction-like multi-key update through internal staging (public Transaction added later), verify readers see only fully built roots, no mutation of old segments. Use idempotent ownership builder rollback.

**Done when:** functionality and stress pass with no reclamation; record root-copy/segment-copy bytes per write and index lookup latency. Tag `part-b-publication`.

### 8.6 Milestone B2 — exact EBR and background cleanup

**Implement:** stable reader/snapshot slot pool, SC protocol above, reader guards, retirement queue, refcount ownership for directory→segments→chunks, background cleaner, `set_reclaim_batch`, safe chunk-ID reuse and fixed stable chunk-directory blocks. Make cleanup resumable and finite per pass. Add disabled and test-only STW reclaim modes for Part D. Add instrumentation counters without touching read hot path except optional compile-time profiling builds.

**Tests:** race injection, long-lived reader/snapshot, forced epoch shifts, rapid reuse, batch limit, end-of-program cleanup, sanitizer stress, no torn rows with active writer. EBR review gate requires concrete proof/tests; do not ship if uncertain about memory ordering.

**Done when:** all tests pass in release/debug/ASan/UBSan; EBR invariants reviewed; no locks taken by hot reads and no collector/writer wait. Tag `part-b-final`; export `part-a-final..part-b-final` diff and README.

### 8.7 Milestone C1 — multi-key transactions and explicit snapshots

**Implement:** standalone Transaction stage/commit/abort, duplicates/failure policy, immutable multi-segment root publication, Snapshot class owning movable pin-token, `acquire_snapshot`, snapshot-scoped `get`, `keys`. Add writer-commit versus reader-snapshot concurrency tests; explain guarantees as atomic single-writer commits + stable snapshot reads, and limits versus textbook SI/serializability and crash durability.

**Tests:** 1/4/16/100-key atomicity with invariant records, update different segments, fail after n-th allocation, abandon Transaction, empty commit, Snapshot held across many commits, move Snapshot, no freed chunk reuse while pinned. Tag `part-c-transactions`.

### 8.8 Milestone C2 — secondary index and compliant join

**Implement:** fixed-column field-byte secondary index atomically coupled to primary index root for each store, `create_index`, `index_get`, indexed value change/removal updates, keys enumeration; implement full join with exactly one coherent snapshot of the relevant database state (subject to cross-store clarification) and full inner-row fetch. Add separate proper join benchmark, not supplied generator's partial probe. If shared database context chosen, specify how two public stores are attached and how current versions publish globally without violating per-store API; if not feasible under allowed contract, document limitation and request instructor clarification rather than invent guarantee.

**Done when:** index correctness under updates/deletes, no torn join results under writer, report and diff. Tag `part-c-final`.

### 8.9 Milestone D — complete benchmark campaign

Run table in Section 9, archive raw samples/perf; produce plots; address no-reclaim and STW comparator; report 100k+ RPS success/failure honestly, p999 stalls, throughput/latency, bottlenecks. Tag `part-d-final`.

### 8.10 Milestone E — optional durability

**Implement after A–D stable:** framed WAL records with length, type/transaction ID, checksum, version or commit marker; append/log durability before success acknowledgment and before/as part of root publication, recovery stop at first incomplete/corrupted tail, deterministic replay of committed operations only. `fsync` commit acknowledgment/ batching policy explicitly specified; no WAL read-path operations. Crash injection between record header/body/footer, before/after fsync, before/after root publish, and during replay. The ordering of durable log and visible in-memory publish must be defined so acknowledged commits survive crash; a partially durable future record cannot be replayed as committed accidentally. Measure txn commit throughput and fsync counts. Tag `part-e-bonus`.

**Do not implement WAL or full arena compaction before the user reviews architecture**, unless deadline demands a scope cut.

### 8.11 EXTRA stages, separate from marks

`extra-compaction`: 80%-obsolete-byte live relocation; `extra-hot`: workload-aware hot-path experiments; `extra-inline`: inline small values; `extra-hier-directory`: hierarchical root copying; `extra-qsbr`: QSBR experiment; `extra-layout`: additional PAX/DSM use inside store; `extra-wal-batching`: group-commit tuning. Each one gets hypothesis → implementation branch → A/B benchmark → keep/revert decision, with no regression against core tests.

---

## 9. Benchmarking protocol and matrices

### 9.1 Two benchmark tracks

**Track R — supplied reference:** Link unmodified `workload_gen.cpp` against each supported store build. Defaults: `m=1000,n=100,hot_every=100,num_readers=8,duration_sec=5,use_txn=false,txn_size=1,use_join=false`. Label `reference / preload-only`. Record actual hit/miss ratio and no writer overlap. This is a compatibility/control benchmark and **does not alone satisfy** the active-writer experiment prose.

**Track C — actual concurrency:** Own `concurrent_bench.cpp` with same schema, known present-key distribution, optional skew, configurable mix, seed, record count, read threads, writer update rate, duration, writes/txn, and writer placement. Warm-load records before timed phase; start readers and **one writer simultaneously** using barrier; measure from coordinated `start` to `end` using `steady_clock`; writer continuously updates existing keys (and optionally inserts/deletes in separate scenario). Record actual elapsed wall-clock instead of sleep duration alone. Use independent per-thread latency reservoirs/vectors to avoid shared measurement locks. Writer does not hold a benchmark mutex on read hot path. Report read hit/miss counts, write attempts/commits, error counts, bytes allocated/reclaimed, queue backlog, and record CPU affinity.

### 9.2 Core matrix

| Scenario | Parameters | Primary outputs |
|---|---|---|
| Reference point read | supplied default and each mode | RPS,p50,p99,p999; explicitly preload-only |
| Baseline contended | global mutex, RW mutex × 1/2/4/8 readers + 1 writer | RPS,p50,p99,p999, perf, flamegraphs |
| Single-thread | one caller thread, no concurrent writer, unless serialized writer+reader on same thread | Comparable per-operation costs; label appropriately |
| RCU scaling | 1/2/4/8 readers + 1 writer | sustained RPS, latency percentiles, meets 100k? |
| Reclaim tuning | 8/64/256/1024 plus disabled and STW | read RPS, maximum observed per-read stall, p999, backlog, memory |
| Write amplification | 1/4/16 keys, grouped same segment vs spread segments | commits/s, commit p50/p99, bytes segments copied |
| Join | 8 readers, writer active vs paused | full joins/s, p50/p99/p999, result consistency |
| Value sizes | 0/8/16/32/64/256/4K/oversize | lookup/copy latency, memory efficiency |
| Hot skew | 100% one key, 80% 1k hot, uniform successes, misses | per-distribution throughput/tails |
| Scale | 10k/100k/1M keys, 1/2/4/8 readers | read scalability, root-copy/segment-copy cost |
| Chunk pressure | update same key, update broad working set, hold snapshot | backlog, retained memory, safe ID reuse |

### 9.3 Repeatability and measurements

- Release build: `-std=c++17 -O3 -g -pthread`, identical compiler for all modes; record compiler/version, architecture, kernel, RAM, clock/power governor, core topology and NUMA information.
- Run at least 5 independent process repetitions per configuration; preserve all raw samples or histograms with max. Report medians and dispersion, not only peak performance.
- CPU affinity via `taskset`/OS tool when possible; consistent reader/writer/cleaner placement. Compare runs with cleaner on shared versus separate core if useful; log context switches and migrations.
- Warm up at least one workload iteration; segregate loading and timed phases. Acknowledge per-call clock overhead and RNG work outside measured get window; measure both end-to-end RPS and operation latency.
- `perf stat -e cycles,instructions,cache-references,cache-misses,context-switches,task-clock` (available-event syntax varies by machine); compute IPC. `perf record -g -- ./program`, then `perf script`, stackcollapse and flamegraph. Capture failures such as `perf_event_paranoid` permissions and document alternatives. For proof of cache-to-cache ping-pong prefer `perf c2c`/HITM if supported; do not claim proof from flame graph alone.
- Capture p50,p99,p999 and **maximum** observed read latency, raw count, RPS, writer operations/s; calculate from sorted per-thread samples with documented percentile estimator; avoid mixing microseconds and nanoseconds. Report latency distribution when plotting pause bound.
- For Section D.2 STW control, clearly state its artificial pauses and do not treat it as production correctness strategy. Compare memory consumption for no reclaim, because unlimited old versions can invalidate longer tests.
- RPS target: count **completed read `get()` operations** only, not writer commits or combined operations. Confirm 8-reader sustained rate ≥100,000 or report honest shortfall and evidence.

### 9.4 Suggested CSV schema

```csv
run_id,git_sha,variant,scenario,readers,writers,cleaner,reclaim_batch,chunk_bytes,segment_capacity,record_count,seconds_actual,read_ops,read_hits,read_misses,read_rps,write_commits,commit_rps,p50_us,p99_us,p999_us,max_us,bytes_copied_index,arena_bytes_live,arena_bytes_retained,retirement_queue_max,notes
```

Raw latency events/histogram: `run_id,thread_id,operation,latency_ns,success`; profile files name run ID + config. The aggregate script must not silently discard outliers or unsuccessful reads.

---

## 10. Testing plan and acceptance gates

### Functional tests

- `put`, `get`, overwrite, missing key, erase, reinsert tombstone, collisions, linear-probe wraparound, duplicates, negative and extreme long keys, zero length, too-small buffer, oversized value, segment growth, tombstone rebuild, failed allocation; compare to simple std::unordered_map **in tests only** as oracle.
- Deterministic hash partition distribution tests; confirm each key hashes to exactly one partition and lookup probes within it.
- Arena: chunk boundary fill, oversized chunk, seal, stable address while appending, overflow checks, null directory slot after reclaim, ID reuse only after safe release; `ValueRef` range and generation debug checks.
- Internal count invariants: new directory adds exactly one segment owner per entry; retired directory decrements same; segment's chunk-pin set correct after clone/rebuild; no double free.

### Concurrency tests

- One writer updates a record with `generation, complement, checksum, repeated payload` and N readers assert no torn record. Use checksums / redundant fields, not only successful lookup.
- Multi-key transaction across at least 2–8 segments; reader pins one Snapshot and verifies invariant across keys while writer commits rapidly.
- Reader acquires V1, writer publishes V2..V1000, V1 remains readable; after pin released, reclaimed resources become eligible.
- Force interleavings at epoch read, slot announce, epoch validate, current root read, retire queue, cleaner scan, reader exit, directory slot removal, ID reuse. Supply hooks in test-only builds, not hot release path.
- Failpoint on each allocation/publication preparation step; published root pointer must remain unchanged if commit fails; no leak beyond documented orphan cleanup.
- Run tests under ASan/UBSan; TSan can report lock-free algorithm subtleties and is not a substitute for C++ memory-model audit. Use long stress at multiple CPU core counts.

### Acceptance criteria by stage

- A: baseline API semantics and tests; required drivers, comparative storage layout numbers, profiles/reports.
- B: exactly one current root load per get after protection; one atomic root publication per update; readers perform no mutex locking and no allocation; EBR no UAF under stress; batch cap respected; Part A diff + README.
- C: transaction visibility all-or-none for snapshot readers; move-only snapshots work; secondary index tracks changes; join's claimed snapshot scope actually matches implementation; diff/report.
- D: full matrix executed, raw data archived, percentiles and max, scaling interpretation, exact 100k RPS check.
- E: restart recovery exactly reproduces durable committed state under crash tests; never adds per-read WAL work.

---

## 11. Explicit remaining design review checkpoints

This blueprint is detailed but must not pretend its remaining external ambiguities are solved. The coding agent should **never silently choose a conflicting behavior** on the items below:

1. **Cross-store join API:** Ask instructor whether one shared snapshot is expected across two independent `KVStore`s or whether an internal shared database context is intended. Until resolved, build per-store correct indexes and use an engineering-only coordinator for full join experiments; do not claim exact API fidelity for an invented coordinator.
2. **Workload files:** The actual supplied `.h` was described in PDF but not uploaded; verify all declarations against original when available. Preserve `.cpp` unchanged.
3. **Reader slot lifetime/mobility:** Implement Snapshot-owned stable slot tokens, with sequential cross-thread transfer supported; fixed capacity in v1. A slot must not be reused while cleaner scans it as active or while any snapshot still owns it. Code review guard.
4. **Epoch safety:** Before implementing physical free, validate the full SC ordering argument and slot-registry lifetime. Use conservative fallback to no-reclaim if proof/testing fails, but do not mark Part B complete until reclamation is correct.
5. **Strict nonblocking writer under memory pressure:** Physical memory is finite and an inactive cleaner or pinned reader can indefinitely retain allocations. Document fail-fast behavior; do not claim unconditional lock-free writer progress under OOM.
6. **Index and arena thresholds:** All chosen numbers are initial hypotheses. Keep tunable through test configs, benchmark them; never confuse `set_reclaim_batch` with hash tombstone or arena compaction thresholds.
7. **Isolation claim:** Use accurate wording; single-writer atomic commits + pinned snapshots are established. Transactional snapshot isolation with concurrent read-write transaction validation is not provided by stated API. Explain write skew hypothetically, not as a demonstrated anomaly of this specific single-writer API.
8. **Compilation/driver configuration:** Keep `KVStore` common implementation; synchronization policy chosen by build target or injected private policy while preserving user-facing API. Build all drivers against consistent layout and workload settings.

---

## 12. Coding-agent work contract

**Use this prompt at the start of each coding-model session:**

> Implement the next named milestone from `docs/BLUEPRINT.md`, preserving assignment-mandated C++17 API signatures and the supplied `workload_gen.cpp` unchanged. Do not rewrite the architecture. State which requirements and invariants the changes touch. Write tests before or with implementation. Compile debug, release and available sanitizer targets, execute unit tests, and report exact commands/results. No performance claims without measured raw data. Commit only when the milestone acceptance gate passes. If an item marked OPEN / INSTRUCTOR blocks a correct implementation, isolate it, state the blocker, and do not invent compatibility or guarantees. Keep all published objects immutable, all multi-key writes single-root-published, and never physically reclaim objects unless the defined EBR and ownership protocols prove them unreachable.

**Recommended delivery for each milestone:** (1) source diff, (2) tests and commands, (3) benchmark/raw data when applicable, (4) file-by-file change inventory, (5) invariants checked, (6) unresolved risks, (7) Git tag/commit SHA, (8) instructions to reproduce. Use small, reviewable commits; no giant one-shot code dump.

Suggested tags: `a0-bootstrap`, `part-a-core`, `part-a-final`, `part-b-publication`, `part-b-final`, `part-c-transactions`, `part-c-final`, `part-d-final`, `part-e-bonus`. Export `git diff part-a-final part-b-final` and `git diff part-b-final part-c-final` to satisfy specified submission diffs. Final archive filename must follow `Asgn2_<RollNumber1>_<RollNumber2>[_<RollNumber3>][_<RollNumber4>].zip`.

### Explicit implementation order / don't skip

`API + tests` → `baseline layout/hash` → `Part A benchmarks` → `version roots with reclamation DISABLED` → `arena/chunk ownership` → `EBR proof and stress` → `bounded cleaner` → `Transaction/Snapshot` → `secondary index/full join` → `Part D comprehensive benchmarks` → `WAL optional` → `compaction/hot/inlining extras`.

Do not enable arena chunk reuse before EBR and segment chunk-pinning invariants pass. Do not add per-read hot-key usage writes before baseline throughput is recorded. Do not let cleaner publish database roots. Do not confuse “a value is absent from the newest root” with “no live snapshot can access its bytes.”

---

## 13. Short glossary

- **Key map:** Hash index mapping `long` keys to ValueRefs.
- **ValueRef:** `(chunk_id, offset, length)` location of immutable value bytes.
- **Hash segment:** One flat open-addressed array owning a subset of the hash space.
- **Index directory:** Immutable per-Version array of hash segment pointers.
- **Chunk directory:** Global stable chunk-ID-to-address lookup, independent from the index directory.
- **Version root:** Single immutable object atomically published to select complete index state.
- **Snapshot:** Move-only handle pinning one Version across multiple lookups.
- **EBR:** Reader epoch announcements + grace-period logic that delays freeing old versions until safe.
- **Commit sequence:** Number of database commits; different from reclamation epoch.
- **Tombstone:** Deleted bucket marker that preserves linear-probe search chains.
- **Sealed chunk:** Chunk that accepts no more appends; it may still contain current live values.
- **Retired root:** Previous Version no longer current, but potentially in use by old readers.
- **Reclaim:** Physically free unreachable memory after lifetime-safety proof.
- **Compact:** Copy remaining live bytes elsewhere to recover space in a fragmented chunk, then publish updated references and eventually reclaim the old chunk.
- **NSM / DSM / PAX:** Row-major / column-major / page-hybrid layouts of actual record fields, distinct from separating key index and value arena.
- **WAL:** Write-ahead log for process-crash recovery, optional Part E.

---

## 14. Review checklist before coding

- [ ] Accept or tune initial directory partition count, segment capacity, arena chunk size, load and tombstone thresholds.
- [ ] Confirm public buffer-error and empty-transaction semantics.
- [ ] Confirm Snapshot movable pin token and fixed-registration capacity strategy.
- [ ] Review and test EBR SC ordering + slot registry proof before allowing any free.
- [ ] Confirm internal segment and chunk owner-count invariants and destruction work budgeting.
- [ ] Confirm chunk-ID reuse after structural release, not merely after N generations.
- [ ] Keep instructor `workload_gen.cpp` untouched; verify the Appendix A.2 header transcribed from the PDF.
- [ ] Resolve or clearly bracket cross-store join inconsistency.
- [ ] Confirm tooling availability (`perf`, FlameGraph scripts, compiler, sanitizer support).
- [ ] Preserve Part A baseline implementation and Part B/C diffs via Git tags.
- [ ] Confirm extra features cannot block assignment milestones.

**Bottom line:** A safe RCU reader does not need a per-value atomic increment. It announces a protected epoch, reads one published root, follows immutable segment→chunk references and copies bytes out. The writer constructs changed segments privately, publishes one new root, and retires old structure. Cleaner can reuse chunk IDs only after epoch safety plus structural ownership prove the old allocation unreachable. All extra optimizations must preserve these invariants and be justified with measurements.


---

## 15. Non-negotiable implementation safety details (revision 2)

The following supplement clarifies places where pseudocode in earlier sections is not a complete proof. Implement these rules before enabling physical reclamation.

### 15.1 Publication and commit failure contract

- Any reported successful `put`, `erase`, or `Transaction::commit` has one precise **linearization point**: replacement of the `current` version root. Its full immutable directory and reachable segments and bytes must be initialized before that point.
- The commit must have enough **pre-reserved retirement capacity** before replacing the root. After publication there can be no allocating, exception-throwing, fallible transfer of the old root into the retirement machinery. If the retirement record is not yet visible to cleaner, the writer must still retain it in a permanently reachable writer-owned pending slot until the queue publication succeeds; the cleaner must not reclaim it based on its own observations.
- Staged `Transaction::put` copies the supplied byte buffer at staging time; it cannot retain application pointers across calls. All supported error paths preserve the prior published root. Partial private allocations are rollback-owned by a staging context, not externally visible.
- A commit's update set is grouped by partition; one private clone per modified segment, one new immutable directory, one root publication. If operations on a key are repeated, the final operation in staging order wins.
- No operation may invoke a callback into client code while holding internal resource state. Public `get` copies bytes before leaving its read guard.

### 15.2 Slot, epoch and read-side protocol contract

- Ordinary `get()` has one cached, thread-affine participant slot per `(thread, store)` with exclusive use while a read is active. The initial implementation **does not permit overlapping operations to share one participant slot**; explicit snapshots own separate slots. This restriction is internal, not a restriction on multiple independent thread readers.
- Slots occupy stable storage for the store's lifetime. Use a fixed configured registry capacity in v1. The collector must scan every slot that can participate. Slot registration/recycling and reclamation scans must have a synchronized lifecycle; no unscanned slot may begin acquiring a root.
- Use the validated enter loop in §5.2 with `seq_cst` for global epoch, participant announcement and current-root operations. While a participant is pinned, it must not refresh its protection epoch to a newer one while continuing to use an older root. On exit, publish INACTIVE with SC ordering.
- A Snapshot owns one stable participant token for its whole lifetime, including between individual snapshot-scoped reads. Sequential cross-thread move transfers token **without unpinning**. Its destructor is the final unpin; the token must not be reclaimed until no scan can access its slot. Concurrent use/move/destruction of the **same handle** is unsupported.
- Candidate reclamation must pass both an epoch grace-period test and structural ownership checks. Merely reaching an age of 30 epochs or commit generations is **never** sufficient to free a root or chunk. An optional age threshold is a scheduling delay only.
- Implement a model-oriented deterministic test for each enter/advance/publish/retire/scan interleaving (including one reader parked for thousands of commits). If a protocol proof cannot be defended in C++ memory-model terms, physical freeing stays disabled and the Part B milestone remains **FAILED**, not falsely completed.

### 15.3 Reclamation budgets and ownership graph

- A version is *fully destroyed* only after the corresponding directory reference and all ownership-release work triggered by it is complete. Track partially dismantled versions as cleanup jobs; don't pretend the retired version is freed while ownership work remains.
- `set_reclaim_batch(n)` controls **number of fully destroyed retired versions per pass**, as requested. Additionally impose explicit bounded work limits on scanned retirement entries, segment-reference decrements, chunk-pin decrements and allocator destructions per worker iteration. A single freed root can otherwise cause an unbounded cascade.
- Reclamation-disabled measurement mode must preserve correct publication/read semantics and intentionally leak retired versions only for the bounded measurement. Stop-the-world mode is for isolated comparison only, never the production read path.
- The cleaner never changes `current`. Compaction is a request submitted to the single writer; relocation publication and validation are writer-owned operations.
- A segment pins each **distinct chunk** referenced by any occupied entry, once. A version's directory owns segment references. Only the last segment owner can retire a segment, and only the last chunk pin plus absence of allocation/staging ownership can retire a chunk. These are reference/ownership transitions, **not reader-side per-value atomics**.
- A global directory slot for a reused chunk ID must be set to null **only after** all old references have been proven unreachable. Do not publish a different chunk at that slot until the old allocation and any old ID's structural references are gone. A generation number is an optional debug aid, not a substitute for these rules.

### 15.4 Resource exhaustion and operational boundaries

- `put` / commit can fail cleanly if index, chunk directory, retired-node pool, memory allocation, or reader/snapshot registration capacity is exhausted; it must not publish partial work. An explicit Snapshot allocation failure needs a consistent error behavior implemented and tested, notwithstanding the appendix's omission of an error return from `acquire_snapshot()`; document the policy and never return an unprotected valid-looking handle.
- Do not claim a bounded maximum *wall-clock* reclamation pause from a batch count alone. Bound units of work and measure actual tail latency, including allocator and scheduler interference.
- Shutdown precondition: application has joined all public reader/writer threads and destroyed all Snapshot handles. Then stop/join cleaner and release the entire ownership graph; assertions and leak checking should prove all owned resources are released.

### 15.5 Source compatibility and the two-table join blocker

- The PDF itself supplies the full `workload_gen.h` in Appendix A.2 (page 10). Transcribe that printed header exactly into `workload_gen.h` if no standalone header was uploaded, documenting PDF provenance. Do not invent extra fields or pretend a separate file was uploaded. Keep the supplied `.cpp` byte-for-byte unchanged and retain its SHA-256.
- The supplied `.cpp` unconditionally requires a complete `Transaction` type. Build a compatible temporary declaration/implementation for Part A or use a final-compatible header; document whichever approach is chosen. Avoid modifying the reference implementation itself.
- Cross-instance one-Snapshot consistency **cannot be guaranteed by independent current-root pointers**. For the proper Part C join, prioritize a shared internal `DatabaseState` root owning both logical tables (with a clearly documented way of associating public handles) if permitted by the public API; otherwise implement exact per-store semantics, keep join guarantee explicitly unresolved, and ask for instructor clarification. Never print 'full snapshot-consistent join passed' from two independently acquired snapshots.
- Treat the assignment's isolation wording accurately: atomic single-writer commits plus pinned snapshot reads are implemented; full textbook transactional snapshot isolation is not implied without transactional read/write conflict semantics.

## 16. Mandatory agent execution and review protocol

### 16.1 One-milestone-at-a-time workflow

The agent must not autonomously implement all Parts A–E in one undifferentiated pass. Its normal unit of work is one **milestone** from §8 (A0, A1, A2, B1, B2, C1, C2, D, E). For each milestone:

1. Read assignment/API constraints and relevant blueprint sections; print a concise **scope contract** (files to change, requirements touched, tests to add, non-goals).
2. Inspect current repository, git status, compiler, CMake targets, hardware and available profiler/sanitizers. Never assert a command ran unless its exit status and output are captured.
3. Implement smallest reviewable changes. Build and run unit tests after each logical subsystem. Preserve provenance and version-control original workload files.
4. Run the milestone's required **functional, stress, sanitizer and/or benchmark gates**. Save logs to `results/<milestone>/` with commands and environment metadata.
5. Generate a structured milestone report following §16.3; mark every gate `PASS`, `FAIL`, or `NOT RUN`, with evidence paths. Never convert `NOT RUN` to `PASS`.
6. Check `git diff --check`, inspect the diff, commit/tag only when required gates pass. Do not amend or erase earlier Part A checkpoint history.
7. Present the report and request the user's approval before starting the next major milestone. The agent may fix failed gates within the current milestone without restarting approval.

### 16.2 Precise pass/fail gates

| Gate | PASS condition | FAIL condition |
|---|---|---|
| API compatibility | All mandatory Appendix A signatures compile with both reference and project drivers; no reference file modified | Missing/incompatible method, changed reference file |
| Functional semantics | Edge tests, collisions, overwrite, erase, buffer policy, allocation-failure rollback all pass | Any incorrect return/copy, partial publication, lost key |
| Race/memory safety | Targeted interleaving tests + debug assertions + applicable ASan/UBSan stress pass | UAF, double free, invalid chunk reuse, torn row, deadlock |
| RCU hot-read behavior | One current-root load after reader protection; no writer mutex or waiting in `get`; no read-side allocation in measured steady state | Shared mutex, per-value refcount, unsafe root acquisition |
| Snapshot and atomicity | One pinned root reused; multi-segment commit observed all-old or all-new under one Snapshot | Inconsistent mixed state within a snapshot |
| Reclamation bounds | Each worker pass honors version cap and independent work-budget counters; long snapshot stalls reclamation, not reading or publication | Frees a pinned allocation or unbounded sweep |
| Benchmark validity | Active-writer harness actually overlaps writer/readers; every measured number has raw data and elapsed-time basis | Preload-only data misrepresented as concurrent |
| Part A evidence | 8 perf configurations, required flamegraphs, actual ≥2-layout comparison, report constraints | Incomplete raw data or theoretical-only layout justification |
| Part D evidence | 1/2/4/8 scaling; p50/p99/p999; batch/reference comparisons; join and commit figures | Fabricated RPS, absent max stall, missing raw data |
| Source delivery | Reproducible clean build, tagged checkpoints, patch artifacts and README | Untracked changed source, missing provenance |

The 100k+ read-RPS goal is a **measurement target, not a license to invent success**. If performance is lower, record measured RPS, profile the bottleneck, and document the result per assignment instructions.

### 16.3 Mandatory milestone report format

Each `reports/milestones/<id>.md` must contain these exact headings:

```text
# Milestone <ID> — <Name>
## Scope completed
## Requirements covered (PDF section/page)
## Changed files and why
## Design decisions and invariants
## Build commands and exact exit statuses
## Tests run (PASS/FAIL/NOT RUN)
## Sanitizer / race checks (PASS/FAIL/NOT RUN)
## Benchmarks and measured environment (if applicable)
## Raw evidence paths and SHA-256 hashes
## Profiling bottlenecks and next hypotheses
## Known limitations and source ambiguities
## Git commit SHA, tag, diff summary
## Gate verdict: PASS / FAIL / BLOCKED
## Next milestone proposed (not started)
```

An example reporting line is `ASan stress: NOT RUN — ASan compiler unavailable`, never `verified safe`. `perf stat: FAIL — perf_event_paranoid restriction` is an environment blocker requiring a recorded alternative plan, not fictitious counters.

### 16.4 Hard rules for benchmarking

- Record machine model, CPU/core count, kernel, compiler and flags, affinity, governor/turbo if known, dataset size, random seed, read/write ratio, duration, and actual read/write overlap.
- Preserve unmodified reference-generator results as **preload-only**. Its random-branch key-space mismatch produces mostly misses with defaults; record hits/misses in supplementary testing rather than rewriting the reference file.
- For each benchmark config, perform a warm-up; use ≥5 independent measured repetitions where feasible; report raw values, median, and variability. Latency percentiles derive from actual per-operation samples; max reader latency/stall is recorded separately.
- Measure writer commits/second independently of reads/second. For STW baseline isolate deliberate stop-the-world pause costs and clearly label it as a noncompliant comparator.
- Save raw `perf stat`, `perf record`, `perf script`, folded stacks, flamegraph SVG, latency/throughput CSV and manifest. A flame graph alone does not establish cache-line bouncing: record coherence-specific counters if available or qualify conclusions.
- Never tune based solely on an average. Inspect p99/p999, max observed latency, memory growth, segment-copy bytes, and cleaner backlog.

## 17. Reviewer quick-start (human operator)

At each handoff, review **the diff**, not just the agent's prose. Compare logs to its declared commands; reproduce at least one clean build and the feature's smallest adversarial test. Inspect hot-path source for unintended lock operations or allocations. Check that any claimed RCU safety involves an actual epoch-protection protocol, not a naked pointer load. Confirm every retired allocation was linked into ownership bookkeeping before it can disappear from writer state.

Stop an agent and request corrections immediately if it: changes the supplied reference generator, weakens a memory-order guarantee without a proof, introduces an in-place write to a published segment/value, frees a chunk based solely on the latest key map or a generation-age threshold, calls two independent snapshots a single cross-store snapshot, or reports performance without raw measurements.

After each accepted milestone, create the tag in the current repository and preserve `reports/milestones/<id>.md` and `results/<id>/`. The associated independent audit checklist is in `NOVA_KV_Verification_Runbook.md`.
