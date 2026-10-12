# NOVA-KV: EBR Safety Argument and Coding-Agent Audit

**Status:** Design-level safety argument under stated assumptions; **not** a machine-checked proof or verification of code that has not been written. **Binding supplement** to `NOVA_KV_Final_Implementation_Blueprint.md`. If they differ, use this document for EBR gating and audit requirements. October 2026.

## 1. The precise safety property

For any reader R, once R has successfully pinned a published Version V and until it unpins, neither V nor any immutable Directory, Segment, Chunk object or value bytes reachable through V may be destroyed, overwritten, reassigned to another chunk ID, or relocated without preserving the old bytes.

A newer Version may publish concurrently. This property applies equally to a normal `get()` (until it finishes `memcpy`) and a move-only `Snapshot` (until its last owning handle is destroyed).

**Not claimed:** EBR prevents indefinitely growing memory, proves bounded total destruction time, makes arbitrary raw-pointer refcounts safe, or supplies atomic snapshots spanning two independent KVStore instances.

## 2. Required implementation assumptions (ALL mandatory)

1. Exactly one application writer publishes each KVStore's `current` Version and retires the preceding Version; one cleaner advances epochs and processes retirement. All atomic state transitions used in EBR, current publication, and retirement use **`memory_order_seq_cst` initially**.
2. Reader slots are **registered and permanently enumerable before readers can enter**. For v1 use a fixed, stable registry of slots, initialize them INACTIVE before worker startup, and do not add/remove slots while operations or cleaner scans may run. Dedicated Snapshot slots are allocated/reserved in the same stable registry. A reader may not enter if registration fails. No unregistered read may dereference any version.
3. Each live slot is used by **at most one active read critical section** at a time; no nested use of the same slot, unless a rigorously implemented depth counter is added. A Snapshot's slot stays active across moves; sequential cross-thread moves transfer ownership without unpinning. The user must synchronize cross-thread access to a Snapshot handle.
4. The reader executes the entry-validation protocol below, and dereferences neither Version nor referenced memory before successful validation. The reader's sole `current` load occurs **after** successful validation.
5. All objects reachable from a published root are fully initialized before the publishing SC `exchange` and become immutable while reachable. The writer does not mutate an already published Segment or already published arena bytes.
6. The writer samples retirement epoch **after** its atomic root replacement. It publishes the retirement descriptor without losing ownership in the gap between replacement and queue publication. The cleaner can only free a descriptor that it has received, and never frees `current`.
7. The cleaner performs a complete scan of every registered slot, then attempts an SC compare-exchange of `global_epoch` from the epoch it scanned. It advances by at most one per successful scan. **If ANY active slot does not announce the scanned epoch, advancement fails.** No arbitrary time-/commit-count-based freedom to reclaim.
8. A descriptor tagged with epoch `r` is eligible for physical destruction only after **at least two successful gated advances beyond `r`** (and only once it is actually retired and visible to cleaner). Destruction is subject to the explicit per-pass budgets and structural ownership rules.
9. Epoch counter will not wrap during the supported deployment lifetime (fail fast before overflow); chunks/segments remain alive while any reachable retained root can refer to them. Chunk-ID reuse occurs only after all structural pins and allocation owners have been released, and after prior roots are safe to destroy.
10. No deallocation or destructor performs a hidden unbounded cascade in one bounded cleanup pass. Incremental destroy queues account for referenced segments/chunks separately from the number of root descriptors completed.

## 3. Precise algorithm (pseudocode, not stand-alone production code)

All lines explicitly marked `SC` are C++ sequentially consistent atomic operations. Pre-registered slots are stable throughout the active period.

```text
Reader pin(slot):
    loop:
        e = global_epoch.load(SC)
        slot.epoch.store(e, SC)     // announces active state, before reading current
        if global_epoch.load(SC) == e:
            break
        // still announced; retry with newer epoch; no root dereference yet
    v = current.load(SC)            // exactly one load of current
    return (slot, v)

Reader unpin(slot):
    slot.epoch.store(INACTIVE, SC)

Writer publish(next):
    // Complete all allocations; initialize/pin every reachable object;
    // reserve retirement descriptor so postpublication retirement cannot fail.
    old = current.exchange(next, SC)
    r = global_epoch.load(SC)       // deliberately AFTER exchange
    publish_retirement_descriptor(old, r, SC)

Cleaner advance():
    e = global_epoch.load(SC)
    for each registered stable slot:
        s = slot.epoch.load(SC)
        if s != INACTIVE and s != e: return NO_ADVANCE
    return global_epoch.compare_exchange_strong(e, e + 1, SC)

Cleaner eligible(record):
    return record is retired & recorded & current_epoch >= record.r + 2
           & ownership/lifetime prerequisites hold
```

