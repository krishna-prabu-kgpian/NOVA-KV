# Milestone D — performance evaluation (D.1, D.2, D.3)

> **Superseded for results.** This is the report of the first campaign, as tagged
> `part-d-final` (9062d83), kept unchanged below this note as a record. Its timing figures
> were measured inside `perf stat` and without control of the lock placement, and several of
> its statements were later corrected or withdrawn: see `reports/milestones/D_corrected.md`
> and `reports/part_d_report.md`. The paths it gives under `results/part_d/` are now under
> `results/part_d/original-wrapped-20261013T090846-615f4357f015/`.
> One count is corrected in this copy: the suite has 22 mandatory gates, not 23 as the tagged
> text says (22 of 22 passed; the failed run before it 21 of 22).

Date: 2026-10-13. Branch `part-d/benchmarks`, from the audited Part C checkpoint `b27efd7`.
Measured commit: `615f435`. Verified commit: `e09da24`.

## Scope completed

Blueprint §8.9 and §9; assignment Part D (PDF pp. 5–6).

1. Branch created from `b27efd7`; all eight earlier tags verified unmoved.
2. Plan re-read; seven deviations recorded in `docs/part_d_plan.md` §9 **before** any result
   was collected.
3. Environment checked by script before the campaign and at every stage boundary.
4. Harness: `scripts/run_part_d.sh` (prerequisites, pilot, campaign, single-stage rerun) and
   `scripts/aggregate_part_d.py` (completeness checks, summaries, figures); a new benchmark
   for the backlog workload (`benchmarks/reclaim_bench.cpp`); copy-cost and histogram output
   added to `benchmarks/join_bench.cpp`; hit-rate reporting added to the generator drivers.
5. Phase 1 pilot (passed, `results/part_d_pilot/`), then the Phase 2 campaign: 300 runs, five
   repetitions per cell, no failed run, no change of conditions.
6. D.1, D.2, D.3 measured; `perf stat` around every run; nine flame graphs; four `perf c2c`
   reports; twelve figures.
7. Report (`reports/part_d_report.md`, `.pdf`, 9 pages) and requirement-by-requirement
   checklist (`docs/part_d_compliance.md`).

Not done, by instruction: Part E, architecture experiments, the submission ZIP. **No
optimization was made**: nothing under `src/` or `include/` differs from `part-c-final`
(`git diff part-c-final HEAD -- src include` is empty).

Preservation: no tag moved, no history rewritten, `main` untouched (`a3cccd8`), Part A
results byte-identical to `part-a-final`, the supplied workload unchanged, the Part B and
Part C diffs still equal to Git. No `sudo`, no system setting changed, no process killed.

## Requirements covered (PDF section/page)

Full table: `docs/part_d_compliance.md`.

| Requirement | Result | Status |
|---|---|---|
| D.1 four configurations at 1, 2, 4, 8 readers, p50/p99/p99.9 | measured on two tracks | PASS |
| D.1 "one background writer thread active throughout" | the supplied generator cannot do it; measured with `concurrent_bench` (track C) | PASS with that qualification |
| D.1 where the lock variants stop scaling | global mutex after 4 readers, shared mutex after 4 | PASS |
| D.1 100,000 reads/s at 8 readers on RCU | **met**: 99.2 M/s (paced writer), 50.1 M/s (unthrottled) | PASS |
| D.2 at least four batch settings, throughput and longest stall | 1, 8, 64, 256, 1024 in two workloads | PASS |
| D.2 the expected trade-off (bigger batch: more throughput, longer stall) | **not observed**; reported as measured | NOTE |
| D.2 reclamation disabled; naive stop-the-world | both, both workloads | PASS |
| D.3 join at 8 threads, writer active against paused | 580 against 1,026 joins/s | PASS, one Snapshot per store (two handles) |
| D.3 commit throughput for 1, 4, 16 keys | 238,103 / 101,298 / 26,655 commits/s | PASS |
| D.3 bounded against stop-the-world worst pause | 28× (steady), 87× (backlog) | PASS |
| D.3 workload where copying hurts | explained and measured | PASS |
| Raw perf and histogram data for each | `results/part_d/` | PASS |

## Changed files and why

