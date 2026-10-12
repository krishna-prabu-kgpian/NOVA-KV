> **Mandatory addendum:** `NOVA_KV_Formal_Safety_and_Agent_Audit.md` contains the precise EBR proof assumptions, counterexample schedules, and H01–H24 audit gates. Run them before any milestone claims complete memory reclamation.

# NOVA-KV — Independent Verification and Execution Runbook

**Purpose:** Allow the project owner to verify a coding agent's work instead of trusting its narrative. Companion to `NOVA_KV_Final_Implementation_Blueprint.md`. This document is a verification plan; commands use expected project targets and may need path/target adjustments once the CMake files exist. A command printed here has **not** been executed against a future implementation.

## 1. Five questions to ask after every milestone

1. What exactly changed? Show `git diff --stat`, the meaningful diff and changed source files.
2. What property was proved or tested? Point to named tests, assertions, failure injection, and an actual command log.
3. How did the test attempt to break it? Request the shortest adversarial interleaving, not just a happy-path example.
4. What happened on real hardware? Show the actual raw benchmark and profiler files and configuration.
5. What is still unverified? Ask for a list of `FAIL`, `BLOCKED` and `NOT RUN` gates. A successful compilation is insufficient.

## 2. Initial setup and provenance

Suggested root checks:

```bash
pwd
find . -maxdepth 3 -type f | sort
git status --short
git log --oneline -5
sha256sum workload_gen.cpp
uname -a
lscpu
g++ --version
cmake --version
perf --version
```

Appendix A.2 of the PDF (page 10) provides the complete `workload_gen.h` text. If not supplied as a standalone file, transcribe it verbatim from the PDF and mark the file provenance accurately. Compare API signatures and defaults line by line with Appendix A.

## 3. Build and unit test gates

Expected CMake convention (coding agent should implement these named presets/targets or document their replacements):

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