Do not silently replace SC slot operations with relaxed/release/acquire: the proof below relies on a single SC order covering slot announcements, epoch increments and pointer publication. A compiler implementation still must uphold the C++ object-lifetime rules.

## 4. Proof sketch: why a vulnerable reader cannot be skipped

Let V be removed from `current` at **atomic exchange X**. Let R be any reader that might dereference V. Assume R has successfully executed `pin` and later loads V using its one atomic current load L. Necessarily **L is before X in the global SC order**, because after X (with only one writer and no republishing of V) the root is different. Let R's validated epoch be `e` and let A be R's SC announcement of `e`.

Since R's validation load E is after A and before L in the SC order, and validated `global_epoch==e`, R is pinned and announced in `e` before it can access V. By the time the writer tags the retired V with `r`, the tag load T is **after X**, so E precedes T. Epochs never decrease, hence **`r >= e`**.

Now consider an attempted epoch advance from `e` to `e+1` while R is still pinned:

- If the collector scans R's slot *after* A, it sees `e` or (if R already left) INACTIVE; it cannot observe an older inactive value after the SC announcement while R remains pinned. If it advances from `e`, R may stay pinned `e` through that **one** advancement.
- If the collector scans R's slot *before* A, it might miss R, but then its successful SC epoch-advance CAS either occurs before R's validation E, in which case R's validation cannot succeed for old `e` and R retries, **or** it occurs after E, in which case A happened before the scan (contradiction). Thus a reader that successfully enters cannot be missed by a relevant advancement.
- Once global epoch is `e+1`, R remains announced `e`; a collector scan cannot pass the gate to advance to `e+2` until R exits (or in a correctly supported protocol changes its announcement only after dropping all old references). New reader entries in later epochs do not invalidate this fact.

Therefore **while R could still use V, the epoch cannot reach `e+2` via properly gated advances**. Since retirement is tagged `r>=e`, it also cannot reach `r+2`. The cleaner's eligibility predicate is false; it cannot free V. When every such reader exits, gated epoch advances can eventually establish eligibility (progress depends on scheduling). This is a conditional invariant proof for the exact SC protocol—not a claim about any different implementation.

**Important about SC reasoning:** The ordering argument is over the SC atomic actions. The reader must not perform access to V before the validation and pointer load. The retirement record's lifetime and registration stability remain separate assumptions; breaking either invalidates the argument.

### Minimal adversarial schedules that MUST pass

- Reader announces old epoch, pauses before validating; cleaner advances; reader must retry before loading `current`.
- Reader validates and pauses before loading `current`; cleaner advances once, but must not advance again until reader unpins.
- Reader loads old root; writer exchanges root and gets paused before retirement queue insertion; cleaner must neither free nor lose the old root.
- Reader stays pinned across thousands of commits; retired root remains live, even if >30 commit versions old.
- After last old reader exits, cleaner advances twice as gated, then eventually destroys eligible roots within budget.
- Explicit Snapshot holds an old root while moved sequentially between threads; move never changes its slot announcement or version pointer.
- Last directory owner disappears and last chunk structural pin is dropped; ID may be reused only after old root and references are entirely dead; no old ValueRef can resolve reused slot.
- Full reader registry or retirement queue; no unregistered lookup, dropped retirement record, or partial root publication.

## 5. What this does NOT prove and required review

- It does not prove implementation correctness, absence of ABA from faulty structural owner counts, safety of a dynamically resized slot registry, or correctness of compaction.
- `seq_cst` operations are necessary to make the above reasoning straightforward but not automatically sufficient if any object/slot lifetime assumptions are violated.
- TSAN, ASAN/UBSAN, deterministic injected-pauses, and millions of stress iterations are supplemental evidence, not formal proof. Reviewer must inspect *all* atomic operations, pointer ownership transitions and return paths of C++ code against this argument.
- Reference: Crossbeam's design documents the gated epoch advancement and two-advance reclamation principle, but NOVA-KV's C++ protocol and integration need their own review: https://github.com/crossbeam-rs/rfcs/blob/master/text/2017-05-23-epoch-gc.md and https://docs.rs/crossbeam-epoch/latest/src/crossbeam_epoch/epoch.rs.html.

## 6. Independent full-handoff audit: corrections / non-negotiable gates