| File | Change |
|---|---|
| `scripts/run_part_d.sh` (new) | the campaign: prerequisites, stages, condition records, campaign stamps, refusal to mix sessions or commits |
| `scripts/aggregate_part_d.py` (new) | validates completeness, computes median/min/max per cell, writes summaries and figures |
| `scripts/part_d_profile_breakdown.py`, `scripts/c2c_by_symbol.py` (new) | cycle shares from the folded stacks; `perf c2c` samples by function |
| `scripts/build_part_d_report.sh` (new) | builds the PDF |
| `benchmarks/reclaim_bench.cpp` (new) | D.2 workload B: large values, a reader that pins snapshots, a timed cleanup phase. Changed once after the campaign (`e09da24`, validation only, D-070) |
| `benchmarks/join_bench.cpp` | histogram output, p99.9, copy cost per commit, `cow` experiment |
| `drivers/driver_common.h` | prints the generator's measured hit rate; optional write mode argument |
| `CMakeLists.txt` | the new target and four harness self-tests (75 tests) |
| `scripts/verify_all.sh` | gate `part_d_artifacts`; tag check now includes `part-c-final` |
| `docs/part_d_plan.md`, `docs/part_d_compliance.md`, `docs/decisions.md` (D-068, D-069, D-070), `README.md`, `CLAUDE.md` | plan deviations, checklist, findings, status |
| `reports/part_d_report.md`, `.pdf` | the Part D report |
| `results/part_d/`, `results/part_d_pilot/`, `results/part_d_diagnostics/` | evidence |

`supplied/`, `src/`, `include/` and `tests/`: unchanged.

## Design decisions and invariants

- **Two tracks, never combined.** Track R is the unmodified generator and is preload-only;
  track C is `concurrent_bench` with a writer that is verified to have written (successful
  updates counted, version sequence checked).
- **The store measured is the audited Part C store.** Benchmark code only observes it through
  the public API and the existing statistics.
- **One campaign, one session, one commit.** Every stage directory carries the campaign id;
  the runner marks the campaign INVALID if power source or governor changes; the aggregator
  lists every missing cell.
- **Nothing is removed.** Every run's stdout, histogram and `perf stat` file is kept; tables
  give median, minimum and maximum of five; no outlier was dropped.
- **Memory guard for "reclamation disabled"**: the run stops at 16 GB retained
  (`RETAIN_LIMIT_MB`); it did not trigger (largest: 4.4 GiB).
- Engine invariants INV-01…INV-15 are untouched; the harness self-tests and the full suite
  ran after the last harness change.

## Build commands and exact exit statuses

`scripts/verify_all.sh` on a clean tree at `e09da24`, every mode from a deleted build
directory; cmake 4.2.3, GCC 15.2.0. **Exit 0; 22 of 22 mandatory gates PASS**
(`results/verification/e09da24ffe0d/summary.md`).

| Mode | Configure + build exit | Warnings | Tests | `ctest` exit | Wall time |
|---|---:|---:|---|---:|---:|
| Debug | 0 | 0 | 75/75 | 0 | 86.4 s |
| Release | 0 | 0 | 75/75 | 0 | 62.0 s |
| ASan + UBSan | 0 | 0 | 75/75 | 0 | 190.4 s |
| TSan | 0 | 0 | 75/75 | 0 | 304.0 s |

Clean checkout (`results/D/clean_clone_build.log`): `git clone`, checkout `e09da24`, release configure exit 0, build exit 0 (0 warnings), `ctest` exit 0, 75/75.

**This was the second verification run of the milestone. The first, at `1006123`, exited 1**
(`results/verification/1006123097e1/`, kept): 21 of 22 mandatory gates passed and
`tsan_ctest` failed with 74 of 75 tests, the failure being the new harness self-test
`bench_validate_reclaim_stw`. The cause was a wrong expectation in the new benchmark, not the
store: it required the retire list to reach zero, while the stop-the-world comparator frees
only a list of at least 1,024 entries, and under TSan the list held 833 at release. The
validation was corrected (decision D-070, commit `e09da24`), the self-test then passed three
times in a row under TSan, and the whole suite was run again. The engine was not changed and
the campaign's stop-the-world runs (7,178–10,596 entries at release, all drained) do not
exercise the corrected branch.

Campaign commands (each exit 0):

