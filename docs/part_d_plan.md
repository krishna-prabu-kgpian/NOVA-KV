# Part D measurement plan (prepared in the C2 audit; NOT executed)

Nothing in this document has been run. It fixes, before any measurement, what will be
measured, with which program, under which conditions, and how results will be judged
comparable. Requirements: `assignment2.pdf` pp. 5–6 (D.1–D.3); blueprint §9; measurement rules
D-024…D-029, D-046.

## 0. Preconditions (yours; nothing here changes the system)

| Need | Why | How (run by you; I will not use `sudo`) |
|---|---|---|
| `perf` usable | D.1's flame graph; `perf stat`; `perf c2c` for the cache-line hypothesis | now: `kernel.perf_event_paranoid = 4`. For this boot: `sudo sysctl kernel.perf_event_paranoid=1`. To survive reboots: `echo 'kernel.perf_event_paranoid = 1' \| sudo tee /etc/sysctl.d/99-perf.conf && sudo sysctl --system`. Optional, kernel symbols: `sudo sysctl kernel.kptr_restrict=0` |
| Mains power, `performance` governor | the campaign must not change conditions part-way (D-029) | plug in; check `cat /sys/class/power_supply/AC*/online` = 1 and `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` |
| Quiet machine | tail latencies and worst stalls are the point of D.2 | close browsers / IDE indexing; load average < 0.5 before starting |
| About 45 minutes uninterrupted | one session for everything that is compared | — |

The scripts refuse to start (or mark the campaign invalid) if the power source or governor
changes between the first and the last run, as `scripts/run_part_a.sh` already does.

## 1. Conditions recorded with every run

`scripts/env_info.sh` (CPU model, topology, kernel, compiler, flags), `conditions.log` (time,
AC state, governor, EPP, load average) at the start and after every repetition, the exact
command line and exit status of every run, the git SHA, CPU affinity. Release build only
(`-O3 -g -DNDEBUG`, frame pointers kept); never a sanitizer build.

**CPU affinity.** Reader *i* on the *i*-th of `0,2,4,6,1,3,5,7` (the four fast cores first, then
the four slower ones; no two readers on SMT siblings); writer on CPU 15; the store's cleaner
thread unpinned (recorded; a second set with the cleaner pinned to CPU 14 for D.2).
**Repetitions:** 5 independent processes per configuration (blueprint §9.3); medians with
min–max; no outlier removed. **Windows:** 0.5 s warm-up, 5 s measured (D.1), 3 s (D.2, D.3).

## 2. Two tracks, never mixed in one table

| Track | Program | What it is | Label |
|---|---|---|---|
| R (reference) | the four drivers linked with the **unmodified** `supplied/workload_gen.cpp`, default parameters | the assignment's generator. Its writer finishes loading **before** readers start: there is no writer during the measured window, and 20 % of its reads are random keys that mostly miss | "supplied generator, preload-only" |
| C (concurrent) | `benchmarks/concurrent_bench.cpp` (`concurrent_bench_<variant>`) | same schema and key distribution; one writer thread running **during** the measured window; every read validated | "active writer" |

D.1 asks for the generator "with one background writer thread active throughout". The supplied
generator cannot do that, so D.1 reports **both** tracks side by side and says which satisfies
which half of the sentence (H03; D-024).

## 3. D.1 — point-query throughput and the 100k RPS target

| | |
|---|---|
| Variants | `single` (1 reader only; a concurrent run of the unsynchronized store is invalid and the harness rejects it), `global_lock`, `rwlock`, `rcu` |
| Readers | 1, 2, 4, 8 |
| Track R | `build/release/<driver> <readers> 5` × 5 repetitions |
| Track C | writer paced at 10,000 puts/s **and** unthrottled, × 5 repetitions |
| Reported | read RPS; read p50 / p99 / p99.9 / max; writer puts/s; for `rcu` also backlog max, memory held, cleaner CPU |
| Scaling statement | for `global_lock` and `rwlock`: the reader count after which the median stops increasing (ranges shown) |
| 100k check | `rcu` at 8 readers, both tracks: median and minimum of 5 against 100,000 gets/s |
| perf | `perf stat` (task-clock, cycles, instructions, cache-misses, context-switches) for each variant at 8 readers; `perf record -g` + flame graph for `rcu` at 8 readers with the writer active (required only if below 100k, taken regardless); `perf c2c` for `rcu` to test the owner-count cache-line hypothesis (R8) |