Sanitizers (targets/options defined by the agent):

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_ASAN_UBSAN=ON
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DNOVA_ENABLE_TSAN=ON
cmake --build build/tsan -j
ctest --test-dir build/tsan --output-on-failure
```

TSan builds must be separate from ASan builds. Some environment/runtime combinations cannot run TSan; record NOT RUN/BLOCKED, don't infer race freedom.

## 4. Adversarial tests by subsystem

| Subsystem | Required scenario | Result that must hold |
|---|---|---|
| Hash table | Several keys collide; erase middle key; find later key | No broken probe chain |
| Hash table | Repeated overwrites, tombstones, resize | Exactly one latest mapping per key |
| Value arena | 0-byte, 1-byte, huge value, boundary fit | No bounds overrun; correct copied bytes |
| Transaction | 2+ modified keys in different segments | Pinned reader sees all old or all new |
| Transaction | Nth allocation fails | No new root publication; no partial visible write |
| RCU | Park reader after acquiring V1; publish V2..V10000 | V1 remains valid until reader exits |
| EBR | Pause reader between announce and current-root load | Reclaimer must never free pointer it can acquire |
| EBR | Force epoch bump during reader-entry loop | Entry retries safely without UAF |
| EBR | Pin reader in older epoch, repeatedly request advancement | Cleaner must not advance past this participant; two successful gated advances are required for retirement eligibility |
| EBR | Move Snapshot to another thread sequentially | Pin remains continuous |
| Shared segments | Destroy old root sharing many segments | Still-referenced segments remain allocated |
| Chunk ID recycle | Remove all refs, free chunk, reuse ID; hold earlier snapshot in variant | No incorrect chunk resolution or reuse while pinned |
| Reclaimer | Force queue near capacity and large ownership cascade | No lost retirement, no unbounded single pass |
| Part C secondary index | Indexed field overwritten; key erased | Index entries match primary data |
| Part C join | Writer commits during full join | Coherent version guarantee or explicitly documented unresolved API limitation |
| Part E WAL | Crash at each log write/fsync/publish boundary | Recovery includes only durably committed transactions |

For code review, require test-only scheduling hooks immediately before/after: epoch read, slot announce, validation, current load, current exchange, retirement enqueue, cleaner slot scan, root destroy, chunk-slot clear, ID reuse. Then enumerate controlled schedules. `sleep()` is not a deterministic race test.

## 5. Benchmark evidence matrix

| Assignment stage | Evidence files to require |
|---|---|
| A | 3 drivers; layout microbenchmark source/results; 8 lock profiling run configs, perf stat raw and perf-record profiles, 8 flamegraphs; ≤5-page report |
| B | A→B code diff, EBR design note, active writer torn-read stress log, batch cap instrumentation |
| C | B→C diff, transaction tests, snapshot-index/join tests, isolation/durability note |
| D.1 | modes × readers (1/2/4/8 where meaningful), raw per-run RPS/p50/p99/p999, active writer overlap, 100k check |
| D.2 | batches 8/64/256/1024, no reclaim and STW, max stall + RPS, retained bytes |
| D.3 | join active/paused writer, 1/4/16-key commit throughput, copy amplification |
| E bonus | crash recovery matrix, write performance with/without WAL, fsync/group-commit policy |

Every benchmark should record machine/core count, CPU affinity, kernel, compiler/flags, warm-up, sample duration, 5+ repeats when feasible, key distribution and hit rate, active writer commits, and reproducible random seed. Preserve samples for maximum latency; the reference `WorkloadResults` has no max field. `perf` may require system permissions.

## 6. Nonnegotiable evidence rules

- Do not accept read measurements with no concurrently running writer as evidence of lock contention or RCU reader/writer coexistence.
- Do not accept `100k+` target claims without run output tied to exact thread count, duration and machine.
- Do not accept a flamegraph-only claim of cache-line bouncing; seek coherence counters or qualify the finding.
- Do not accept 'epoch older than 30' as reclaim-safety reasoning.
- Do not accept a chunk deemed dead only because newest index does not reference it; older snapshots matter.
- Do not accept changes to the supplied `.cpp` or a fabricated supplied `.h`.
- Do not accept two separate store snapshots as one atomic cross-table snapshot.
- Do not accept 'no torn reads' as proof of no use-after-free; both properties need different tests.

## 7. A simple decision to approve each milestone

Write a one-page gate verdict:

```text
Milestone:
Date / machine / revision SHA:
Functional: PASS | FAIL | NOT RUN
Concurrency safety: PASS | FAIL | NOT RUN
Sanitizers: PASS | FAIL | NOT RUN
Performance data: PASS | FAIL | NOT RUN | N/A
Assignment artifacts: PASS | FAIL | NOT RUN
Known unresolved blockers:
Evidence paths:
Approved for next milestone? YES / NO
```

Approval criteria: required source and tests exist, no blocking correctness failures, raw logs support results, changes have a reviewable commit, and any unrun optional check is clearly disclosed. For EBR enablement, a memory-safety proof/audit and stress interleavings are mandatory, not optional.

## 8. Recommended owner checkpoints

- **After A1:** Personally inspect a collision + deletion case; verify three locking modes use the same data structure.
- **After A2:** Open at least two flamegraphs, read perf stat logs, inspect the NSM/PAX or NSM/DSM comparison program and raw data.
- **After B1:** Pause a reader on an old root while writer publishes; verify old segments and values are unchanged.
- **After B2:** Inspect EBR entry/exit and collector scan code line by line; run parked-reader and chunk reuse tests under sanitizers.
- **After C1:** Run a multi-segment atomic transaction test while one snapshot is held throughout.
- **After C2:** Demand an explicit cross-store consistency claim and demonstration; don't accept a partial index probe as a full join.
- **After D:** Reproduce one 8-reader active-writer configuration and verify RPS, p999, and max latency directly from raw samples.
- **After E:** Kill process at WAL fault points and run recovery verification.

## 9. Handoff request to send the coding agent

> Read the blueprint and your system prompt. Begin only Milestone A0. First report a repository inventory, required public APIs, source/reference discrepancies, missing dependencies, planned changes, exact tests/build gates, and anticipated blockers. Do not implement Part B or later yet. At completion, create the milestone report with evidence and stop for review.

## 10. EBR independent reference and audit condition

Model the advancement gate on Crossbeam epoch source: https://docs.rs/crate/crossbeam-epoch/latest/source/src/internal.rs and https://docs.rs/crossbeam-epoch/latest/src/crossbeam_epoch/epoch.rs.html . The cleaner may advance from E to E+1 only if all currently pinned participants report E; never advance blindly via `fetch_add` in the chosen classical protocol. A retired object becomes eligible after two valid advances (subject to safe registration and publication). A coding agent must provide an operation-by-operation C++ memory-model argument for entry, validation, publication, slot scan, advancement, and destruction, including reader pause/restart and Snapshot movement. This runbook does not certify an unimplemented algorithm.

**Cross-store correctness claim:** two separately pinned store snapshots suffice for independent, immutable versions in each table; they do not establish one coherent cross-store transaction-time snapshot if both stores can change. Benchmark the static Instrument case and document the stronger PDF requirement separately.