```bash
scripts/run_part_d.sh check                      # prerequisites
scripts/run_part_d.sh pilot                      # -> results/part_d_pilot/
scripts/run_part_d.sh all                        # -> results/part_d/   (37 minutes)
.tools/venv/bin/python scripts/aggregate_part_d.py results/part_d   # "complete: no problems found"
.tools/venv/bin/python scripts/part_d_profile_breakdown.py results/part_d
scripts/build_part_d_report.sh
```

The exact command and exit status of each of the 300 runs is in its `stdout/` file; the
profiling commands are in `results/part_d/perf/*/COMMANDS.txt`.

## Tests run (PASS/FAIL/NOT RUN)

| Test set | Result |
|---|---|
| Full suite, four build modes (75 tests each) | PASS: 75/75 in Debug, Release, ASan + UBSan and TSan (second run; first run 74/75 under TSan, see above) |
| Harness self-tests `bench_validate_reclaim_*` (3), `bench_validate_join_cow`, and the earlier `bench_validate_*` | PASS (part of the suite) |
| Gate `part_d_artifacts` | PASS |
| Per-run validation inside each benchmark (reader checksums, writer progress, final contents, join result against an index-free oracle) | PASS in all 300 runs |
| Aggregator completeness check | PASS: `results/part_d/summary/problems.txt` is empty |

No correctness defect of the store appeared in any benchmark or test run. One defect of the
benchmark's own validation was found and corrected (above).

## Sanitizer / race checks (PASS/FAIL/NOT RUN)

| Check | Result |
|---|---|
| ASan + UBSan, full suite including the new harness self-tests | PASS, 75/75, no report |
| TSan, full suite including the new harness self-tests | PASS, 75/75, no report (second run) |

No sanitizer build was used for any measurement.

## Benchmarks and measured environment (if applicable)

AMD Ryzen AI 7 350 (8 cores, 16 logical CPUs; 4 fast, 4 slower cores), 30 GiB, Ubuntu 26.04,
kernel 7.0.0-28, GCC 15.2, `-O3 -g -DNDEBUG -fno-omit-frame-pointer`. Mains power,
`performance` governor and EPP, at all 56 condition records; battery charging 57 % → 90 %,
temperature 44 °C at the start, 77 °C at the end, 86 °C at the hottest record. Readers pinned to CPUs
0,2,4,6,1,3,5,7 in that order, writer to CPU 15, cleaner not pinned. Medians of five.

**D.1, track C (writer active), million reads/s at 1 / 2 / 4 / 8 readers**

| Store | Writer paced at 10,000/s | Writer unthrottled |
|---|---|---|
| global mutex | 17.00 / 18.47 / 21.73 / 12.39 | 5.93 / 7.14 / 9.53 / 8.37 |
| shared mutex | 16.86 / 23.49 / 34.28 / 21.37 | 0.14 / 0.49 / 1.57 / 21.30 |
| RCU | 14.81 / 30.39 / 60.44 / 99.25 | 8.36 / 18.00 / 28.69 / 50.09 |

Single thread: 19.12 M/s read-only, 18.21 M/s with writes interleaved (no concurrent run).
RCU at 8 readers: p99.9 0.21 µs (paced), 0.56 µs (unthrottled). The shared mutex reaches its
8-reader figure by starving the writer (1,582 of 10,000 paced updates/s; 1,500/s
unthrottled). The RCU writer does 167k → 101k updates/s as readers are added.

**D.1, track R (supplied generator, preload-only), million lookups/s:** global mutex 15.58 /
15.74 / 18.39 / 12.05; shared mutex 15.50 / 19.05 / 24.20 / 18.66; RCU 15.07 / 25.69 / 49.29 /
84.19; single thread 16.89. Measured hit rate 80.02 %.

**D.2 (RCU, 8 readers, unthrottled writer)**

| Setting | Reads/s (M), A | Longest stall µs, A (median / worst) | Reclaimed/s, A | Reads/s (M), B | Longest stall µs, B | Drain ms, B |
|---|---:|---:|---:|---:|---:|---:|
| batch 1 | 50.99 | 227 / 388 | 81,672 | 37.84 | 235 | 29.9 |
| batch 8 | 50.38 | 129 / 495 | 87,068 | 40.30 | 275 | 18.1 |
| batch 64 | 50.31 | 56 / 683 | 87,419 | 40.21 | 289 | 18.4 |
| batch 256 | 50.63 | 68 / 340 | 83,175 | 40.20 | 203 | 15.3 |
| batch 1024 | 50.01 | 46 / 272 | 97,374 | 40.22 | 188 | 14.6 |
| disabled | 73.97 | 96 / 396 | 0 (4,405 MiB in 1.5 s) | 43.09 | 54 | — (1,725 MiB) |
| stop-the-world | 4.62 | 6,436 / 6,777 | — | 3.42 | 25,061 | — |

