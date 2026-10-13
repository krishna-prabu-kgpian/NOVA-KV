# Part D measurement plan

Sections 0–8 were written in the C2 audit, before anything was measured: what will be
measured, with which program, under which conditions, and how results will be judged
comparable. Section 9 records the deviations from that plan decided after the pilot and
**before** the campaign was run. The campaign itself is `scripts/run_part_d.sh`; its results
and report are `results/part_d/` and `reports/part_d_report.md`. Requirements: `assignment2.pdf` pp. 5–6 (D.1–D.3); blueprint §9; measurement rules
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

## 9. Deviations from the plan, recorded after the pilot and before the campaign (decision D-068)

The pilot (`scripts/run_part_d.sh pilot`, kept in `results/part_d_pilot/`) ran one repetition
of every stage with 1 s windows and passed: output schema, validation, histograms, perf stat,
flame graphs, perf c2c, aggregation and all twelve figures. What it showed led to these
changes; none of them alters a requirement of D.1–D.3.

1. **The cleaner thread cannot be pinned, so the "cleaner on CPU 14" set of §1 is dropped.**
   The store owns that thread and has no affinity interface; adding one would be a change to
   the store made for a benchmark. The cleaner runs wherever the kernel puts it (recorded).
2. **Measured windows of 3 s for D.2 and D.3 (5 s for D.1), as planned; reclamation disabled
   gets 1 s** because it retains every version (measured in the pilot: about 4.4 GB in 1.5 s
   in workload A).
3. **The memory guard is raised from the harness default of 6 GB to 16 GB** for every RCU run.
   The pilot showed that with an unthrottled writer and 8 readers the cleaner does not keep
   up on this machine (backlog 31,629 versions and 837 MiB held after 1.5 s; the same on the
   audited Part B binary measured alongside, so it is not something Part C introduced). A 5 s
   run is expected to retain a few GB. The guard must not cut such a run short and turn a
   finding into an invalid run; it still protects the machine (27 GiB available).
4. **Workload B is fixed as:** 2,000 keys, 4 KiB values, 8 readers, unthrottled writer, a
   Snapshot held 20 ms out of every 50 ms, a final snapshot released at the end of the window,
   and a cleanup phase measured separately (time to drain; reader stalls during it). Program:
   `benchmarks/reclaim_bench.cpp`.
5. **Additions to §3:** RCU with no writer at 1 and 8 readers (an upper bound), and each store
   at 8 readers with the generator's own key distribution *and* a paced writer (hit and miss
   rates with a writer present).
6. **perf c2c** is taken for RCU at 8 readers (writer unthrottled; reads only), RCU at 1
   reader, and the shared-mutex store at 8 readers. Flame graphs: RCU at 1/2/4/8 readers with
   the unthrottled writer, RCU at 8 with the paced writer and with none, both lock stores at
   8 readers, and the join with 8 joiners and the writer active.
7. **`perf stat` wraps every track C, D.2 and D.3 run** (counting mode), together with
   `/usr/bin/time` for the peak resident set, so CPU utilization, context switches and memory
   come from the same runs as the throughput numbers.
8. **No optimization is attempted in this milestone.** If perf c2c confirms the cache-line
   hypothesis it is reported as evidence, with the experiment that would test a fix; the store
   measured is the audited Part C store.

## 10. Corrected campaign: unwrapped timing, separate profiling (decision D-071; written before it was run)

**Why a second campaign.** Deviation 7 of §9 was a mistake in method. A check made after the
first campaign (`results/part_d_diagnostics/perf_stat_wrapper_effect.txt`, decision D-070)
showed that running a benchmark under `perf stat` changes the result where threads block
often: global mutex at 8 readers about 20 % higher, stop-the-world read throughput about one
half, shared mutex up to 6 % lower, RCU within 4 %. Counting mode is not free: the kernel
saves and restores the counters at every context switch, and a contended lock makes millions
of them. The first campaign's timing figures are therefore superseded. Its data stays, moved
unchanged (same Git tree, `607e836…`, as `results/part_d/` at tag `part-d-final`) to
`results/part_d/original-wrapped-20261013T090846-615f4357f015/`.

**What changes.**

1. **Timing track.** Every measured executable is started directly by the campaign script:
   no `perf`, no `/usr/bin/time`, no tracer. (`taskset` for the supplied drivers stays: it
   sets the affinity mask and then *becomes* the driver; nothing remains around it.) These
   runs are the only source of throughput, latency, stall, backlog and store-memory figures.
   Each run's file records `# wrapper: none`, its command, start and end time, exit status.
2. **Profiling track**, run after all timing stages, under `<campaign>/profiling/`:
   - `perf_stat/`: every track C, D.2 and D.3 cell again, 2 repetitions, under `perf stat`
     with `/usr/bin/time` (counters, CPUs used, context switches, peak resident set);
   - `flamegraphs/`: `perf record` at 1,999 Hz with frame pointers: RCU at 1/2/4/8 readers
     with the unthrottled writer, RCU at 8 with the paced writer / no writer / the
     stop-the-world comparator (new), both lock stores at 8 readers, the join with 8 joiners;
   - `c2c/`: `perf c2c` for RCU at 8 readers (writer, no writer), RCU at 1 reader, the
     shared mutex and (new) the global mutex at 8 readers.
   The throughput printed by a profiled run is reported in one place only: a table of what
   each profiler does to the cell it observes.