| ID | Risk found | Required coding-agent action / verification |
|---|---|---|
| H01 | Reader-slot registration may race with scans | Pre-register fixed slots, never resize or unregister during active use; explicit exhaustion behavior; assert each slot has one owner. |
| H02 | Reference workload includes `std::optional<Transaction>` even in Part A | Make compatibility-facing Transaction declaration/implementation available for linking the unchanged workload from day one, **while preserving Part A baseline semantics**; don't falsely claim Part A already implements Part C transactions. |
| H03 | Supplied workload bulk-loads before readers | Run supplied generator unchanged AND supplementary true concurrent writer/readers; label outputs. |
| H04 | API's snapshot qualification ambiguous in PDF text | Choose one source-compatible exposed type and compile a conformance test using exact Appendix A signatures; publish type alias only if strictly needed. |
| H05 | Two independent stores and one snapshot for join conflict | Two snapshots yield per-store consistency only; this **does not meet the literal exactly-one-handle condition**. Record noncompliance until clarified or fixed by a shared version context; do not mark Part C full PASS silently. |
| H06 | Root directory copy entails O(P) refcount changes on every publication | Benchmark root-copy bytes, segment-copy bytes, reference-count atomics, writer latency and reclaimer destruction cost; use bounded incremental destroy workflow. |
| H07 | Chunk ID may be reused before old references are gone | Enforce segment lifetime pins; maintain slot publication and cleared state via atomics; reuse only after final old ownership release; deterministic ABA stress. |
| H08 | Chunk directory fixed top-level eventually fills | Define capacity, reject growth exhaustion before publication, never resize reader-visible arrays unsafely. |
| H09 | A reader might not have a preallocated slot | Register before concurrency; do not allocate on steady-state `get`; fail safely on exhaustion; thread ID reuse must not overlap slot ownership. |
| H10 | `get()` success on zero-length values, invalid buffers, collision chains, erasure | Direct API tests; buffer not written on insufficient capacity; verify value copies before returning. |
| H11 | Recovery after failed append or transaction | Injection at every allocation and publication-preparation step; old root unchanged; future commits still succeed; no leaked ownership pins. |
| H12 | Writer retirement handoff fails after root exchange | Pre-reserve descriptor; test forced failure before exchange; prove retirement record never dropped. |
| H13 | Cleaner frees > batch or hidden work cascade | Separate budgets for versions, segment release and chunk release; full deletion of root counts only when complete; record max duration and work count. |
| H14 | Background cleaner / writer may modify same refcounts | Enumerate ownership transitions and use synchronized ownership updates; no increment-from-zero resurrecting freed segments; test allocator faults. |
| H15 | Reference generator's random keys are mostly absent | Preserve exactly; record miss rate; provide a second controlled uniform-hit benchmark. |
| H16 | Compiler/sanitizer performance comparisons | Never compare ASAN/TSAN benchmark numbers to production -O3; separate correct/sanitized/perf builds. |
| H17 | Single-thread variant vs PDF's 1/2/4/8 list | Run real single-thread baseline with one reader; label any concurrent wrapper implementation; don't call an unsynchronized multithread run valid. |
| H18 | Part A layout benchmark NSM vs DSM/PAX | Implement two physical representations of actual Trade records; provide measured whole-row point lookups including reconstruction; don't call key/value separation alone DSM. |
| H19 | Part C Transaction semantics/secondary index incomplete | Test atomicity under held snapshots, updates to indexed column, overwrite, erase, duplicate index matches and keys enumeration. |
| H20 | Part E crash ordering | Only acknowledge durable commit after WAL durable policy; checksum/length/commit marker and injected truncation; demonstrate replay; do not mislabel memory-only completion as durability. |
| H21 | Hardware/perf may be unavailable | Record CPU/core count, OS, permissions; mark perf gates NOT RUN rather than invent flame graphs. |
| H22 | Wrong file paths or instructor declarations | Extract header from Appendix A.2 and preserve supplied cpp byte-for-byte (checksum); compile/link conformance build from repository root. |
| H23 | Built-in `std::shared_ptr` on hot path | Prohibit unless proven truly lock-free and accepted as design change; reference-count only on writer/cleaner ownership operations. |
| H24 | C++ undefined behavior masked by logical correctness | Review object lifetime, atomics, bounds, overflows, padding, placement/memcpy, and reference invalidation; require sanitizer and race stress. |

## 7. Automated gates the agent must implement

In addition to the original runbook, create `scripts/verify_all.sh` that returns nonzero if a **mandatory** implemented milestone gate fails. It must run each applicable test target and record stdout/stderr, exit code, timestamp, compiler version, git SHA and build mode under `results/verification/<sha>/`. `NOT RUN` must appear explicitly and block full PASS for a mandatory test. The agent must not alter test expectations to conceal a failure.

Produce `reports/milestones/<ID>.md` for each stage with: *assignment clauses*, *code changes*, *tests*, *result artifacts*, *known gaps*, *go/no-go*. Every gate has objective assertions; e.g. old snapshot read always returns old value across writer commits; new snapshot returns new value; destructor counters stay zero while old reader pinned and increase after unpin; no root leak after cleanup drain.

**Human approval is still required** for a protocol-review gate and for any deviation from the professor's API. A blueprint cannot eliminate the need to inspect concurrency-sensitive code or experimentally verify throughput.