(A: the D.1 benchmark; B: 2,000 keys of 4 KiB with a reader pinning snapshots. Disabled, workload B: 43.09 M reads/s. Workload B stalls are medians. Minimum and maximum of each cell: `summary/d2_steady.csv`, `summary/d2_backlog.csv`.)

**D.3.** Transactions without an index: 238,103 / 101,298 / 26,655 commits/s for 1 / 4 / 16
keys (238,103 / 405,192 / 426,486 rows/s; p50 3.7 / 9.2 / 37.5 µs; 26,102 / 97,729 / 376,025
bytes copied per commit); with one secondary index 58,824 / 17,555 / 6,134. Join of 100,000
trades with 50 instruments, 8 joiners: 1,026 joins/s, p50 7.14 ms, p99 9.34 ms with the
writer paused; 580 joins/s, p50 13.15 ms, p99 21.12 ms with it active. Loading 100,000 rows:
13.4 ms and 61 bytes copied per row in one transaction; 559 ms and 26,103 bytes per row as
single writes. Generator index probe (not a join): 61.30 M operations/s, hit rate 100 %.

## Raw evidence paths and SHA-256 hashes

| Path | SHA-256 |
|---|---|
| `results/verification/e09da24ffe0d/summary.md` | `455671f4ef7e15ebe83be646f16df1ec42523495d0a427b04452609c6d6f53b3` |
| `results/verification/e09da24ffe0d/logs/debug_ctest.log` | `42b3da870a2fd652c55a9cf44a42f65cdd3150e3a3d4b762d688d6ccbeeefee9` |
| `results/verification/e09da24ffe0d/logs/release_ctest.log` | `571db96e608c4aaa24b6e1ce836bce225b9cebc0120f9035e49bf8460ee84199` |
| `results/verification/e09da24ffe0d/logs/asan_ubsan_ctest.log` | `77a48ea970089e415a36a0b1ec07a12016bca22ce50707b2e7f64e398e783c40` |
| `results/verification/e09da24ffe0d/logs/tsan_ctest.log` | `91d20bac3ac9606741ac1ca7efbfb65d6fb9b84c4c6c6946902b51871a87d122` |
| `results/verification/1006123097e1/summary.md` | `f98a29a65db425372135b4edd78f6f7023201a30d81e0ee7fe5d34f7959dbb47` |
| `results/D/clean_clone_build.log` | `e455f6b388192269161902ecd20ced054358ea8ffa5e2ad276864cb598963497` |
| `results/part_d/CAMPAIGN` | `8a9c617dd6f3fed6ada609156cb5277cbb65513f53e676efa41ba035ab00dc83` |
| `results/part_d/conditions.log` | `04654aba6ba77518eddfe52c172540eba9e2eb95c3a44c46f8ac75121d284b1a` |
| `results/part_d/env/campaign_config.txt` | `0e67f51f3a2a63f443e291687c23a4266bfa430309e25212375f9279de06f052` |
| `results/part_d/summary/summary.md` | `6ba39824e284487a4b2b3211ede46a6def0bd2cd95ae78c6fbfe2a58a8dca130` |
| `results/part_d/summary/profile_breakdown.txt` | `de6281ebefa87cae67799149072163fa9ed1548fd2cb5b85f3d3e9a2ef862519` |
| `results/part_d/d1/concurrent/runs.csv` | `3e6468f54144afb72117f5a9b3205a533dc8d36458156ea65a6a9d2d2d959f39` |
| `results/part_d/d1/reference/runs.csv` | `22be7468e31b16c58bac896ecc91e4ef3bdbb0f0d7944302a4367a9c3857fc01` |
| `results/part_d/d2/steady/runs.csv` | `f2ec0b94ad19368d2789ef3149b2b703c8c38016f920c60b582c374b0840446e` |
| `results/part_d/d2/backlog/runs.csv` | `56bf8ff0e25fc6543074ffaa11c6efad275a34a2226323261f2185c900c6bad4` |
| `results/part_d/d3/txn/runs.csv` | `9a5ac50bf4198b1b8f5be64d37fef5f89a358f931abf97066da3d2c6cefcf349` |
| `results/part_d/d3/join/runs.csv` | `0c0ea164e0b48d5d1845bcfc0c1338d7495d8421369135fd0bc15d47b031f411` |
| `results/part_d/d3/cow/runs.csv` | `345e78d9c53a4a460e0847e43758d6230d9633883c95b051ae1468b570b35551` |
| `results/part_d_diagnostics/perf_stat_wrapper_effect.txt` | `e282da3e2954607843b2a3fedcb552a6a34930759c329e69a60af989ccfd73c3` |
| `results/part_d_diagnostics/part_b_reference_backlog.txt` | `53b090e4ea7ee1c5925f854f630b9bd831ce73bdbd14ba5a7445d0c20813ea1d` |
| `reports/part_d_report.pdf` | `2e916569663758215fba6a4491535405b08cd03945dea9a59043513a9a3d5d48` |