3. **Peak resident set** is no longer available for timing runs (it came from
   `/usr/bin/time`); memory in the timing tables is the store's own accounting, and peak RSS
   is quoted from the profiling track, labelled.
4. **Everything else is unchanged**: matrix, programs, arguments, CPU placement, five
   repetitions, windows (5 s D.1; 3 s D.2/D.3; 1 s with reclamation disabled), 16 GB memory
   guard (8 GB for workload B disabled), repetitions outermost, conditions recorded after
   every repetition (now also swap, a 1 s idle sample and the busiest process), INVALID
   marker on any change of power source or governor.
5. **A campaign directory is never reused**: `run_part_d.sh all` creates
   `results/part_d/unwrapped-<start time>-<commit>/` and refuses an existing one.
6. **Aggregator audits added**: a timing run whose recorded command starts a profiler; a
   duplicate run id; a malformed row; every reported p50/p99/p99.9 recomputed from the run's
   raw histogram.
7. **No ratio mixes the two campaigns.** A separate table sets original beside corrected to
   show what the wrapper did; all results come from the corrected campaign alone.
8. `reclaim_bench` validation (corrected in `e09da24`) is kept, and one assertion added: in
   stop-the-world mode, a retire list that reached the trigger must have been swept. The
   store is unchanged.

Pilot of the corrected method: `results/part_d_pilot_unwrapped/` (one repetition, 1 s
windows; passed).

## 11. Lock placement: found by the first unwrapped campaign, controlled in the final one (decision D-072; written before the final campaign)

**What the first unwrapped campaign (`unwrapped-20261013T105136-146b851dc3dd`) showed.** Its
shared-mutex store at 8 readers with a paced writer ran at 34 M reads/s with a healthy writer,
where every earlier run of that cell, wrapped or not, had given 21–22 M with a starved writer.
Controlled runs (`results/part_d_diagnostics/lock_placement_effect.txt`,
`rwlock_r8_paced_two_regimes.txt`) found the cause: **the position of the store's lock object
inside its 64-byte cache line.** Varying nothing but the length of a label on the command line
(which changes a few heap allocations and so moves the store) moves the lock among offsets
0, 16, 32 and 48, and:

| Lock offset in its line | shared mutex, 8 readers, paced writer | global mutex, same |
|---|---|---|
| 48 | 33–34 M reads/s, writer gets its 10,000/s | 11.1–11.8 M |
| 0, 16, 32 | 21–23 M reads/s, writer starved (1,700–4,700/s) | 9.2–9.8 M |

18 of 18 label lengths and, with the new placement option, 16 of 16 runs follow that rule.
The same store, the same binary, the same arguments apart from a label.

**Consequences.**

- The lock-based cells of both earlier campaigns were measured at whatever offset each
  cell's own command line produced; the offset differed from cell to cell (run identifiers
  and paths differ in length), so their scaling curves mix the two speeds. Neither campaign
  is a valid source for the lock-based stores' scaling.
- The `perf stat` effect of D-070 is real and separate: with identical arguments, hence an
  identical offset, the wrapper still raises the global mutex by about 22 % and lowers the
  shared mutex by 2–6 % and the stop-the-world comparator by 20–50 %.
- The RCU store's readers take no lock; its cells agree across all three campaigns.

**What the final campaign does.**

1. `concurrent_bench --lock-offset N` (benchmark only): the store is constructed, and if its
   lock is not at offset N it is destroyed and constructed again after a small allocation.
   The store's code is untouched.
2. Every track C cell of the global-mutex and shared-mutex stores is measured **at both
   placements**: offset 48 (scenario `<name>`) and offset 0 (`<name>_o0`; a cache-line-aligned
   lock, and representative of 16 and 32). The harness records the offset of every run and
   the aggregator rejects a lock-store run at any other offset.
3. Track R cannot be controlled (the generator's arguments are fixed and the supplied file is
   not edited); the drivers now print the offset they got, and it is reported beside their
   numbers (`rwlock` driver: 0; `global_lock` driver: 48).
4. The stop-the-world comparator's own lock is inside the RCU engine and is not placed; its
   read throughput varied between 5.6 and 7.4 M/s with the label length. It is a comparator
   and its stall, not its throughput, is what D.2 and D.3 use; the variation is reported.
5. Profiling adds, for both lock stores at 8 readers with the paced writer, a flame graph and
   a `perf c2c` report at each placement.

**Why offset 48 is faster: a hypothesis, not a result.** At 48 only the first 16 bytes of the
lock (the words every lock and unlock modifies) are in one line and everything from byte 16
on is in the next. glibc's `pthread_mutex_lock` first reads `__kind` (byte 16) and
`pthread_rwlock_rdlock` first reads `__cur_writer` (byte 24) before the atomic update of the
word at byte 0. With both on one line the read fetches the contended line shared and the
update must then upgrade it; apart, the read is served from a line nobody writes. This fits
all eight placements measured; it has not been isolated by an experiment.

Part A's accepted results (`results/part_a/`) were measured without placement control and
are not changed; the same sensitivity applies to them.

