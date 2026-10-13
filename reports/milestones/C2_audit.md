# Post-C2 source-level audit

Date: 2026-10-13. Branch `part-c/indexes-joins`. Audited: tag `part-c-final` (`27d3161`,
source as verified at `5d414bc`). A reading of the committed code against the PDF, the C2
report, `docs/secondary_index.md`, `docs/transactions.md`, `docs/reclamation.md`, the
blueprint and the formal-safety supplement. Tests are reported separately and are not the
argument.

**Verdict: C2 AUDIT PASS — READY FOR PART D.** No defect was found in the store or in the
join; no file under `src/`, `include/` or `benchmarks/` changed. The audit corrected one
statement of the C2 report (the supplied generator's probe), added a check for it, assessed
the API-coverage question (no gap), and prepared the Part D plan.

## Scope completed

1. Pushed `part-c/indexes-joins` and `part-c-final`; hashes compared with the remote.
2. Source audit: primary/secondary consistency, index ownership and reclamation, the join.
3. Option A documentation confirmed in all four places.
4. The supplied generator's eighth symbol byte: verified in the source and measured.
5. Part C API coverage on the lock variants: requirement-by-requirement assessment.
6. Part C deliverables checked as committed; diff compared with Git.
7. Full verification on the audit commit.
8. Part D plan prepared, not executed.

Not done, by instruction: any Part D measurement.

## Requirements covered (PDF section/page)

PDF pp. 4–5 (Part C), pp. 5–6 (Part D, for the plan), Appendix A.1–A.2.

## 1. Push

| Ref | Local | Remote (`git ls-remote origin`) |
|---|---|---|
| `refs/heads/part-c/indexes-joins` at push time | `27d316199e7bea9250d8e2839fa649e5c1f318d7` | same |
| `refs/tags/part-c-final` (tag object) | `481a7fc453ad227f8c25378a7b861c161a1ec7a5` | same |
| `part-c-final^{}` (commit) | `27d316199e7bea9250d8e2839fa649e5c1f318d7` | same |
| `refs/heads/main` | `a3cccd84dec0f1df3e522868f60f064986549df6` | same, unchanged |
| `part-a-final^{}` | `3444f23…` | same |
| `part-b-audited^{}` | `87d38aa…` | same |
| `part-c-transactions^{}` | `1e6af10…` | same |

Tree clean before pushing. All earlier tags and branches on the remote are as before
(`a0-bootstrap` 2ef5f19, `part-a-core` 80bc092, `part-b-publication` 2d9c02c, `part-b-final`
304a8ac; branches `part-a/*`, `part-b/rcu` 87d38aa, `part-c/transactions` c0144b7). Ordinary
pushes; nothing forced, merged or rewritten.

- https://github.com/krishna-prabu-kgpian/NOVA-KV/tree/part-c/indexes-joins
- https://github.com/krishna-prabu-kgpian/NOVA-KV/tree/part-c-final

The audit's commits (`21af184`, then this report) were pushed to the same branch afterwards
with an ordinary push; final hashes under "Git commit SHA" below. The tag was not touched.

## 2. Primary and secondary index consistency: findings from the code

| Property | Finding | Where |
|---|---|---|
| Every published root contains a matching primary and secondary index | There are exactly three writes of `current_`: construction (empty version, no index), destruction, and the one `exchange` in `publish_locked`. `publish_locked` is called from two places, `commit_locked` and `create_index`, and in both the `Version` passed in was completely assembled (primary directory, index definition, index directory) before the call | `versioned_store.cpp` |
| An indexed transaction is exactly one publication | `Transaction::commit` → `commit_staged` → one `commit` → one `publish_locked`; the index changes are built inside that same `commit_locked` | same |
| Failed updates publish nothing | of the 15 `return` statements in the body of `commit_locked`, three precede any allocation (argument checks), ten go through `fail()` (frees the delta array, every index segment built, the raw version and node, rolls the arena back), one is the no-change return (frees the delta array since D-066), one is success. None is after the "nothing below can fail" line except success | same |
| A changed field removes its old entry | before each operation the key's entry is looked up **in the private copy**; if its value is long enough, its field is hashed and compared; a differing new field emits `remove(old hash, key)` | `commit_locked`, phase 2 |
| A changed field adds its new entry | `add(new hash, key)`, ordered after the removal (`seq = 2i`, `2i + 1`), so that when both name the same pair (two values, one hash) the addition wins | same |
| Deleted keys disappear from the current index | an `erase` that took effect emits the removal; `index_merge` drops the pair | same; `secondary_index.cpp` |
| Historical snapshots keep the old index | a commit never modifies an `IndexSegment`: it builds replacements and shares the rest; `index_get_pinned` reads `pin.version->isegs` | same |
| Duplicate values return all matching keys | all pairs of one hash are contiguous; the lookup walks the whole run | `index_read` |
| Truncation and ordering | pairs are sorted by (hash, key) at build and merge; the lookup stops at `max_keys`; so: ascending, smallest first | same |
| Short values never cause out-of-bounds reads | the three places that form a field pointer (`old_field`, `new_field`, backfill) and the two that compare (`index_read`, `validate_index`) are each guarded by `length >= offset + len` on the same line or the line before; `create_index` rejects ranges beyond the value limit, so the sum cannot overflow | `versioned_store.cpp` |
| Embedded zero bytes | fields are hashed and compared with explicit lengths (`index_hash(ptr, len)`, `memcmp`); no string function | both files |
| Hash collisions cannot produce wrong matches | `index_read` returns a key only after `memcmp` of the probe with the field of that key's value **in the same version**; the hash selects candidates and nothing else | `index_read` |
| Index creation and backfill are atomic | pairs, version, node and every index segment are allocated and built before the "nothing below can fail" line; then owner counts and one `publish_locked`. `fail()` frees all of it | `create_index` |

Allocation-failure rollback, checked against the code rather than the tests: the delta array
is allocated before phase 2 and freed on every path (`fail()`, the no-change return, and
unconditionally after the merges); merged-pair scratch buffers are freed inside `index_merge`
on both outcomes; a replacement index segment is recorded in `iseg_replaced` only after it
was built successfully, and `fail()` destroys exactly those.

## 3. Index ownership and reclamation: findings from the code

`IndexSegment::owners` is written in five places, all found by `grep`:

| Site | Operation | Condition that makes it safe |
|---|---|---|
| `commit_locked`, new segment | `store(1)` | the segment is not yet reachable |
| `commit_locked`, shared segment | `fetch_add(1)` | taken from `base` = the current version, which holds a reference and is retired only by this commit's own `publish_locked`, later: never an increment from zero |
| `create_index`, new segment | `store(1)` | not yet reachable |
| `run_job` | `fetch_sub(1) == 1` → destroy | reached only for a version that became a job after the epoch test `[C6]` (or in the destructor) |
| `validate` | `load` | quiet store |

`index_destroy` has three call sites: `run_job` (above) and the two `fail()` paths, where the
segment was never published.

- **Shared index segments cannot be destroyed prematurely.** The count equals the number of
  versions whose entry `run_job` has not yet processed; a version's entries are processed only
  after its own `[C6]`.
- **A snapshot protects its index and values.** It pins a version; by Theorem 1 of
  `docs/reclamation.md` that version does not become a job; so its index segments keep
  `owners ≥ 1`, its primary segments likewise, and the chunks those list keep `refs ≥ 1`.
- **No index node references recycled memory.** An `IndexSegment` contains hashes and keys
  and two pointers into its own allocation. It holds no `ValueRef` and no chunk ID, so chunk
  recycling cannot touch it; a lookup obtains `ValueRef`s only from the primary segment of the
  same pinned version.
- **Chunk-ID reuse** is unchanged from B2: an ID returns to the pool only after `refs`
  reached zero.
- **All comparator modes.** Epoch passes and the stop-the-world sweep both destroy versions
  through `run_job` after the same comparison; disabled mode destroys nothing until the
  destructor. `run_job` releases the 64 index entries after the 256 primary ones, with the
  same resumable cursor and work budget.
- **No leak on a no-op indexed commit.** The early return frees the delta array (D-066);
  `index_test::test_commit_without_change_on_indexed_store` fails under the ASan build's leak
  checker without that line (output kept in `results/C2/`).

The B2 guarantees (gated epochs, reader-slot teardown) and the C1 ones (stop-the-world sweep
by the epoch rule, writer mutex) are untouched: `src/epoch.*` and `src/pinned_arena.*` are
identical to `part-c-transactions`.

## 4. Join correctness: findings from the code

`benchmarks/join.h` makes exactly eight calls into a store (found by `grep`), all listed:

| Call | Handle |
|---|---|
| `outer.acquire_snapshot()`, `inner.acquire_snapshot()` | — (in `acquire_join_snapshots`, the only acquisition) |
| `outer.keys(snaps.outer, …)` ×2 (first call and the retry loop) | outer |
| `store.get(snap, …)` ×2 in `detail::fetch` (the record, and the size check) | whichever snapshot the caller passed with its store |
| `inner.index_get(snaps.inner, …)` ×2 (first call and the retry loop) | inner |

- **Complete outer table:** `keys()` is repeated with a doubled buffer while it comes back
  full; the order is repeatable for one snapshot, so the final call returns all keys.
- **Uses the inner index:** one `index_get` per outer row; the inner table is never
  enumerated by the join.
- **Full inner rows; multiple matches; no matches:** every key the probe returns is fetched
  and emitted; the probe is repeated with a larger buffer while it comes back full; zero
  matches is counted as an unmatched outer row.
- **Stable snapshots; no current-version read:** there is no call of a non-snapshot `get`,
  `index_get` or `keys` in the file.
- **Release:** `JoinSnapshots` holds two `Snapshot` members; their destructors release each
  pin once when the object goes out of scope (move semantics from C1).
- **Independent oracles** in `tests/join_test.cpp`: `oracle()` joins two `std::map` models by
  brute force and never calls the store; `snapshot_oracle()` enumerates both snapshots with
  `keys()` and `get()` and joins by a plain nested loop, never calling `index_get`. Both
  implement one-snapshot-per-store semantics, and no test asserts a cross-store instant.

## 5. Option A

Final choice (your decision, D-060): **one Snapshot per `KVStore`**, both acquired at the
start of the join and held to its end.

Stated, identically, in `docs/decisions.md` (D-060), `docs/part_c_compliance.md` (J2 and note
3), `reports/part_c_readme.md`, `docs/secondary_index.md` §7.1, `docs/transactions.md` §7 and
the header comment of `benchmarks/join.h`; the verification gate `part_c_artifacts` fails if
the limitation is missing from any of the four required files:

- each store is internally snapshot-consistent for the whole join;
- there is **no** guarantee of one global cross-store instant;
- two handles do **not** literally satisfy the PDF's "exactly one Snapshot handle".

In the compliance checklist that requirement's status is **DEVIATION**, not PASS. No global
snapshot mechanism was added and none is proposed.

## 6. The supplied generator's eighth symbol byte

Verified in `supplied/workload_gen.cpp` (unmodified; checksum gate):

| Step | Code | Bytes |
|---|---|---|
| Declaration | `Trade t;` in `make_trade` — no initializer | all of `t` indeterminate |
| Symbol | `std::snprintf(t.symbol, sizeof(t.symbol), "SYM%03d", (i + j) % 50);` | `(i + j) % 50` has at most two digits, so the text is `SYM0dd`: 6 characters + terminator = bytes 0–6. **Byte 7 is never written** |
| Store | `trades.put(t.trade_id, &t, sizeof(t))` (or `txn->put`) | all 32 bytes, including that byte, become the stored row |
| Probe | `run_reader`: `trades.get(id, &t, sizeof(t))`, then, if `cfg.use_join`, `instruments->index_get(t.symbol, sizeof(t.symbol), match_ids, 8)` | an 8-byte probe taken from the stored row |
| Inner table | `load_instruments`: `create_index(offsetof(Instrument, symbol), sizeof(Instrument::symbol))`; rows from `Instrument ins{}` | 8-byte field; byte 7 is 0 |

So: is the byte uninitialized? **Yes, in the source.** How does it affect a byte-exact lookup?
The probe matches a trade exactly when that trade's stored byte 7 is 0.

**Measured** (`results/C2_audit/generator_probe_hit_rate.txt`): in every build of this
repository on this machine — debug, release, ASan+UBSan, TSan with 200 trades; release and
debug with the default 100,000 trades, loaded by puts and by transactions — the byte was 0 in
every stored trade, and the generator's probe matched **100 %**.

**Correction to the C2 report.** It said the probe "may miss". That is what the language
permits; it is not what these binaries do. Nothing was mis-measured (no generator `use_join`
result has been reported), but the statement was worded as a risk without having been
measured; it is now measured and the documents say so.

- Correctness checks: not affected. The generator discards the probe's result and checks
  nothing; the repository's own tests use fully initialized data.
- Performance: it decides what a generator `use_join` read costs (a miss is a hash and a
  bucket probe; a hit adds a primary lookup and a compare).
- A permanent check now asserts the equivalence "probe matches ⟺ stored byte 7 is 0" and
  prints both counts (`reference_workload_link_test`); it asserts nothing about the rate,
  which belongs to the supplied program.
- Part D: generator runs with `use_join` are labelled "generator index probe", carry the hit
  rate measured for that binary, and are not called a join. Index and join behaviour is
  measured with `join_bench`, whose records are zero-filled before their fields are set.

## 7. Part C API coverage on the lock-based variants

| PDF text | Bearing |
|---|---|
| Overview: Part C adds multi-key writes and a secondary index "both built on the RCU mechanism from Part B" | Part C is defined on the RCU store |
| Part C intro: "without adding a second locking scheme on top of RCU" | same |
| C.1: commit "with a single pointer-swap, built the same way as a single-key write in Part B" | a lock-based store has no pointer swap |
| A.1: a Snapshot's version "must stay pinned by your Part B reclamation scheme" | meaningless without versions |
| Part A: "Implement a KVStore class with the interface from Appendix A (put, get, erase)" | the baseline's required interface |
| Part B: "The change must live inside the KVStore class itself: don't fork the store" | one class and header, hence visible to every build |
| D.1: point gets for "all four configurations", generator "with its default parameters" (`use_txn` and `use_join` false) | the only requirement naming the lock variants uses no Part C API |
| D.3: join and transaction benchmarks | no configuration list; they measure the Part C store |
| A.2: the Part A driver example passes `nullptr` for `instruments` | baselines are not expected to see an index call |

**Assessment: not a requirement gap.** The Part C APIs must compile and link on every build
(they do) and must work on the store Part C extends, the RCU one (they do). On the lock
variants `Transaction` works (one lock hold with undo), `acquire_snapshot` throws, and
`create_index` / `index_get` report "no index": documented (D-055, D-063) and tested
(`snapshot_test_*`, `index_test_*` for the three lock variants).

Residual risk, stated: a grader who runs the generator with `use_join` against a lock driver
gets no index. Smallest correct remedy if you want it: a mutable hash index in the baseline
stores, maintained under the existing lock in `put` / `erase` / `commit_staged`, confined to
the lock branch of `kvstore.cpp`; `part-a-final` and its measurements stay as they are. Not
done and not recommended: not required, and it would put index maintenance on the Part A write
path that D.1 compares. Recorded as D-067.

## 8. Part C deliverables, as committed

All tracked in Git at `part-c-final` (checked with `git ls-files`):

| Deliverable | Where | Status |
|---|---|---|
| Transactions | `src/kvstore.cpp`, `src/staged_writes.*`, `VersionedStore::commit` | present |
| Snapshots | `src/kvstore.cpp`, `VersionedStore::Pin`, `src/epoch.h` | present |
| Secondary index | `src/secondary_index.*`, `src/versioned_store.*` | present |
| Snapshot-aware index lookups | `KVStore::index_get(const Snapshot&, …)` | present |
| Complete index-nested-loop join | `benchmarks/join.h` | present |
| Correctness tests | `transaction_test`, `snapshot_test`, `txn_failure_test`, `txn_concurrency_test`, `index_test`, `index_failure_test`, `index_concurrency_test`, `join_test` | present, passing |
| Join benchmark | `benchmarks/join_bench.cpp`, `scripts/run_c2_observation.sh`, `results/c2/` | present |
| Part C design report | `reports/part_c_readme.md`; `docs/transactions.md`, `docs/secondary_index.md` | present |
| Isolation and write-skew discussion | `docs/transactions.md` §5; README | present |
| Cross-store snapshot limitation | README, `docs/secondary_index.md` §7.1, checklist note 3, `join.h` | present |
| Part C compliance checklist | `docs/part_c_compliance.md` | present |
| Part B → Part C source diff | `results/diffs/part_b_to_part_c.diff` | present; **byte-identical to `git diff part-b-audited part-c-final -- src include`** (compared with `cmp`; also a verification gate) |
| Reproduction instructions | README, Part C README, `reports/milestones/C2.md` | present |

## Changed files and why

| File | Change | Why |
|---|---|---|
| `tests/reference_workload_link_test.cpp` | asserts "generator probe matches ⟺ stored 8th byte is 0"; prints the counts | §6 |
| `docs/decisions.md` | D-065 rewritten with the measurement; D-067 | §6, §7 |
| `docs/secondary_index.md` §8, `reports/milestones/C2.md` (annotated) | generator statement corrected | §6 |
| `docs/part_d_plan.md` | new | Part D plan, not executed |
| `results/C2_audit/` | measurement, test output, clean-clone log | evidence |
| `src/`, `include/`, `benchmarks/` | **no change** | — |

## Design decisions and invariants

D-065 (measured), D-067 (API coverage). No change to any protocol, ownership rule, public
signature or to the join.

## Build commands and exact exit statuses

`scripts/verify_all.sh` on a clean tree at `21af184`, every mode from a deleted build
directory; cmake 4.2.3, GCC 15.2.0. **Exit 0; 21 of 21 mandatory gates PASS**
(`results/verification/21af1841e8c9/summary.md`). One run; it passed.

| Mode | Configure + build exit | Warnings | Tests | `ctest` exit | Wall time |
|---|---:|---:|---|---:|---:|
| Debug | 0 | 0 | 71/71 | 0 | 81.6 s |
| Release | 0 | 0 | 71/71 | 0 | 58.7 s |
| ASan + UBSan | 0 | 0 | 71/71 | 0 | 177.9 s |
| TSan | 0 | 0 | 71/71 | 0 | 303.0 s |

Clean checkout (`results/C2_audit/clean_clone_build.log`): `git clone`, checkout `21af184`,
release configure exit 0, build exit 0 (0 warnings), `ctest` exit 0, 71/71.

The informational gate `perf_stat_usable` **passed** in this run: `kernel.perf_event_paranoid`
is now 1 (it was 4 during C2; changed outside this work, not by me). At the end of the audit
the machine was on mains power with the `performance` governor.

## Tests run (PASS/FAIL/NOT RUN)

All 71 CTest tests PASS in all four modes. Run again by hand in each verified build directory
with full output kept (`results/C2_audit/key_test_output.txt`, 48 runs, all exit 0):

| Requested | Program | Result (each of 4 modes) |
|---|---|---|
| Index tests | `index_test_rcu`, `index_failure_test`, `index_concurrency_test` | 0 failed |
| Join tests | `join_test` | 0 failed |
| Transaction and Snapshot tests | `transaction_test_rcu`, `snapshot_test_rcu`, `txn_concurrency_test` | 0 failed |
| Allocation-failure tests | `txn_failure_test_rcu`, `versioned_alloc_failure_test`, `index_failure_test` | 0 failed |
| Reclamation tests | `reclamation_test`, `reclamation_stress_test` | 0 failed |
| Generator probe | `reference_workload_link_test_rcu` | 0 failed; 200 of 200 trades with a zero 8th byte, 200 matches, in each mode |

No existing assertion was weakened; one assertion was added (the generator-probe
equivalence).

NOT RUN: any Part D measurement; profiling; machine-checked verification; the C2 mutation set
was not repeated (no source changed).

## Sanitizer / race checks (PASS/FAIL/NOT RUN)

| Check | Result |
|---|---|
| ASan + UBSan, 71 tests | PASS, exit 0, 0 reports, no leaks |
| TSan, 71 tests | PASS, exit 0, 0 reports |
| Key programs by hand under both | PASS, 0 reports in the captured output |

Sanitizer success means no problem was observed in the executions that ran; the findings of
§2–§4 come from reading the code.

## Benchmarks and measured environment (if applicable)

None run for this audit. The C2 observation set (`results/c2/`) stands as preliminary.

## Raw evidence paths and SHA-256 hashes

| Path | SHA-256 |
|---|---|
| `results/verification/21af1841e8c9/summary.md` | `10a7fecd0de3ea032b0e2cf377cce6dd0d97323dc0fd2361f19ba6be776d7961` |
| `results/verification/21af1841e8c9/logs/debug_ctest.log` | `73b099bd202992b298526571137662749a67a52cf436a3fc39e8b178979d41c1` |
| `results/verification/21af1841e8c9/logs/release_ctest.log` | `e3330bf1bf7700c55a7a23c87d722d8210055a8e9423821d17b0e4bc1940829b` |
| `results/verification/21af1841e8c9/logs/asan_ubsan_ctest.log` | `a8855c30148dc8b0eff3b407b25c9fd33d96a3b269532076b7f51ffe5ebcbb42` |
| `results/verification/21af1841e8c9/logs/tsan_ctest.log` | `a029200f3b34c271222d2e8ec1f8817ed69ae9e75258d24f9c8cb89d16bfde62` |
| `results/C2_audit/clean_clone_build.log` | `bc782b07616fd4f2aab9eae5f314298cddfc55d32ccfb7c3af2b13bb65f504b6` |
| `results/C2_audit/generator_probe_hit_rate.txt` | `88de693aa4a318a5b271a3133971d5d85c7fbf8d1ac654856ef397ed41e4d9ae` |
| `results/C2_audit/key_test_output.txt` | `2c83a80f34d0d38bce005a74ce6edba3ce09167c2d2600d28b1e0044aaf62c59` |

## Profiling bottlenecks and next hypotheses

Not applicable to this audit; carried into the Part D plan (§8 of `docs/part_d_plan.md`).

## 9. Part D readiness

Plan: `docs/part_d_plan.md` (prepared, **not executed**). In brief:

| Requirement | Plan |
|---|---|
| All required variants | `single` (1 reader only), `global_lock`, `rwlock`, `rcu` |
| Reader counts | 1, 2, 4, 8 |
| Read and writer throughput; p50 / p99 / p99.9 (and max) | `concurrent_bench` (writer active during the window), and the unmodified generator as a separately labelled preload-only track |
| Transaction sizes 1, 4, 16 | `join_bench --experiment txn` |
| ≥ 4 reclaim batches | 1, 8, 64, 256, 1024, plus a workload in which reclamation falls behind |
| No-reclamation and stop-the-world | `--reclaim disabled`, `--reclaim stw` |
| Join latency, writer running / paused | `join_bench --experiment join --threads 8 --writer on\|off` |
| Raw data and perf evidence | per-run CSV, latency histograms, stdout with command and exit status, `perf stat`, flame graph, `perf c2c` |
| Repetitions, affinity, environment | 5 processes per cell; readers on 0,2,4,6,1,3,5,7, writer on 15; conditions logged; campaign discarded if power or governor changes |
| Comparability | one session only; lock variants re-measured in it; generator runs never called concurrent; design difference stated beside every `rcu`-vs-lock comparison |

Still to be written before measuring: the campaign script and aggregator, the
"reclamation falls behind" scenario, a generator wrapper that prints the probe hit rate, the
report.

Setup you would need to do (I have not and will not change system settings):

```bash
sudo sysctl kernel.perf_event_paranoid=1                 # this boot (currently 4)
echo 'kernel.perf_event_paranoid = 1' | sudo tee /etc/sysctl.d/99-perf.conf && sudo sysctl --system   # persistent
sudo sysctl kernel.kptr_restrict=0                       # optional: kernel symbols in profiles
```

plus mains power, the `performance` governor, and a quiet machine. Without `perf`, D.1's
"backed by a flame graph" cannot be met and that gate would be BLOCKED.

State observed at the end of this audit (re-check before measuring; it reverts on reboot):
`kernel.perf_event_paranoid = 1`, `perf stat` usable, mains power, `performance` governor;
load average about 2.5, which is too high for tail-latency measurements.

## Known limitations and source ambiguities

- The join uses one Snapshot per store: a deviation from the PDF's single-handle wording.
- No index and no Snapshot on the lock-based variants (not required; §7).
- A write that changes the indexed field rebuilds whole index partitions; reads next to an
  active writer run at about half speed (cause not established: no profiler).
- Isolation as stated in C1: no validation; lost updates and write skew possible; no
  durability.
- The generator's eighth symbol byte is indeterminate in the source (0 in every build
  measured here).