Also in `results/part_d/`: `env/` (environment, hardware, build log, prerequisites,
configuration with the hashes of the measured executables), per-stage `stdout/`, `hist/`,
`perf_stat/`, `perf/flamegraphs/` (SVG, folded stacks, compressed `perf script` output),
`perf/c2c/`, `figures/` (12 PNG).

## Profiling bottlenecks and next hypotheses

Evidence (flame graphs, `perf stat`, `perf c2c`; `summary/profile_breakdown.txt`):

- **Global mutex, 8 readers:** 77 % of cycles in the kernel's futex path and 13 % in the
  mutex functions. The lock is the bottleneck.
- **Shared mutex, 8 readers:** 66 % of cycles in `pthread_rwlock` read lock and unlock; 67 %
  of the cache-line contention samples on one line (the lock word). Readers do not block each
  other but all write the same line.
- **RCU, read-only:** a third of the cycles are the harness's own clock reads; the rest is
  the lookup. No lock, no shared write.
- **RCU next to an unthrottled writer:** instructions per cycle fall from 1.33 to 0.58 and
  cycles per read rise from 335 to 928.

Hypotheses, with their status:

| Hypothesis | Status |
|---|---|
| R8: owner counts share cache lines with data the readers load | **weakly supported**: reader lookups are 6.7 % of the contended-line samples, 0.07 % of sampled loads; most coherence traffic is writer against cleaner |
| The read slowdown next to an unthrottled writer comes from copy-on-write moving the data through the shared cache | **not established**; consistent with the counters, not isolated by an experiment |
| A larger batch gives higher read throughput | **refuted** for this design |
| A larger batch gives longer reader stalls | **refuted** for this design: readers never wait for the cleaner |
| The cleaner keeps up with an unthrottled writer | **refuted** at 4 and 8 readers in `performance` mode |

Candidate experiments, none started: separate the owner count from the segment header;
back-pressure or a second cleaner; batching writes (already available as transactions).

## Known limitations and source ambiguities

1. **The measurement wrapper is not neutral.** Every timed run ran under `perf stat` (a plan
   deviation chosen before the campaign). A check afterwards
   (`results/part_d_diagnostics/perf_stat_wrapper_effect.txt`) shows the wrapper raises the
   global-mutex figure at 8 readers by about 20 %, halves stop-the-world read throughput
   (3.5 against 6.2–8.0 M/s), lowers the shared mutex by 0–6 % and moves RCU by at most 4 %;
   longest stalls are unaffected. The tables are the wrapped runs, unchanged; no conclusion
   depends on the affected figures, but those throughputs are biased. Transactions and joins
   were not checked. A second campaign with unwrapped timing runs would remove this; it has
   not been run.
1. Track R cannot satisfy "one background writer thread active throughout"; track C does, on
   the same schema, with the repository's own harness.
2. With an unthrottled writer and 4 or 8 readers the RCU store is **not in a steady state**:
   backlog 23,852 / 138,157 versions and 655 / 3,662 MiB at the end of a 5 s run, still
   growing. The audited Part B binary behaves the same. Numbers for those cells describe a
   5 s window, not a sustainable rate of memory use.
3. The expected D.2 trade-off was not observed (decision D-069).
4. The join reads each store from one Snapshot: two handles, not one atomic cross-store
   snapshot (decision D-060). The literal single-handle requirement is not claimed.