Commands (track C, one cell):
`build/release/concurrent_bench_rcu --readers 8 --reader-cpus 0,2,4,6,1,3,5,7 --writer on --writer-cpu 15 --warmup 0.5 --duration 5 --seed <rep> --csv … --hist …`

## 4. D.2 — reclamation-batch tuning

Same point-get benchmark (track C, `rcu`, 8 readers, unthrottled writer).

| Setting | How |
|---|---|
| Batch 1, 8, 64, 256, 1024 | `--reclaim-batch N` (→ `KVStore::set_reclaim_batch`) |
| Reclamation disabled | `--reclaim disabled`, 1 s window, memory guard (it retains everything) |
| Naive stop-the-world | `--reclaim stw` |

Reported per setting: sustained read RPS; **longest single reader stall** (max of the latency
histogram, per run and worst of 5); p99.9; versions freed per pass (max); passes; backlog max;
memory held; cleaner CPU.

Known problem to solve first (B2 observation): with the default workload the cleaner keeps up
at every batch size, so 256 and 1024 never fill and the trade-off is invisible. The plan adds
a second, clearly labelled workload in which reclamation falls behind, so that a pass actually
has a full batch to free: larger values (4 KB) so that a version's destruction costs more, and
a reader that periodically holds a snapshot for a few milliseconds so that a backlog builds and
is then released in passes. Both workloads are reported. If the trade-off still does not show,
that is what will be reported, with the data.

## 5. D.3 — join and multi-key write

| Experiment | Program | Matrix |
|---|---|---|
| Index-nested-loop join, 8 joiner threads, writer active vs paused | `join_bench --experiment join --threads 8 --writer on\|off` | also 1/2/4 threads; 100,000 Trades × 50 Instruments; joins/s, joined rows/s, p50/p99/p99.9/max per join; memory held, backlog |
| Transaction commit throughput vs size | `join_bench --experiment txn --txn-size 1\|4\|16 --index off` (the Trades table as the assignment loads it), and `--index on` as an extra | commits/s, rows/s, commit p50/p99/max; segments cloned and bytes copied per commit from the store's counters |
| Bounded reclamation vs stop-the-world: worst reader pause | from D.2: max stall, epoch-based at each batch vs `stw` | ratio, with both raw maxima |
| COW worst case (the question in D.3) | one transaction touching all 256 partitions (e.g. 100,000 keys) vs the same rows as single puts; bytes copied per row | explanation backed by the counters |

The join uses **one Snapshot per store** (D-060). Every join table and figure carries that
statement: each table is read from one version; the two versions are not one instant; two
handles, not the PDF's single handle.

**Generator `use_join` runs** (track R) are reported separately as "generator index probe",
with the measured hit rate of the run (D-065), and are not called a join.

## 6. Comparability rules

1. Only numbers from **one session** of the campaign script are compared. Part A's accepted
   results (`results/part_a/`) and the B2/C2 observation sets stay as they are and are not used
   for ratios; the Part A variants are re-measured in the session.
2. `rcu` vs the lock variants compares two whole designs (storage and update strategy differ,
   not only synchronization): said next to every such comparison (D-038).
3. Track R numbers are never called "concurrent" or "with a writer".
4. A sanitizer or debug build is never measured.
5. Every reported number traces to a file under `results/part_d/`; raw per-run CSV rows,
   latency histograms, stdout with command and exit status, perf data and the generated flame
   graphs are kept. A failed validation fails the campaign.
6. If the power source or governor changes, the campaign is discarded and rerun (D-029).
7. No tuning between measurement and report without before/after data under the same
   conditions.

## 7. What has to be written for it (not started)

- `scripts/run_part_d.sh`: the session described above, modelled on `run_part_a.sh`;
- `scripts/aggregate_part_d.py`: medians, ranges, the scaling statement, tables and figures;
- `concurrent_bench`: a "reclamation falls behind" scenario (§4);
- a generator driver wrapper that prints the probe hit rate for `use_join` runs;
- the Part D report.

## 8. Risks known in advance

- `perf` unavailable ⇒ no flame graph ⇒ D.1's bottleneck explanation cannot be "backed by a
  flame graph"; that gate would be BLOCKED, not PASS.
- On battery or with background load the tail numbers are not meaningful.
- Reads next to an active writer currently run at about half speed (C2 observation, cause not
  established): the R8 hypothesis should be tested with `perf c2c` before D.1's final numbers,
  as a measured before/after change, or left alone and explained.