- The safety argument is manual. `reclamation_stress_test` keeps scheduling-dependent
  coverage floors: run the suite serially.

## Git commit SHA, tag, diff summary

| Commit | Content |
|---|---|
| `27d3161` | tag `part-c-final` (pushed; unchanged) |
| `21af184` | audit: generator-probe check and measurement, D-065 rewritten, D-067, Part D plan. **Verified commit.** No change under `src/`, `include/`, `benchmarks/` |
| (next) | this report and the verification evidence |

`git diff part-c-final 21af184 -- src include benchmarks` is empty, so the Part B → Part C
diff is unchanged and still equals `git diff part-b-audited part-c-final -- src include`.
`main` untouched (`a3cccd8`); nothing merged; no tag moved or created.

## Gate verdict: PASS / FAIL / BLOCKED

**PASS.** Read as committed: every published root carries the index that matches its rows;
an indexed commit is one publication and a failed one publishes nothing; index entries follow
field changes and deletions; lookups cannot return a wrong key or read past a value; index
creation is atomic; index segments are owned and reclaimed by the argued rule and are
protected by snapshots in every reclamation mode; the join is complete, index-driven and
snapshot-scoped throughout, and its tests use oracles that do not touch the index. Option A is
documented as a deviation everywhere it must be. All Part C deliverables are committed and
the diff matches Git. 21/21 mandatory gates pass in four build modes on the audit commit.

Fixes required: none in the store or the join. One statement of the C2 report was corrected
by measurement (§6).

Qualifications, none an unresolved safety or correctness obligation for Part C: the literal
single-handle requirement is a recorded deviation, not PASS; the safety argument is manual;
the Part C APIs are not provided on the lock variants (not required).

**C2 AUDIT PASS — READY FOR PART D**

## Next milestone proposed (not started)

Part D, on your approval and once `perf`, power and machine state are as in §9.