5. Throughput includes two clock reads per operation; absolute read rates of the store alone
   are higher than reported.
6. One machine, heterogeneous cores, laptop cooling: temperature rose during the campaign
   while conditions otherwise held. Stage order was fixed, not randomized.
7. `perf c2c` on this CPU uses IBS sampling; kernel symbols are hidden (`kptr_restrict=1`).
8. The palette validator of the plotting guideline could not be run (no `node`); the figures
   use its documented default palette in fixed order, with labels and a legend.
9. `perf_event_paranoid=1` was set by the user and does not survive a reboot.

## Git commit SHA, tag, diff summary

| Commit | Content |
|---|---|
| `b27efd7` | audited Part C checkpoint; base of this branch |
| `615f435` | campaign harness, new benchmark, plan deviations, pilot. **Measured commit.** |
| `1006123` | campaign results, report, checklist, gate. First verification run: FAIL (one harness self-test under TSan) |
| `e09da24` | self-test validation corrected; wrapper-effect diagnostic and disclosure. **Verified commit.** |
| (next) | this report, verification evidence; tag `part-d-final` |

Tags, all unmoved (checked by the gates): `a0-bootstrap` 2ef5f19, `part-a-core` 80bc092,
`part-a-final` 3444f23, `part-b-publication` 2d9c02c, `part-b-final` 304a8ac, `part-b-audited`
87d38aa, `part-c-transactions` 1e6af10, `part-c-final` 27d3161.

Diff from `b27efd7` to the verified commit, outside `results/`: 17 files changed, 2875 insertions(+), 19 deletions(-); none under
`src/`, `include/`, `tests/` or `supplied/`. `main` is untouched (`a3cccd8`); nothing merged;
no force push. The branch `part-d/benchmarks` and the tag `part-d-final` are **local, not
pushed** (pushing was not part of this milestone's instructions).

## Gate verdict: PASS / FAIL / BLOCKED

**PASS** for Milestone D.

| Gate | Verdict |
|---|---|
| Environment prerequisites (mains, governor, perf, idle machine) | PASS |
| Conditions unchanged through the campaign | PASS (56 records) |
| Pilot | PASS |
| Campaign complete, five repetitions per cell | PASS (300 runs, 0 failures) |
| D.1 measured, scaling limits identified | PASS |
| D.1 100,000 reads/s target | PASS (met) |
| D.2 measured, both reference points | PASS |
| D.2 expected trade-off demonstrated | **not observed** — reported, not a harness failure |
| D.3 join, transactions, pause comparison, copy-cost discussion | PASS (join: documented two-handle deviation) |
| perf stat, flame graphs, perf c2c | PASS |
| Report, checklist, figures | PASS |
| Full verification suite after the last harness change | PASS |
| Clean-clone build | PASS |
| Part E | NOT RUN (not started, by instruction) |
| Submission ZIP | NOT RUN (not assembled, by instruction) |

Handoff audit items applicable to this milestone (formal-safety supplement §H):

| Item | Requirement | Evidence | Verdict |
|---|---|---|---|
| H03 | supplied generator unchanged and a true concurrent benchmark, outputs labelled | tracks R and C; gate `supplied_files_unchanged` | PASS |
| H05 | two snapshots are not the single-handle contract | stated in the report, checklist and here | PASS as a recorded deviation; the literal requirement is not claimed |
| H06 | measure copy bytes, writer latency, destruction cost | `d3/txn`, `d3/cow`, `summary/d1_rcu_reclamation.csv` | PASS |
| H13 | cleaner work per pass bounded and recorded | `max_versions_in_a_pass` in `summary/d2_*.csv` | PASS |
| H15 | record the generator's miss rate; a controlled-hit benchmark | 80.02 % hits measured; track C | PASS |
| H16 | no sanitizer numbers compared with release | release builds only | PASS |
| H17 | real single-thread baseline; no unsynchronized concurrent run | single thread at one reader only | PASS |
| H21 | record hardware and perf permissions; no invented flame graphs | `env/`; flame graphs generated from recorded samples | PASS |

## Next milestone proposed (not started)

Either Part E (optional WAL and durability bonus) or the final packaging milestone
(`NOVA_KV_Final_Submission_Checklist.md`; needs the roll numbers). Neither is started; both
wait for explicit approval.
