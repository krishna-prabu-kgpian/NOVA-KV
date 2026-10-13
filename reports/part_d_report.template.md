---
title: "NOVA-KV — Part D: Benchmarking Everything Together (corrected campaign)"
---

Campaign `{{ CAMPAIGN }}` (directory `results/part_d/{{ PRIMARY }}/`), measured at commit
`fcd2938`. Every number below is the **median of 5 independent processes, with [min–max]**
where given. This file is generated from `reports/part_d_report.template.md` by
`scripts/render_part_d_report.py`: each number is computed from the campaign's summary CSVs,
which `scripts/aggregate_part_d.py` derives from the raw runs; none is typed in. Nothing was
estimated and no run was dropped. {{ count('d[123]/*/stdout/*.txt') }} timing runs and
{{ count('profiling/perf_stat/*/*/stdout/*.txt') }} profiled runs, 0 failures, 0 problems
found by the completeness check.

# 0. Why this is the third measurement, and what changed

This report replaces the one tagged `part-d-final`. Two errors of method were found in the
first campaign, the second of them only because the first was being corrected.

**1. The timed runs were inside `perf stat`.** The first campaign
(`20261013T090846-615f4357f015-all`) wrapped every timed run in `perf stat` so that counters
and timings would come from the same process. That changes the timing where threads block
often. Here the timed executables are **started directly**, by nothing but the shell, and all
profiling is done afterwards in separate runs whose throughput is never used as a result
(§1, §5). Measured in this campaign, same cell, same lock placement, with and without the
wrapper: global mutex at 8 readers {{ n2(oh('D.1','global_lock; writer_paced; 8 reader(s)','reads/s')) }}×
(paced writer); shared mutex with an unthrottled writer and 1 to 4 readers
{{ n2(oh('D.1','rwlock; writer_max; 1 reader(s)','reads/s')) }}–{{ n2(oh('D.1','rwlock; writer_max; 2 reader(s)','reads/s')) }}×;
stop-the-world {{ n2(oh('D.2 A','stop-the-world','reads/s')) }}× (workload A) and
{{ n2(oh('D.2 B','stop-the-world','reads/s')) }}× (workload B); RCU at 8 readers
{{ n2(oh('D.1','rcu; writer_paced; 8 reader(s)','reads/s')) }}× and {{ n2(oh('D.1','rcu; writer_max; 8 reader(s)','reads/s')) }}×.

**2. The position of the lock inside its cache line was not controlled, and it decides the
speed of the lock-based stores.** The second campaign (`20261013T105136-146b851dc3dd-all`,
unwrapped) gave {{ M(cmp3('rwlock; writer_paced; 8 reader(s)','reads/s','middle_unwrapped')) }} M reads/s
for the shared mutex at 8 readers where the first had given
{{ M(cmp3('rwlock; writer_paced; 8 reader(s)','reads/s','original_wrapped')) }} M. The cause was not the
wrapper. The lock object is the first member of the store; where it lands in a 64-byte cache
line depends on the heap, and the heap depends on the lengths of the strings on the command
line. Changing only the length of a label moves the lock, and the store runs at one of two
speeds (`results/part_d_diagnostics/lock_placement_effect.txt`: 18 of 18 runs, and 16 of 16
with explicit placement, follow the rule). In the first two campaigns each cell had its own
accidental offset (in that cell: {{ cmp3('rwlock; writer_paced; 8 reader(s)','reads/s','original_lock_offset') }}
in the first campaign, {{ cmp3('rwlock; writer_paced; 8 reader(s)','reads/s','middle_lock_offset') }} in the
second), so their lock-store curves mix the two speeds. In this campaign the benchmark places
the lock explicitly and **every lock-store cell is measured at offset 48 and at offset 0**
(§1). The store's code is unchanged.

**What changed in the conclusions.**

| Conclusion of the first report | Now |
|---|---|
| RCU meets 100,000 reads/s at 8 readers | unchanged ({{ M(d1('rcu','writer_paced',8,'read_rps_median')) }} M paced, {{ M(d1('rcu','writer_max',8,'read_rps_median')) }} M unthrottled) |
| RCU scales to 8 readers; the lock stores stop at 4 | unchanged in substance; the lock stores' numbers changed (§2) |
| "The shared mutex starves its writer at 8 readers" | **true only at one lock placement**: at offset 0 the paced writer gets {{ n0(d1('rwlock','writer_paced_o0',8,'write_rps_median')) }} of 10,000 updates/s, at offset 48 all 10,000 |
| "The global mutex reaches 21.7 M reads/s at 4 readers and stops scaling after 4" | **not reproduced**: {{ M(d1('global_lock','writer_paced',4,'read_rps_median')) }} M (offset 48), {{ M(d1('global_lock','writer_paced_o0',4,'read_rps_median')) }} M (offset 0); it gains nothing from more readers. Neither the wrapper nor the placement accounts for the earlier figure (§7) |
| "The reclaim batch changes reclamation throughput and memory" | **withdrawn**: no effect of the batch on any measure (§3) |
| "Stop-the-world reads at one eleventh of the bounded rate" | **corrected**: about one seventh ({{ M(d2a('reclaim_stw','read_rps_median')) }} against {{ M(d2a('batch_64','read_rps_median')) }} M/s) |
| Bounded reclamation reduces the worst reader pause 28× / 87× | **{{ n0(d2a('reclaim_stw','max_stall_ns_median')/worst_batch('A')[1]) }}× / {{ n0(d2b('reclaim_stw','max_stall_ns_median')/worst_batch('B')[1]) }}×** |
| Transactions, joins, copy-on-write costs | unchanged within a few percent (§4) |

All three campaigns are kept: `results/part_d/README.md`; the cell-by-cell comparison is
`results/part_d/summary/original_vs_corrected.md`. No ratio in this report mixes campaigns.

# 1. Environment and method

**Machine.** AMD Ryzen AI 7 350, 8 cores / 16 logical CPUs (cores 0, 2, 4, 6 reach 5.09 GHz;
cores 1, 3, 5, 7 reach 3.51 GHz; CPU *N*+8 is the SMT sibling of *N*), L3 16 MiB, 30 GiB RAM,
8 GiB swap (unused). Ubuntu 26.04, Linux 7.0.0-28, GCC 15.2.0, Release build `-O3 -g -DNDEBUG
-std=c++17 -fno-omit-frame-pointer`. Mains power, `performance` governor and EPP on all CPUs
at each of {{ ncond() }} checkpoints (start, after every repetition, end);
`kernel.perf_event_paranoid = 1`. Idle CPU sampled at every checkpoint:
{{ n0(min(condall(r'idle_pct_1s=(\d+)'))) }}–{{ n0(max(condall(r'idle_pct_1s=(\d+)'))) }} %; no
process other than the session's own terminal above 5 % CPU. Battery full; ACPI temperature
{{ n0(min(condall(r'temp=(\d+)C'))) }}–{{ n0(max(condall(r'temp=(\d+)C'))) }} °C. Campaign from
{{ cond(r'^(\S+)', 0) }} to {{ cond(r'^(\S+)', -1) }} (`conditions.log`, `env/`).

**Timing track and profiling track.**

- **Timing.** Each measured executable is started directly: no `perf`, no `/usr/bin/time`, no
  tracer (each run's file records its command and `# wrapper: none`; the aggregator and a
  verification gate reject a timing run started any other way). Throughput, latency, stalls,
  backlog and store memory come only from these runs.
- **Profiling**, after all timing stages, under `profiling/`: every track C, D.2 and D.3 cell
  again under `perf stat` with `/usr/bin/time` (2 repetitions: counters, context switches, peak
  resident set); `perf record` at 1,999 Hz with frame pointers for 14 flame graphs; `perf c2c`
  for 9 cases. Their throughput appears only in §5.

**Lock placement.** `concurrent_bench --lock-offset N` constructs the store and, if its lock
object is not at offset N of a cache line, destroys it and constructs it again after a small
allocation. Global-mutex and shared-mutex cells are measured at **offset 48** (the first 16
bytes of the lock end one line, the rest begins the next) and at **offset 0** (cache-line
aligned; offsets 16 and 32 behaved like 0 in the diagnostic). Neither is "the" result and
neither is better everywhere. Where a single lock-store number is compared with RCU, the
higher of the two is used. The RCU store's readers take no lock.

**Thread placement.** Reader (or joiner) thread *i* is pinned to the *i*-th of CPUs 0, 2, 4, 6,
1, 3, 5, 7: one logical CPU per physical core, fast cores first. The writer is pinned to CPU
15. The store's cleaner thread is not pinned (the store has no interface for it).

**Two workload tracks, never in one table or figure.**

- **Track R — supplied generator, preload-only.** The four drivers linked with the unmodified
  `supplied/workload_gen.cpp`, default parameters. Its writer loads all trades and *finishes
  before any reader starts*: there is no writer in these numbers. 20 % of its reads use
  `rng() % (m·n)` while trade ids are `i·100000 + j`, so only 100 of those 100,000 possible keys
  exist: measured hit rate **{{ pct(ref('rcu',8,'expected_overall_hit_rate'),2) }} %**.
- **Track C — writer active.** `benchmarks/concurrent_bench.cpp`: the same schema, hot set and
  80/20 mix, every requested key present, and one writer thread overwriting existing trades
  for the whole measured window (0.5 s warm-up, 5 s measured). Every read is validated; the
  writer's successful updates are counted. Two writer modes: paced at 10,000 updates/s, and
  unthrottled.

D.1 asks for the generator "with one background writer thread active throughout". The supplied
generator cannot do that, so both tracks are reported and labelled.

**Caveats that apply to all throughput numbers.** Both harnesses time every `get` with two
clock reads; in the profile of the RCU store with no writer those clock reads are
{{ n0(fgshare('rcu_r8_readonly','clock reads')) }} % of all cycles. The numbers are therefore
lower bounds on what the stores can do, equally for all four.

**Comparing RCU with the lock variants** compares two whole designs. The Part A stores update
one slot in place under a lock; the RCU store copies the 24 KiB segment it touches and a 2 KiB
directory for every update and reclaims the old ones later. The difference between them is not
the cost of a synchronization primitive alone.

# 2. D.1 — point-query throughput and the 100,000 RPS target

## 2.1 Track C: one writer active throughout

![](../results/part_d/figures/d1_read_throughput.png){width=100%}

**Reads per second (millions), median [min–max]; writer paced at 10,000 updates/s:**

| Readers | global mutex, lock at 48 | global mutex, lock at 0 | shared mutex, lock at 48 | shared mutex, lock at 0 | RCU |
|---:|---:|---:|---:|---:|---:|
| 1 | {{ rng('global_lock','writer_paced',1) }} | {{ rng('global_lock','writer_paced_o0',1) }} | {{ rng('rwlock','writer_paced',1) }} | {{ rng('rwlock','writer_paced_o0',1) }} | {{ rng('rcu','writer_paced',1) }} |
| 2 | {{ rng('global_lock','writer_paced',2) }} | {{ rng('global_lock','writer_paced_o0',2) }} | {{ rng('rwlock','writer_paced',2) }} | {{ rng('rwlock','writer_paced_o0',2) }} | {{ rng('rcu','writer_paced',2) }} |
| 4 | {{ rng('global_lock','writer_paced',4) }} | {{ rng('global_lock','writer_paced_o0',4) }} | {{ rng('rwlock','writer_paced',4) }} | {{ rng('rwlock','writer_paced_o0',4) }} | {{ rng('rcu','writer_paced',4) }} |
| 8 | {{ rng('global_lock','writer_paced',8) }} | {{ rng('global_lock','writer_paced_o0',8) }} | {{ rng('rwlock','writer_paced',8) }} | {{ rng('rwlock','writer_paced_o0',8) }} | **{{ rng('rcu','writer_paced',8) }}** |

**Writer unthrottled:**

| Readers | global mutex, lock at 48 | global mutex, lock at 0 | shared mutex, lock at 48 | shared mutex, lock at 0 | RCU |
|---:|---:|---:|---:|---:|---:|
| 1 | {{ rng('global_lock','writer_max',1) }} | {{ rng('global_lock','writer_max_o0',1) }} | {{ rng('rwlock','writer_max',1) }} | {{ rng('rwlock','writer_max_o0',1) }} | {{ rng('rcu','writer_max',1) }} |
| 2 | {{ rng('global_lock','writer_max',2) }} | {{ rng('global_lock','writer_max_o0',2) }} | {{ rng('rwlock','writer_max',2) }} | {{ rng('rwlock','writer_max_o0',2) }} | {{ rng('rcu','writer_max',2) }} |
| 4 | {{ rng('global_lock','writer_max',4) }} | {{ rng('global_lock','writer_max_o0',4) }} | {{ rng('rwlock','writer_max',4) }} | {{ rng('rwlock','writer_max_o0',4) }} | {{ rng('rcu','writer_max',4) }} |
| 8 | {{ rng('global_lock','writer_max',8) }} | {{ rng('global_lock','writer_max_o0',8) }} | {{ rng('rwlock','writer_max',8) }} | {{ rng('rwlock','writer_max_o0',8) }} | **{{ rng('rcu','writer_max',8) }}** |

Every read in these tables found its key (hit rate 100 %; reads attempted = reads
successful).

**Read latency at 8 readers (µs):**

| Store, writer | p50 | p99 | p99.9 | max, median run / worst run |
|---|---:|---:|---:|---:|
| global mutex at 48, paced | {{ us(d1('global_lock','writer_paced',8,'p50_ns_median')) }} | {{ us(d1('global_lock','writer_paced',8,'p99_ns_median')) }} | {{ us(d1('global_lock','writer_paced',8,'p999_ns_median')) }} | {{ us(d1('global_lock','writer_paced',8,'max_ns_median'),0) }} / {{ us(d1('global_lock','writer_paced',8,'max_ns_worst'),0) }} |
| global mutex at 0, paced | {{ us(d1('global_lock','writer_paced_o0',8,'p50_ns_median')) }} | {{ us(d1('global_lock','writer_paced_o0',8,'p99_ns_median')) }} | {{ us(d1('global_lock','writer_paced_o0',8,'p999_ns_median')) }} | {{ us(d1('global_lock','writer_paced_o0',8,'max_ns_median'),0) }} / {{ us(d1('global_lock','writer_paced_o0',8,'max_ns_worst'),0) }} |
| shared mutex at 48, paced | {{ us(d1('rwlock','writer_paced',8,'p50_ns_median')) }} | {{ us(d1('rwlock','writer_paced',8,'p99_ns_median')) }} | {{ us(d1('rwlock','writer_paced',8,'p999_ns_median')) }} | {{ us(d1('rwlock','writer_paced',8,'max_ns_median'),0) }} / {{ us(d1('rwlock','writer_paced',8,'max_ns_worst'),0) }} |
| shared mutex at 0, paced | {{ us(d1('rwlock','writer_paced_o0',8,'p50_ns_median')) }} | {{ us(d1('rwlock','writer_paced_o0',8,'p99_ns_median')) }} | {{ us(d1('rwlock','writer_paced_o0',8,'p999_ns_median')) }} | {{ us(d1('rwlock','writer_paced_o0',8,'max_ns_median'),0) }} / {{ us(d1('rwlock','writer_paced_o0',8,'max_ns_worst'),0) }} |
| RCU, paced | {{ us(d1('rcu','writer_paced',8,'p50_ns_median')) }} | {{ us(d1('rcu','writer_paced',8,'p99_ns_median')) }} | {{ us(d1('rcu','writer_paced',8,'p999_ns_median')) }} | {{ us(d1('rcu','writer_paced',8,'max_ns_median'),0) }} / {{ us(d1('rcu','writer_paced',8,'max_ns_worst'),0) }} |
| global mutex at 48, unthrottled | {{ us(d1('global_lock','writer_max',8,'p50_ns_median')) }} | {{ us(d1('global_lock','writer_max',8,'p99_ns_median')) }} | {{ us(d1('global_lock','writer_max',8,'p999_ns_median')) }} | {{ us(d1('global_lock','writer_max',8,'max_ns_median'),0) }} / {{ us(d1('global_lock','writer_max',8,'max_ns_worst'),0) }} |
| global mutex at 0, unthrottled | {{ us(d1('global_lock','writer_max_o0',8,'p50_ns_median')) }} | {{ us(d1('global_lock','writer_max_o0',8,'p99_ns_median')) }} | {{ us(d1('global_lock','writer_max_o0',8,'p999_ns_median')) }} | {{ us(d1('global_lock','writer_max_o0',8,'max_ns_median'),0) }} / {{ us(d1('global_lock','writer_max_o0',8,'max_ns_worst'),0) }} |
| shared mutex at 48, unthrottled | {{ us(d1('rwlock','writer_max',8,'p50_ns_median')) }} | {{ us(d1('rwlock','writer_max',8,'p99_ns_median')) }} | {{ us(d1('rwlock','writer_max',8,'p999_ns_median')) }} | {{ us(d1('rwlock','writer_max',8,'max_ns_median'),0) }} / {{ us(d1('rwlock','writer_max',8,'max_ns_worst'),0) }} |
| shared mutex at 0, unthrottled | {{ us(d1('rwlock','writer_max_o0',8,'p50_ns_median')) }} | {{ us(d1('rwlock','writer_max_o0',8,'p99_ns_median')) }} | {{ us(d1('rwlock','writer_max_o0',8,'p999_ns_median')) }} | {{ us(d1('rwlock','writer_max_o0',8,'max_ns_median'),0) }} / {{ us(d1('rwlock','writer_max_o0',8,'max_ns_worst'),0) }} |
| RCU, unthrottled | {{ us(d1('rcu','writer_max',8,'p50_ns_median')) }} | {{ us(d1('rcu','writer_max',8,'p99_ns_median')) }} | {{ us(d1('rcu','writer_max',8,'p999_ns_median')) }} | {{ us(d1('rcu','writer_max',8,'max_ns_median'),0) }} / {{ us(d1('rcu','writer_max',8,'max_ns_worst'),0) }} |

The maximum of a run is one sample in several hundred million. In this session it is about
140–160 µs in almost every cell, including one reader on an otherwise idle machine and the
store with no writer at all, so that floor belongs to the machine, not to any store.

![](../results/part_d/figures/d1_tail_latency.png){width=100%}

**Successful writer updates per second:**

| Readers | global mutex at 48, unthrottled | global mutex at 0, unthrottled | shared mutex at 48, unthrottled | shared mutex at 0, unthrottled | RCU, unthrottled | shared mutex at 0, **paced** (target 10,000) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | {{ wr('global_lock','writer_max',1) }} | {{ wr('global_lock','writer_max_o0',1) }} | {{ wr('rwlock','writer_max',1) }} | {{ wr('rwlock','writer_max_o0',1) }} | {{ wr('rcu','writer_max',1) }} | {{ wr('rwlock','writer_paced_o0',1) }} |
| 2 | {{ wr('global_lock','writer_max',2) }} | {{ wr('global_lock','writer_max_o0',2) }} | {{ wr('rwlock','writer_max',2) }} | {{ wr('rwlock','writer_max_o0',2) }} | {{ wr('rcu','writer_max',2) }} | {{ wr('rwlock','writer_paced_o0',2) }} |
| 4 | {{ wr('global_lock','writer_max',4) }} | {{ wr('global_lock','writer_max_o0',4) }} | {{ wr('rwlock','writer_max',4) }} | {{ wr('rwlock','writer_max_o0',4) }} | {{ wr('rcu','writer_max',4) }} | {{ wr('rwlock','writer_paced_o0',4) }} |
| 8 | {{ wr('global_lock','writer_max',8) }} | {{ wr('global_lock','writer_max_o0',8) }} | {{ wr('rwlock','writer_max',8) }} | **{{ wr('rwlock','writer_max_o0',8) }}** | {{ wr('rcu','writer_max',8) }} | **{{ wr('rwlock','writer_paced_o0',8) }}** |

Every other paced cell delivers its 10,000 updates/s (medians {{ n0(min(d1(v,s,n,'write_rps_median') for v,s in (('global_lock','writer_paced'),('global_lock','writer_paced_o0'),('rwlock','writer_paced'),('rcu','writer_paced')) for n in (1,2,4,8))) }}–{{ n0(max(d1(v,s,n,'write_rps_median') for v,s in (('global_lock','writer_paced'),('global_lock','writer_paced_o0'),('rwlock','writer_paced'),('rcu','writer_paced')) for n in (1,2,4,8))) }}).

![](../results/part_d/figures/d1_writer_throughput.png){width=100%}

One thread only: the unsynchronized store reads {{ M(d1('single','readonly',1,'read_rps_median')) }} M/s
alone and {{ M(d1('single','inline_writer',1,'read_rps_median')) }} M/s while doing one update per 100
reads on the same thread; it is never run with concurrent threads. The RCU store with no
writer reads {{ M(d1('rcu','readonly',1,'read_rps_median')) }} M/s on one thread and
{{ M(d1('rcu','readonly',8,'read_rps_median')) }} M/s on eight.

With the generator's own key distribution *and* a paced writer (8 readers), lookups/s
attempted and reads/s successful, hit rate {{ pct(d1('rcu','refkeys_paced',8,'hit_rate_median'),2) }} % in
every case: global mutex {{ M(d1('global_lock','refkeys_paced',8,'read_rps_median')) }} / {{ M(d1('global_lock','refkeys_paced',8,'successful_reads_per_s_median')) }} M
(lock at 48) and {{ M(d1('global_lock','refkeys_paced_o0',8,'read_rps_median')) }} / {{ M(d1('global_lock','refkeys_paced_o0',8,'successful_reads_per_s_median')) }} M
(at 0); shared mutex {{ M(d1('rwlock','refkeys_paced',8,'read_rps_median')) }} / {{ M(d1('rwlock','refkeys_paced',8,'successful_reads_per_s_median')) }} M
and {{ M(d1('rwlock','refkeys_paced_o0',8,'read_rps_median')) }} / {{ M(d1('rwlock','refkeys_paced_o0',8,'successful_reads_per_s_median')) }} M;
RCU {{ M(d1('rcu','refkeys_paced',8,'read_rps_median')) }} / {{ M(d1('rcu','refkeys_paced',8,'successful_reads_per_s_median')) }} M.

## 2.2 Where the lock-based stores stop scaling

Rule used: the largest reader count whose median is more than 5 % above the best median at
any smaller count.

- **Global mutex: it does not scale.** With the paced writer the best count is
  {{ stop_after('global_lock','writer_paced') }} reader at offset 48 and
  {{ stop_after('global_lock','writer_paced_o0') }} at offset 0: {{ M(d1('global_lock','writer_paced',1,'read_rps_median')) }},
  {{ M(d1('global_lock','writer_paced',2,'read_rps_median')) }}, {{ M(d1('global_lock','writer_paced',4,'read_rps_median')) }} M/s at 1, 2, 4
  readers, then a **fall** to {{ M(d1('global_lock','writer_paced',8,'read_rps_median')) }} M/s at 8
  ({{ M(d1('global_lock','writer_paced_o0',8,'read_rps_median')) }} M/s at offset 0). With the unthrottled
  writer it rises until {{ stop_after('global_lock','writer_max') }} readers
  ({{ M(d1('global_lock','writer_max',1,'read_rps_median')) }} → {{ M(d1('global_lock','writer_max',4,'read_rps_median')) }} M/s)
  only because more readers take the lock from the writer more often: the writer falls from
  {{ n0(d1('global_lock','writer_max',1,'write_rps_median')) }} to {{ n0(d1('global_lock','writer_max',8,'write_rps_median')) }}
  updates/s. Under `perf record` at 8 readers,
  {{ n0(fgshare('global_lock_r8_writer_max','kernel')) }} % of sampled on-CPU cycles are in the kernel
  (futex wait and wake), {{ n0(fgshare('global_lock_r8_writer_max','lock functions')) }} % in the mutex
  functions and {{ n1(fgshare('global_lock_r8_writer_max','reader: store get')) }} % in the lookup
  (`profiling/flamegraphs/global_lock_r8_writer_max.svg`). p99 read latency grows from
  {{ us(d1('global_lock','writer_paced',1,'p99_ns_median'),2) }} µs at 1 reader to
  {{ us(d1('global_lock','writer_paced',8,'p99_ns_median'),1) }} µs at 8.
- **Shared mutex: after {{ stop_after('rwlock','writer_paced') }} readers with a paced writer, at both
  placements, but what happens at 8 differs.** At offset 48:
  {{ M(d1('rwlock','writer_paced',1,'read_rps_median')) }} → {{ M(d1('rwlock','writer_paced',2,'read_rps_median')) }} →
  {{ M(d1('rwlock','writer_paced',4,'read_rps_median')) }} → {{ M(d1('rwlock','writer_paced',8,'read_rps_median')) }} M/s:
  a plateau, and the writer keeps its 10,000 updates/s. At offset 0:
  {{ M(d1('rwlock','writer_paced_o0',1,'read_rps_median')) }} → {{ M(d1('rwlock','writer_paced_o0',2,'read_rps_median')) }} →
  {{ M(d1('rwlock','writer_paced_o0',4,'read_rps_median')) }} → {{ M(d1('rwlock','writer_paced_o0',8,'read_rps_median')) }} M/s:
  a fall, and **the writer is starved** ({{ wr('rwlock','writer_paced_o0',8) }} of 10,000). Readers do
  not exclude each other, but every `get` writes the lock's reader count twice: at 8 readers
  {{ n0(fgshare('rwlock_r8_writer_paced','lock functions')) }} % (offset 48) and
  {{ n0(fgshare('rwlock_r8_writer_paced_o0','lock functions')) }} % (offset 0) of on-CPU cycles are in
  `pthread_rwlock_rdlock`/`unlock`.
  With an unthrottled writer and few readers the opposite happens: the writer holds the lock
  and reads collapse to {{ M(d1('rwlock','writer_max',1,'read_rps_median')) }} M/s at 1 reader. The rise to
  {{ M(d1('rwlock','writer_max',8,'read_rps_median')) }} M/s (offset 48) or {{ M(d1('rwlock','writer_max_o0',8,'read_rps_median')) }} M/s
  (offset 0) at 8 readers is the readers taking the lock away from the writer, not parallel
  speed-up: at offset 0 the writer is left with {{ n0(d1('rwlock','writer_max_o0',8,'write_rps_median')) }}
  updates/s, at offset 48 with {{ n0(d1('rwlock','writer_max',8,'write_rps_median')) }}.
- **RCU: still rising at 8 readers** in both writer modes:
  {{ M(d1('rcu','writer_paced',1,'read_rps_median')) }} → {{ M(d1('rcu','writer_paced',2,'read_rps_median')) }} →
  {{ M(d1('rcu','writer_paced',4,'read_rps_median')) }} → {{ M(d1('rcu','writer_paced',8,'read_rps_median')) }} M/s paced
  ({{ n1(d1('rcu','writer_paced',8,'read_rps_median')/d1('rcu','writer_paced',1,'read_rps_median')) }}× from 1 to 8
  readers; the last four readers run on the slower cores and one shares its core with the
  writer), {{ M(d1('rcu','writer_max',1,'read_rps_median')) }} → {{ M(d1('rcu','writer_max',2,'read_rps_median')) }} →
  {{ M(d1('rcu','writer_max',4,'read_rps_median')) }} → {{ M(d1('rcu','writer_max',8,'read_rps_median')) }} M/s unthrottled.
  At 8 readers with the paced writer that is
  {{ n1(d1('rcu','writer_paced',8,'read_rps_median')/max(d1('rwlock','writer_paced',8,'read_rps_median'),d1('rwlock','writer_paced_o0',8,'read_rps_median'))) }}×
  the better shared-mutex placement and
  {{ n1(d1('rcu','writer_paced',8,'read_rps_median')/max(d1('global_lock','writer_paced',8,'read_rps_median'),d1('global_lock','writer_paced_o0',8,'read_rps_median'))) }}×
  the better global-mutex placement.

**The lock's position in its cache line.** Same binary, same arguments, 8 readers, paced
writer:

| | lock at offset 48 | lock at offset 0 |
|---|---:|---:|
| shared mutex, reads/s (M) | {{ M(d1('rwlock','writer_paced',8,'read_rps_median')) }} | {{ M(d1('rwlock','writer_paced_o0',8,'read_rps_median')) }} |
| shared mutex, writer updates/s | {{ n0(d1('rwlock','writer_paced',8,'write_rps_median')) }} | {{ n0(d1('rwlock','writer_paced_o0',8,'write_rps_median')) }} |
| shared mutex, sampled loads that hit a line modified by another core (`perf c2c`) | {{ n1(c2csrc('rwlock_r8_writer_paced','HITM')) }} % | {{ n1(c2csrc('rwlock_r8_writer_paced_o0','HITM')) }} % |
| shared mutex, share of those in `pthread_rwlock_unlock`, on one line | {{ n0(c2csym('rwlock_r8_writer_paced','pthread_rwlock_unlock','pct')) }} % | {{ n0(c2csym('rwlock_r8_writer_paced_o0','pthread_rwlock_unlock','pct')) }} % |
| global mutex, reads/s (M) | {{ M(d1('global_lock','writer_paced',8,'read_rps_median')) }} | {{ M(d1('global_lock','writer_paced_o0',8,'read_rps_median')) }} |

At offset 48 only the lock's first 16 bytes (the words every lock and unlock modifies) are in
one cache line and the rest of the lock is in the next. In glibc 2.43 (field offsets checked
on this machine, `results/part_d_diagnostics/glibc_lock_layout.txt`; the order of accesses is
from the glibc sources) `pthread_rwlock_unlock` first reads `__cur_writer` (byte 24) and
`pthread_mutex_lock` first reads `__kind` (byte 16) before the atomic update of the first
word. With both on one line the read fetches the contended
line and the update must then acquire it again; apart, the read is served from a line nobody
writes. The `perf c2c` rows above are consistent with that, and so are all eight placements
tried, but **this explanation is a hypothesis: no experiment here isolates it.** The effect
itself is measured. It is not uniformly in favour of offset 48: with the unthrottled writer the
global mutex is *slower* at 48 ({{ M(d1('global_lock','writer_max',1,'read_rps_median')) }} against
{{ M(d1('global_lock','writer_max_o0',1,'read_rps_median')) }} M/s at 1 reader).

## 2.3 The 100,000 reads/s target

| RCU store, 8 readers | Median reads/s | Minimum of 5 runs | 100,000/s |
|---|---:|---:|---|
| Track C, writer paced at 10,000/s | {{ n0(d1('rcu','writer_paced',8,'read_rps_median')) }} | {{ n0(d1('rcu','writer_paced',8,'read_rps_min')) }} | **met** |
| Track C, writer unthrottled ({{ n0(d1('rcu','writer_max',8,'write_rps_median')) }} updates/s) | {{ n0(d1('rcu','writer_max',8,'read_rps_median')) }} | {{ n0(d1('rcu','writer_max',8,'read_rps_min')) }} | **met** |
| Track R, supplied generator (preload-only) | {{ n0(ref('rcu',8,'rps_median')) }} lookups/s | {{ n0(ref('rcu',8,'rps_min')) }} | **met** |

The target is met by a factor of about {{ n0(d1('rcu','writer_max',8,'read_rps_min')/100000) }} to
{{ n0(d1('rcu','writer_paced',8,'read_rps_median')/100000) }} in every configuration measured, with
p99.9 read latency of {{ us(d1('rcu','writer_paced',8,'p999_ns_median'),2) }} µs (paced writer) and
{{ us(d1('rcu','writer_max',8,'p999_ns_median'),2) }} µs (unthrottled). These are unwrapped runs.
Because the target is met, the assignment's fallback ("report the number you achieved and
explain the remaining bottleneck, backed by a flame graph") does not apply; the flame graphs
were taken anyway and are used in §2.2 and §2.5.

## 2.4 Track R — supplied generator, preload-only

![](../results/part_d/figures/d1_reference_generator.png){width=62%}

| Readers | single thread | global mutex | shared mutex | RCU |
|---:|---:|---:|---:|---:|
| 1 | {{ M(ref('single_thread',1,'rps_median')) }} [{{ M(ref('single_thread',1,'rps_min')) }}–{{ M(ref('single_thread',1,'rps_max')) }}] | {{ M(ref('global_lock',1,'rps_median')) }} [{{ M(ref('global_lock',1,'rps_min')) }}–{{ M(ref('global_lock',1,'rps_max')) }}] | {{ M(ref('rwlock',1,'rps_median')) }} [{{ M(ref('rwlock',1,'rps_min')) }}–{{ M(ref('rwlock',1,'rps_max')) }}] | {{ M(ref('rcu',1,'rps_median')) }} [{{ M(ref('rcu',1,'rps_min')) }}–{{ M(ref('rcu',1,'rps_max')) }}] |
| 2 | — | {{ M(ref('global_lock',2,'rps_median')) }} [{{ M(ref('global_lock',2,'rps_min')) }}–{{ M(ref('global_lock',2,'rps_max')) }}] | {{ M(ref('rwlock',2,'rps_median')) }} [{{ M(ref('rwlock',2,'rps_min')) }}–{{ M(ref('rwlock',2,'rps_max')) }}] | {{ M(ref('rcu',2,'rps_median')) }} [{{ M(ref('rcu',2,'rps_min')) }}–{{ M(ref('rcu',2,'rps_max')) }}] |
| 4 | — | {{ M(ref('global_lock',4,'rps_median')) }} [{{ M(ref('global_lock',4,'rps_min')) }}–{{ M(ref('global_lock',4,'rps_max')) }}] | {{ M(ref('rwlock',4,'rps_median')) }} [{{ M(ref('rwlock',4,'rps_min')) }}–{{ M(ref('rwlock',4,'rps_max')) }}] | {{ M(ref('rcu',4,'rps_median')) }} [{{ M(ref('rcu',4,'rps_min')) }}–{{ M(ref('rcu',4,'rps_max')) }}] |
| 8 | — | {{ M(ref('global_lock',8,'rps_median')) }} [{{ M(ref('global_lock',8,'rps_min')) }}–{{ M(ref('global_lock',8,'rps_max')) }}] | {{ M(ref('rwlock',8,'rps_median')) }} [{{ M(ref('rwlock',8,'rps_min')) }}–{{ M(ref('rwlock',8,'rps_max')) }}] | {{ M(ref('rcu',8,'rps_median')) }} [{{ M(ref('rcu',8,'rps_min')) }}–{{ M(ref('rcu',8,'rps_max')) }}] |

Lookups per second (millions), **no writer running**, {{ pct(ref('rcu',8,'expected_overall_hit_rate'),2) }} %
of lookups hit. The lock placement of the drivers cannot be chosen (their arguments are fixed
and the supplied file is not edited); each driver reports the offset it got: **0 for the
shared-mutex driver, 48 for the global-mutex driver.** The shared-mutex driver's fall from 4 to
8 readers is therefore the offset-0 behaviour of §2.2, and no offset-48 figure exists for it.
The shape is otherwise that of track C: the mutex stores do not gain beyond 4 readers, RCU
keeps rising. p99.9 at 8 readers as printed by the generator:
{{ n1(ref('global_lock',8,'p999_us_median')) }} µs (global mutex), {{ n1(ref('rwlock',8,'p999_us_median')) }} µs
(shared mutex), {{ n1(ref('rcu',8,'p999_us_median')) }} µs (RCU).

## 2.5 What RCU costs, and what limits it

**Memory and reclamation.** Every update copies {{ n0(recl('writer_max',8,'bytes_copied_per_update_median')) }}
bytes (one 24 KiB segment and the 2 KiB directory) for a 32-byte value. With the paced writer
the store holds {{ n0(recl('writer_paced',8,'memory_at_end_mib_median')) }} MiB at the end of every run and
the backlog is empty. **With the unthrottled writer the cleaner does not keep up beyond 2
readers:**

| Readers (unthrottled writer) | Writer updates/s | Memory held at end, MiB (median; max) | Retired versions not yet reclaimed at end | Cleaner CPU, s of 5.5 |
|---:|---:|---:|---:|---:|
| 1 | {{ n0(d1('rcu','writer_max',1,'write_rps_median')) }} | {{ n0(recl('writer_max',1,'memory_at_end_mib_median')) }}; {{ n0(recl('writer_max',1,'memory_at_end_mib_max')) }} | {{ n0(recl('writer_max',1,'backlog_at_end_median')) }} | {{ n2(recl('writer_max',1,'cleaner_cpu_s_median')) }} |
| 2 | {{ n0(d1('rcu','writer_max',2,'write_rps_median')) }} | {{ n0(recl('writer_max',2,'memory_at_end_mib_median')) }}; {{ n0(recl('writer_max',2,'memory_at_end_mib_max')) }} | {{ n0(recl('writer_max',2,'backlog_at_end_median')) }} | {{ n2(recl('writer_max',2,'cleaner_cpu_s_median')) }} |
| 4 | {{ n0(d1('rcu','writer_max',4,'write_rps_median')) }} | {{ n0(recl('writer_max',4,'memory_at_end_mib_median')) }}; {{ n0(recl('writer_max',4,'memory_at_end_mib_max')) }} | {{ n0(recl('writer_max',4,'backlog_at_end_median')) }} | {{ n2(recl('writer_max',4,'cleaner_cpu_s_median')) }} |
| 8 | {{ n0(d1('rcu','writer_max',8,'write_rps_median')) }} | {{ n0(recl('writer_max',8,'memory_at_end_mib_median')) }}; {{ n0(recl('writer_max',8,'memory_at_end_mib_max')) }} | {{ n0(recl('writer_max',8,'backlog_at_end_median')) }} | {{ n2(recl('writer_max',8,'cleaner_cpu_s_median')) }} |

The cleaner thread is busy for {{ n1(recl('writer_max',1,'cleaner_cpu_s_median')) }}–{{ n1(recl('writer_max',4,'cleaner_cpu_s_median')) }}
of the 5.5 s at every reader count. At 8 readers the backlog at the end of a 5 s window is
{{ n0(recl('writer_max',8,'backlog_at_end_median')) }} versions and
{{ n1(recl('writer_max',8,'memory_at_end_mib_median')/1024) }} GiB, about
{{ n0(recl('writer_max',8,'backlog_at_end_median')/5.5) }} versions and
{{ n2(recl('writer_max',8,'memory_at_end_mib_median')/1024/5.5) }} GiB per second, for as long as the writer
runs flat out. The store has no back-pressure (a deliberate choice, decision D-040), so this
ends only when the writer slows down or memory runs out. It drains completely afterwards
(validated in every run). The audited Part B binary behaved the same when run alternately
with the first campaign's binary (a diagnostic outside the campaigns,
`results/part_d_diagnostics/part_b_reference_backlog.txt`), so it is not something Part C
introduced. **A sustained unthrottled writer with 4 or 8 readers is not a steady state for
this store on this machine.** The 10,000 updates/s workload is.

**Reads slow down by half next to an unthrottled writer**
({{ M(d1('rcu','writer_paced',8,'read_rps_median')) }} → {{ M(d1('rcu','writer_max',8,'read_rps_median')) }} M/s at 8
readers; {{ M(d1('rcu','readonly',1,'read_rps_median')) }} → {{ M(d1('rcu','writer_max',1,'read_rps_median')) }} M/s for a
single reader with idle cores all around it). What the profiling track says (counters are per
process and come from profiled runs; RCU cells move by at most a few percent under the
profilers, §5):

- It is not lock or scheduler contention: readers take no lock, and the slowdown is uniform
  across reader threads (slowest/fastest {{ n2(perfcell('rcu','writer_max',8,'slowest')) }}/{{ n2(perfcell('rcu','writer_max',8,'fastest')) }}
  M/s against {{ n2(perfcell('rcu','readonly',8,'slowest')) }}/{{ n2(perfcell('rcu','readonly',8,'fastest')) }} M/s with
  no writer) and present with a single reader.
- Instructions per read hardly change ({{ n0(perfcell('rcu','readonly',8,'instr/read')) }} →
  {{ n0(perfcell('rcu','writer_max',8,'instr/read')) }}); cycles per read go from
  {{ n0(perfcell('rcu','readonly',8,'cycles/read')) }} to {{ n0(perfcell('rcu','writer_max',8,'cycles/read')) }} and
  instructions per cycle from {{ n2(perfcell('rcu','readonly',8,'IPC')) }} to
  {{ n2(perfcell('rcu','writer_max',8,'IPC')) }}. The readers are stalled, not doing more work.
- `perf c2c` (AMD IBS load sampling): of the loads with a known data source, those served from
  L3 rise from {{ n1(c2csrc('rcu_r8_readonly','L3')) }} % to {{ n1(c2csrc('rcu_r8_writer_max','L3')) }} % and
  those that hit a line modified by another core from {{ n1(c2csrc('rcu_r8_readonly','HITM')) }} % to
  {{ n1(c2csrc('rcu_r8_writer_max','HITM')) }} %.
- **The hypothesis that writer-updated segment owner counts share cache lines with reader
  data (audit item R8) is only weakly supported.** Of {{ n0(c2csym('rcu_r8_writer_max','x','total')) }}
  such "modified elsewhere" load samples at 8 readers,
  {{ n0(c2csym('rcu_r8_writer_max','VersionedStore::comm','pct')) }} % were taken by the writer's own commit
  and {{ n0(c2csym('rcu_r8_writer_max','PinnedArena','pct') + c2csym('rcu_r8_writer_max','VersionedStore::run_','pct')) }} %
  by the functions taking and releasing references (cleaner and writer): the writer and the
  cleaner contending with each other on owner and reference counts.
  **{{ n1(c2csym('rcu_r8_writer_max','VersionedStore::read','pct')) }} %
  ({{ n0(c2csym('rcu_r8_writer_max','VersionedStore::read','samples')) }} samples, on
  {{ n0(c2csym('rcu_r8_writer_max','VersionedStore::read','lines')) }} different cache lines) were taken by
  the reader's lookup**, which is
  {{ n2(100*c2csym('rcu_r8_writer_max','VersionedStore::read','samples')/c2c('rcu_r8_writer_max','load_operations')) }} %
  of all sampled loads. Readers do touch lines the writer or cleaner has just modified, but
  too rarely for that to explain a factor of two. The hypothesis is not established.
- What fits all of the above, without being proven by it: copy-on-write *relocates* what
  readers read. Each update publishes a new 24 KiB segment at a new address, so the copy of
  that partition a reader has in its own caches is useless and the next probe has to fetch
  lines from L3 or from the writer's core; at
  {{ n0(d1('rcu','writer_max',8,'write_rps_median')) }} updates/s that is
  {{ n1(d1('rcu','writer_max',8,'write_rps_median')*recl('writer_max',8,'bytes_copied_per_update_median')/1e9) }} GB/s
  of freshly written index flowing through a 16 MiB shared L3.
- The profile cannot separate this further. In the flame graphs the reader threads' cycles
  shift from the lookup ({{ n0(fgshare('rcu_r8_readonly','reader: store get')) }} % →
  {{ n0(fgshare('rcu_r8_writer_max','reader: store get')) }} %) to the harness's clock reads
  ({{ n0(fgshare('rcu_r8_readonly','clock reads')) }} % → {{ n0(fgshare('rcu_r8_writer_max','clock reads')) }} %); a
  timestamp read waits for the loads before it, so stall time is charged to it. That shift is
  an artefact of attribution, not evidence that reading the clock became slower.

**Where the writer's and the cleaner's time goes** (unthrottled writer, share of sampled
on-CPU cycles of the whole process): with 1 reader the cleaner takes
{{ n0(fgshare('rcu_r1_writer_max','cleaner thread')) }} % and the writer's `put`
{{ n0(fgshare('rcu_r1_writer_max','writer: store put')) }} %; with 8 readers
{{ n0(fgshare('rcu_r8_writer_max','cleaner thread')) }} % and {{ n0(fgshare('rcu_r8_writer_max','writer: store put')) }} %
(the readers' share grows; the cleaner still uses a whole core). The cleaner costs more
cycles than the writer whose versions it destroys, which is why it is the one that falls
behind.

No optimization was attempted. A test that would decide the read slowdown: per-thread
hardware counters for the reader threads alone, and the same workload against a store that
updates in place.

# 3. D.2 — reclamation-batch tuning

`set_reclaim_batch(n)`: one reclamation pass frees at most *n* retired versions. Settings 1, 8,
64, 256, 1024; reclamation disabled; and the naive stop-the-world sweep (all readers frozen by a
lock, the whole retire list freed at once, as soon as it holds 1,024 versions). RCU store, 8
readers, unthrottled writer, 3 s windows (1 s with reclamation disabled, which retains
everything; guarded by a 16 GB limit that was never reached). In every epoch-based run the
largest number of versions freed in one pass equalled the batch
({{ n0(d2a('batch_1','max_versions_in_a_pass')) }}, {{ n0(d2a('batch_8','max_versions_in_a_pass')) }},
{{ n0(d2a('batch_64','max_versions_in_a_pass')) }}, {{ n0(d2a('batch_256','max_versions_in_a_pass')) }},
{{ n0(d2a('batch_1024','max_versions_in_a_pass')) }}) and never exceeded it, in both workloads.

Two workloads, **not comparable with each other**:

- **A, steady point gets:** the D.1 benchmark (100,000 trades of 32 bytes).
- **B, backlog-building:** `benchmarks/reclaim_bench.cpp`, 2,000 keys of 4 KiB, and a thread
  that holds a Snapshot for 20 ms out of every 50 ms. While it is held nothing retired after it
  can be freed, so thousands of versions accumulate and are released in passes. At the end the
  writer stops, the last snapshot is released, and the time to drain is measured with the
  readers still running.

## 3.1 Results

**Workload A, what a reader sees:**

{{ d2_table('A','readers') }}

**Workload A, writer and cleaner:**

{{ d2_table('A','reclamation') }}

**Workload B, what a reader sees:**

{{ d2_table('B','readers') }}

**Workload B, writer and cleaner:**

{{ d2_table('B','reclamation') }}

The two comparators do not make bounded passes, so those columns are empty for them.

![](../results/part_d/figures/d2_batch_worst_stall.png){width=100%}

![](../results/part_d/figures/d2_batch_throughput.png){width=100%}

![](../results/part_d/figures/d2_batch_backlog.png){width=100%}

## 3.2 What the batch size trades off here: nothing that was measured

The assignment expects "a bigger batch should mean higher throughput but a longer worst-case
stall". **That is not what was measured.** Across a thousandfold range of batch sizes, in both
workloads, no measure taken depends on the batch:

- **Read throughput.** {{ M(min(batches('A','read_rps_median'))) }}–{{ M(max(batches('A','read_rps_median'))) }} M/s
  in workload A, with p50, p99 and p99.9 equal to within 1 ns. In workload B the medians are
  {{ M(min(batches('B','read_rps_median'))) }}–{{ M(max(batches('B','read_rps_median'))) }} M/s, but every batch
  size has runs at both about 37 and about 40.5 M/s (ranges in the table): the run-to-run
  spread, not the batch, sets the median.
- **The longest reader stall.** Medians of
  {{ ', '.join(us(x,0) for x in batches('A','max_stall_ns_median')) }} µs for batches 1 to 1024 in
  workload A and {{ ', '.join(us(x,0) for x in batches('B','max_stall_ns_median')) }} µs in workload B: no
  trend. With reclamation **disabled** the longest stall is
  {{ us(d2a('reclaim_disabled','max_stall_ns_median'),0) }} µs (A) and
  {{ us(d2b('reclaim_disabled','max_stall_ns_median'),0) }} µs (B), the same; it is the machine's floor seen
  in §2.1. There is no evidence that a reclamation pass stalls a reader, at any batch size.
  The cleaner works on its own thread and takes no lock a reader takes.
- **Reclamation throughput, backlog and memory.**
  {{ n0(min(batches('A','versions_reclaimed_per_s_median'))) }}–{{ n0(max(batches('A','versions_reclaimed_per_s_median'))) }}
  versions/s in workload A and
  {{ n0(min(batches('B','versions_reclaimed_per_s_median'))) }}–{{ n0(max(batches('B','versions_reclaimed_per_s_median'))) }}
  in workload B, not ordered by batch; the largest backlog is
  {{ n0(min(batches('B','backlog_max_median'))) }}–{{ n0(max(batches('B','backlog_max_median'))) }} versions in B for
  every batch; the cleaner uses the same CPU time
  ({{ n2(min(batches('A','cleaner_cpu_s_median'))) }}–{{ n2(max(batches('A','cleaner_cpu_s_median'))) }} s in A).
  Draining the final backlog of workload B takes
  {{ ', '.join(n1(x) for x in batches('B','drain_ms_median')) }} ms for batches 1 to 1024: no trend.
  What does change is the number of passes
  ({{ n0(d2a('batch_1','passes_median')) }} at batch 1, {{ n0(d2a('batch_1024','passes_median')) }} at batch
  1024), each of them correspondingly longer.

The first report said that a batch of 1 reclaims more slowly and holds more memory. That was
measured inside `perf stat` and is **not reproduced** by the unwrapped runs; it is withdrawn.

So for this design, on this machine, the tunable is a bound on the cleaner's work between two
checks of the epoch and nothing more: it is what makes a pass finite and its work accounted
for, and it has no measurable price or benefit between 1 and 1024. The design reason is that
the cost the assignment expects a batch to amortize, stopping or signalling readers, does not
exist here: readers are never stopped.

## 3.3 The two reference points

- **Reclamation disabled** reads fastest in workload A
  ({{ M(d2a('reclaim_disabled','read_rps_median')) }} against about {{ M(d2a('batch_64','read_rps_median')) }} M/s)
  because nothing is being freed: no cleaner competing for a core and for the allocator. It
  retained {{ n1(d2a('reclaim_disabled','memory_at_end_mib_median')/1024) }} GiB in 1.5 s in workload A and
  {{ n1(d2b('reclaim_disabled','memory_max_mib_median')/1024) }} GiB in workload B. It is not a usable
  configuration. (In workload B it reads at {{ M(d2b('reclaim_disabled','read_rps_median')) }} M/s, inside
  the range of the batch runs.)
- **Stop-the-world** frees memory fastest
  ({{ n0(d2a('reclaim_stw','versions_reclaimed_per_s_median')) }} versions/s;
  {{ n0(d2a('reclaim_stw','memory_at_end_mib_median')) }} MiB held where the epoch-based runs hold about
  {{ n0(d2a('batch_64','memory_at_end_mib_median')/1024) }} GiB) and lets the writer run faster
  ({{ n0(d2a('reclaim_stw','write_rps_median')) }} against about {{ n0(d2a('batch_64','write_rps_median')) }}
  updates/s), because the sweep has no batch limit and nothing else runs meanwhile. It pays
  with the readers: every `get` takes a shared lock, so **read throughput falls to
  {{ M(d2a('reclaim_stw','read_rps_median')) }} M/s (one
  {{ n0(d2a('batch_64','read_rps_median')/d2a('reclaim_stw','read_rps_median')) }}th) and the longest reader
  stall rises to {{ ms(d2a('reclaim_stw','max_stall_ns_median')) }} ms in workload A and
  {{ ms(d2b('reclaim_stw','max_stall_ns_median')) }} ms in workload B.** Its lock is inside the engine and
  was not placed; in a diagnostic its read throughput varied between 5.6 and 7.4 M/s with the
  heap layout, so that figure is good to about ±15 %. The stall is the length of a sweep and
  did not vary that way.

**Bounded reclamation against stop-the-world, worst reader pause** (median over 5 runs of each
run's longest stall; the epoch figure is the batch size with the largest median):

| Workload | Stop-the-world | Epoch-based (worst batch) | Reduction |
|---|---:|---:|---:|
| A, steady | {{ us(d2a('reclaim_stw','max_stall_ns_median'),0) }} µs | {{ us(worst_batch('A')[1],0) }} µs ({{ worst_batch('A')[0].replace('_',' ') }}) | {{ n0(d2a('reclaim_stw','max_stall_ns_median')/worst_batch('A')[1]) }}× |
| B, backlog-building | {{ us(d2b('reclaim_stw','max_stall_ns_median'),0) }} µs | {{ us(worst_batch('B')[1],0) }} µs ({{ worst_batch('B')[0].replace('_',' ') }}) | {{ n0(d2b('reclaim_stw','max_stall_ns_median')/worst_batch('B')[1]) }}× |

Since the epoch-based maximum is the machine's floor rather than anything reclamation does,
these ratios are lower bounds on the reduction and would be larger on a quieter machine.

# 4. D.3 — join and multi-key writes

## 4.1 Index-nested-loop join, writer active and paused

The join of Part C.2 (`benchmarks/join.h`): for every key that `keys()` returns for the Trades
table, fetch the trade, probe the Instruments table's secondary index with its symbol, and
fetch every matching instrument. 100,000 trades, 50 instruments, 100,000 joined rows per join;
every join is validated (complete; pairs match; the inner table of one version).

**Snapshots: one per store.** The join takes one Snapshot of Trades and one of Instruments at
its start and uses each for every call on its own store. Each table is read from one
immutable version for the whole join; the two versions are stable per-store versions and are
not guaranteed to be one atomic instant; and it is two handles, not the "exactly one Snapshot
handle" of the assignment's wording (a recorded deviation, decision D-060).

When active, the writer issues single Trade puts that change the trade's symbol and, after
every 64 of them, one transaction rewriting all 50 instruments.

| Joiners | Writer | Joins/s | Joined rows/s (M) | p50 ms | p99 ms | p99.9 ms | max ms, median run / worst run | Memory max, MiB | Backlog max |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | paused | {{ n1(join(1,'off','joins_per_s')) }} [{{ n1(join(1,'off','joins_per_s','min')) }}–{{ n1(join(1,'off','joins_per_s','max')) }}] | {{ M(join(1,'off','joined_rows_per_s')) }} | {{ n2(join(1,'off','join_p50_ms')) }} | {{ n2(join(1,'off','join_p99_ms')) }} | {{ n2(join(1,'off','join_p999_ms')) }} | {{ n2(join(1,'off','join_max_ms')) }} / {{ n2(join(1,'off','join_max_ms','max')) }} | {{ n1(join(1,'off','memory_held_idle_mib')) }} | 0 |
| 1 | active | {{ n1(join(1,'on','joins_per_s')) }} [{{ n1(join(1,'on','joins_per_s','min')) }}–{{ n1(join(1,'on','joins_per_s','max')) }}] | {{ M(join(1,'on','joined_rows_per_s')) }} | {{ n2(join(1,'on','join_p50_ms')) }} | {{ n2(join(1,'on','join_p99_ms')) }} | {{ n2(join(1,'on','join_p999_ms')) }} | {{ n2(join(1,'on','join_max_ms')) }} / {{ n2(join(1,'on','join_max_ms','max')) }} | {{ n1(join(1,'on','memory_held_max_mib')) }} | {{ n0(join(1,'on','backlog_max_versions')) }} |
| 2 | paused | {{ n1(join(2,'off','joins_per_s')) }} [{{ n1(join(2,'off','joins_per_s','min')) }}–{{ n1(join(2,'off','joins_per_s','max')) }}] | {{ M(join(2,'off','joined_rows_per_s')) }} | {{ n2(join(2,'off','join_p50_ms')) }} | {{ n2(join(2,'off','join_p99_ms')) }} | {{ n2(join(2,'off','join_p999_ms')) }} | {{ n2(join(2,'off','join_max_ms')) }} / {{ n2(join(2,'off','join_max_ms','max')) }} | {{ n1(join(2,'off','memory_held_idle_mib')) }} | 0 |
| 2 | active | {{ n1(join(2,'on','joins_per_s')) }} [{{ n1(join(2,'on','joins_per_s','min')) }}–{{ n1(join(2,'on','joins_per_s','max')) }}] | {{ M(join(2,'on','joined_rows_per_s')) }} | {{ n2(join(2,'on','join_p50_ms')) }} | {{ n2(join(2,'on','join_p99_ms')) }} | {{ n2(join(2,'on','join_p999_ms')) }} | {{ n2(join(2,'on','join_max_ms')) }} / {{ n2(join(2,'on','join_max_ms','max')) }} | {{ n1(join(2,'on','memory_held_max_mib')) }} | {{ n0(join(2,'on','backlog_max_versions')) }} |
| 4 | paused | {{ n1(join(4,'off','joins_per_s')) }} [{{ n1(join(4,'off','joins_per_s','min')) }}–{{ n1(join(4,'off','joins_per_s','max')) }}] | {{ M(join(4,'off','joined_rows_per_s')) }} | {{ n2(join(4,'off','join_p50_ms')) }} | {{ n2(join(4,'off','join_p99_ms')) }} | {{ n2(join(4,'off','join_p999_ms')) }} | {{ n2(join(4,'off','join_max_ms')) }} / {{ n2(join(4,'off','join_max_ms','max')) }} | {{ n1(join(4,'off','memory_held_idle_mib')) }} | 0 |
| 4 | active | {{ n1(join(4,'on','joins_per_s')) }} [{{ n1(join(4,'on','joins_per_s','min')) }}–{{ n1(join(4,'on','joins_per_s','max')) }}] | {{ M(join(4,'on','joined_rows_per_s')) }} | {{ n2(join(4,'on','join_p50_ms')) }} | {{ n2(join(4,'on','join_p99_ms')) }} | {{ n2(join(4,'on','join_p999_ms')) }} | {{ n2(join(4,'on','join_max_ms')) }} / {{ n2(join(4,'on','join_max_ms','max')) }} | {{ n1(join(4,'on','memory_held_max_mib')) }} | {{ n0(join(4,'on','backlog_max_versions')) }} |
| **8** | **paused** | **{{ n0(join(8,'off','joins_per_s')) }} [{{ n0(join(8,'off','joins_per_s','min')) }}–{{ n0(join(8,'off','joins_per_s','max')) }}]** | {{ M(join(8,'off','joined_rows_per_s')) }} | **{{ n2(join(8,'off','join_p50_ms')) }}** | {{ n2(join(8,'off','join_p99_ms')) }} | {{ n2(join(8,'off','join_p999_ms')) }} | {{ n2(join(8,'off','join_max_ms')) }} / {{ n2(join(8,'off','join_max_ms','max')) }} | {{ n1(join(8,'off','memory_held_idle_mib')) }} | 0 |
| **8** | **active** | **{{ n1(join(8,'on','joins_per_s')) }} [{{ n1(join(8,'on','joins_per_s','min')) }}–{{ n1(join(8,'on','joins_per_s','max')) }}]** | {{ M(join(8,'on','joined_rows_per_s')) }} | **{{ n2(join(8,'on','join_p50_ms')) }}** | {{ n2(join(8,'on','join_p99_ms')) }} | {{ n2(join(8,'on','join_p999_ms')) }} | {{ n2(join(8,'on','join_max_ms')) }} / {{ n2(join(8,'on','join_max_ms','max')) }} | {{ n1(join(8,'on','memory_held_max_mib')) }} | {{ n0(join(8,'on','backlog_max_versions')) }} |

Writer while the joins ran: {{ n0(join(1,'on','writer_trade_puts_per_s')) }} Trade puts/s and
{{ n0(join(1,'on','writer_instrument_txns_per_s')) }} Instrument transactions/s with one joiner;
{{ n0(join(8,'on','writer_trade_puts_per_s')) }} and {{ n0(join(8,'on','writer_instrument_txns_per_s')) }} with
eight.

![](../results/part_d/figures/d3_join_throughput.png){width=49%} ![](../results/part_d/figures/d3_join_latency.png){width=49%}

**What the concurrent writer costs a join.** At 8 joiners:
{{ n2(join(8,'on','join_p50_ms')/join(8,'off','join_p50_ms')) }}× the median latency
({{ n2(join(8,'off','join_p50_ms')) }} → {{ n2(join(8,'on','join_p50_ms')) }} ms),
{{ n1(join(8,'on','join_p99_ms')/join(8,'off','join_p99_ms')) }}× the p99
({{ n2(join(8,'off','join_p99_ms')) }} → {{ n2(join(8,'on','join_p99_ms')) }} ms) and
{{ n0(100*(1-join(8,'on','joins_per_s')/join(8,'off','joins_per_s'))) }} % of the throughput
({{ n0(join(8,'off','joins_per_s')) }} → {{ n0(join(8,'on','joins_per_s')) }} joins/s). The same factor of
about two as for point reads next to an unthrottled writer (§2.5), and plausibly for the same
reason: the join never waits for the writer, but the rows and index segments it reads keep
being republished at new addresses. A join is never blocked and never returns a wrong or
partial result in either mode.

**Joins hold reclamation back.** A join pins both stores for its 7–26 ms, so everything
retired meanwhile waits: up to {{ n0(join(8,'on','memory_held_max_mib')) }} MiB held and
{{ n0(join(8,'on','backlog_max_versions')) }} unreclaimed versions with 8 joiners and the writer active,
against {{ n1(join(8,'off','memory_held_idle_mib')) }} MiB with the writer paused. All of it is reclaimed
when the joins end.

**The generator's `use_join` path is not this join.** It follows each successful `get` with
one `index_get` on the Instruments store and discards the result: an index probe. On the RCU
store, 8 readers, preload-only: {{ M(gen('join','rps_median')) }} M operations/s against
{{ M(gen('put','rps_median')) }} M without the probe. Its probe passes an eighth symbol byte that the
generator never writes; in these binaries that byte was 0 in all 100,000 stored trades and the
probe matched **{{ pct(gen('join','probe_hit_rate')) }} %** (measured from the store after each run).

## 4.2 Transaction commit throughput by size

One writer committing transactions of *N* Trades into the 100,000-row table, keys spread over
the table (so *N* keys fall into *N* different hash partitions almost always). "No index" is
the Trades table as the assignment loads it; "indexed" adds a secondary index on the field
every write changes, the worst case for index maintenance.

| Keys | Index | Commits/s | Rows/s | p50 µs | p99 µs | p99.9 µs | max µs, median run / worst run | Segments cloned per commit | Bytes copied per commit | Bytes copied per row |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | none | {{ n0(txn(1,'off','commits_per_s')) }} [{{ n0(txn(1,'off','commits_per_s','min')) }}–{{ n0(txn(1,'off','commits_per_s','max')) }}] | {{ n0(txn(1,'off','rows_per_s')) }} | {{ n1(txn(1,'off','commit_p50_us')) }} | {{ n1(txn(1,'off','commit_p99_us')) }} | {{ n1(txn(1,'off','commit_p999_us')) }} | {{ n0(txn(1,'off','commit_max_us')) }} / {{ n0(txn(1,'off','commit_max_us','max')) }} | {{ n2(txn(1,'off','segments_cloned_per_commit')) }} | {{ n0(txn(1,'off','bytes_copied_per_commit')) }} | {{ n0(txn(1,'off','bytes_copied_per_row')) }} |
| 4 | none | {{ n0(txn(4,'off','commits_per_s')) }} [{{ n0(txn(4,'off','commits_per_s','min')) }}–{{ n0(txn(4,'off','commits_per_s','max')) }}] | {{ n0(txn(4,'off','rows_per_s')) }} | {{ n1(txn(4,'off','commit_p50_us')) }} | {{ n1(txn(4,'off','commit_p99_us')) }} | {{ n1(txn(4,'off','commit_p999_us')) }} | {{ n0(txn(4,'off','commit_max_us')) }} / {{ n0(txn(4,'off','commit_max_us','max')) }} | {{ n2(txn(4,'off','segments_cloned_per_commit')) }} | {{ n0(txn(4,'off','bytes_copied_per_commit')) }} | {{ n0(txn(4,'off','bytes_copied_per_row')) }} |
| 16 | none | {{ n0(txn(16,'off','commits_per_s')) }} [{{ n0(txn(16,'off','commits_per_s','min')) }}–{{ n0(txn(16,'off','commits_per_s','max')) }}] | {{ n0(txn(16,'off','rows_per_s')) }} | {{ n1(txn(16,'off','commit_p50_us')) }} | {{ n1(txn(16,'off','commit_p99_us')) }} | {{ n1(txn(16,'off','commit_p999_us')) }} | {{ n0(txn(16,'off','commit_max_us')) }} / {{ n0(txn(16,'off','commit_max_us','max')) }} | {{ n2(txn(16,'off','segments_cloned_per_commit')) }} | {{ n0(txn(16,'off','bytes_copied_per_commit')) }} | {{ n0(txn(16,'off','bytes_copied_per_row')) }} |
| 1 | indexed | {{ n0(txn(1,'on','commits_per_s')) }} [{{ n0(txn(1,'on','commits_per_s','min')) }}–{{ n0(txn(1,'on','commits_per_s','max')) }}] | {{ n0(txn(1,'on','rows_per_s')) }} | {{ n1(txn(1,'on','commit_p50_us')) }} | {{ n1(txn(1,'on','commit_p99_us')) }} | {{ n1(txn(1,'on','commit_p999_us')) }} | {{ n0(txn(1,'on','commit_max_us')) }} / {{ n0(txn(1,'on','commit_max_us','max')) }} | {{ n2(txn(1,'on','segments_cloned_per_commit')) }} | {{ n0(txn(1,'on','bytes_copied_per_commit')) }} | {{ n0(txn(1,'on','bytes_copied_per_row')) }} |
| 4 | indexed | {{ n0(txn(4,'on','commits_per_s')) }} [{{ n0(txn(4,'on','commits_per_s','min')) }}–{{ n0(txn(4,'on','commits_per_s','max')) }}] | {{ n0(txn(4,'on','rows_per_s')) }} | {{ n1(txn(4,'on','commit_p50_us')) }} | {{ n1(txn(4,'on','commit_p99_us')) }} | {{ n1(txn(4,'on','commit_p999_us')) }} | {{ n0(txn(4,'on','commit_max_us')) }} / {{ n0(txn(4,'on','commit_max_us','max')) }} | {{ n2(txn(4,'on','segments_cloned_per_commit')) }} | {{ n0(txn(4,'on','bytes_copied_per_commit')) }} | {{ n0(txn(4,'on','bytes_copied_per_row')) }} |
| 16 | indexed | {{ n0(txn(16,'on','commits_per_s')) }} [{{ n0(txn(16,'on','commits_per_s','min')) }}–{{ n0(txn(16,'on','commits_per_s','max')) }}] | {{ n0(txn(16,'on','rows_per_s')) }} | {{ n1(txn(16,'on','commit_p50_us')) }} | {{ n1(txn(16,'on','commit_p99_us')) }} | {{ n1(txn(16,'on','commit_p999_us')) }} | {{ n0(txn(16,'on','commit_max_us')) }} / {{ n0(txn(16,'on','commit_max_us','max')) }} | {{ n2(txn(16,'on','segments_cloned_per_commit')) }} | {{ n0(txn(16,'on','bytes_copied_per_commit')) }} | {{ n0(txn(16,'on','bytes_copied_per_row')) }} |

![](../results/part_d/figures/d3_txn_commits.png){width=49%} ![](../results/part_d/figures/d3_txn_rows.png){width=49%}

- **Commit throughput falls almost in proportion to the transaction size**:
  {{ n0(txn(1,'off','commits_per_s')) }} → {{ n0(txn(4,'off','commits_per_s')) }} →
  {{ n0(txn(16,'off','commits_per_s')) }} commits/s for 1, 4, 16 keys, because each key lands in a
  different partition and each touched partition is one 24 KiB segment to copy. A commit costs
  about 24 KiB of copying per key plus a fixed 2 KiB directory and about 250 owner-count
  updates.
- **Rows per second rise** ({{ n0(txn(1,'off','rows_per_s')) }} → {{ n0(txn(4,'off','rows_per_s')) }} →
  {{ n0(txn(16,'off','rows_per_s')) }}) because that fixed part and the single publication are shared
  by more rows; the gain is small since the per-key copy dominates.
- Each commit is still one publication, whatever its size. With a single writer and no
  reader the cleaner keeps up: the store never holds more than
  {{ n1(max(txn(s,i,'memory_held_max_mib') for s in (1,4,16) for i in ('off','on'))) }} MiB or
  {{ n0(max(txn(s,i,'backlog_max_versions') for s in (1,4,16) for i in ('off','on'))) }} unreclaimed versions.
- **A secondary index on the written field costs about a factor of
  {{ n0(txn(1,'off','commits_per_s')/txn(1,'on','commits_per_s')) }}** ({{ n0(txn(1,'on','commits_per_s')) }}
  against {{ n0(txn(1,'off','commits_per_s')) }} commits/s for one key): changing a row's indexed
  value rebuilds two of the 64 index partitions, about 115 KiB. With 100,000 rows and only 50
  distinct values those partitions hold whole posting lists of 2,000 keys. This is the cost of
  copying index segments; the table the assignment indexes (Instruments, 50 rows) pays almost
  nothing, and the writer rewrote it in whole-table transactions
  {{ n0(join(1,'on','writer_instrument_txns_per_s')) }} times per second during the join runs.

## 4.3 Where copy-on-write helps little, or hurts

Measured: the same Trade rows written as one transaction and as single puts.

| Case | Publications | Elapsed ms | Rows/s | Segments cloned | Bytes copied | Bytes copied per row (payload 32) |
|---|---:|---:|---:|---:|---:|---:|
| 100,000 rows, one transaction | {{ n0(cow('all rows as one transaction','publications')) }} | {{ n2(cow('all rows as one transaction','elapsed_ms')) }} [{{ n2(cow('all rows as one transaction','elapsed_ms','min')) }}–{{ n2(cow('all rows as one transaction','elapsed_ms','max')) }}] | {{ n0(cow('all rows as one transaction','rows_per_s')) }} | {{ n0(cow('all rows as one transaction','segments_cloned')) }} | {{ n1(cow('all rows as one transaction','bytes_copied')/1e6) }} MB | {{ n0(cow('all rows as one transaction','bytes_copied_per_row')) }} |
| 100,000 rows, single puts | {{ n0(cow('all rows as single puts','publications')) }} | {{ n2(cow('all rows as single puts','elapsed_ms')) }} [{{ n2(cow('all rows as single puts','elapsed_ms','min')) }}–{{ n2(cow('all rows as single puts','elapsed_ms','max')) }}] | {{ n0(cow('all rows as single puts','rows_per_s')) }} | {{ n0(cow('all rows as single puts','segments_cloned')) }} | {{ n0(cow('all rows as single puts','bytes_copied')/1e6) }} MB | {{ n0(cow('all rows as single puts','bytes_copied_per_row')) }} |
| 4,096 rows over all partitions, one transaction | {{ n0(cow('4096 rows spread over all partitions as one transaction','publications')) }} | {{ n2(cow('4096 rows spread over all partitions as one transaction','elapsed_ms')) }} [{{ n2(cow('4096 rows spread over all partitions as one transaction','elapsed_ms','min')) }}–{{ n2(cow('4096 rows spread over all partitions as one transaction','elapsed_ms','max')) }}] | {{ n0(cow('4096 rows spread over all partitions as one transaction','rows_per_s')) }} | {{ n0(cow('4096 rows spread over all partitions as one transaction','segments_cloned')) }} | {{ n1(cow('4096 rows spread over all partitions as one transaction','bytes_copied')/1e6) }} MB | {{ n0(cow('4096 rows spread over all partitions as one transaction','bytes_copied_per_row')) }} |
| 4,096 rows, single puts | {{ n0(cow('4096 rows spread as single puts','publications')) }} | {{ n2(cow('4096 rows spread as single puts','elapsed_ms')) }} [{{ n2(cow('4096 rows spread as single puts','elapsed_ms','min')) }}–{{ n2(cow('4096 rows spread as single puts','elapsed_ms','max')) }}] | {{ n0(cow('4096 rows spread as single puts','rows_per_s')) }} | {{ n0(cow('4096 rows spread as single puts','segments_cloned')) }} | {{ n0(cow('4096 rows spread as single puts','bytes_copied')/1e6) }} MB | {{ n0(cow('4096 rows spread as single puts','bytes_copied_per_row')) }} |
| 16 rows, one transaction | {{ n0(cow('16 rows as one transaction','publications')) }} | {{ n2(cow('16 rows as one transaction','elapsed_ms')) }} | {{ n0(cow('16 rows as one transaction','rows_per_s')) }} | {{ n0(cow('16 rows as one transaction','segments_cloned')) }} | {{ n2(cow('16 rows as one transaction','bytes_copied')/1e6) }} MB | {{ n0(cow('16 rows as one transaction','bytes_copied_per_row')) }} |
| 16 rows, single puts | {{ n0(cow('16 rows as single puts','publications')) }} | {{ n2(cow('16 rows as single puts','elapsed_ms')) }} | {{ n0(cow('16 rows as single puts','rows_per_s')) }} | {{ n0(cow('16 rows as single puts','segments_cloned')) }} | {{ n2(cow('16 rows as single puts','bytes_copied')/1e6) }} MB | {{ n0(cow('16 rows as single puts','bytes_copied_per_row')) }} |

Copy-on-write copies a whole 24 KiB segment to change one 32-byte row:
**{{ n0(cow('all rows as single puts','bytes_copied_per_row')/32) }} bytes copied per byte of
payload** for a single-key update. That is where it hurts:

1. **Small, frequent, scattered writes.** Every update pays the full segment; that is why the
   global-mutex store, which updates one slot in place, completes
   {{ n2(max(d1('global_lock','writer_max',1,'write_rps_median'),d1('global_lock','writer_max_o0',1,'write_rps_median'))/1e6) }}
   million updates/s where the RCU store completes {{ n0(d1('rcu','writer_max',1,'write_rps_median')) }}
   (D.1, 1 reader, unthrottled), and why the cleaner cannot keep up at 4 or 8 readers (§2.5).
2. **A very large transaction that touches most of the table**, the assignment's example. It
   is efficient *per row* ({{ n0(cow('all rows as one transaction','bytes_copied_per_row')) }} bytes per row for
   100,000 rows: the segments are copied once and filled completely), but: it copies the
   entire index ({{ n1(cow('all rows as one transaction','bytes_copied')/1e6) }} MB here, the whole table); the
   old and the new version coexist, so memory doubles until the old one is reclaimed, and
   stays doubled for as long as any reader or snapshot is in the old version; and it is one
   {{ n0(cow('all rows as one transaction','elapsed_ms')) }} ms-long commit during which no other write can be
   published. For a table larger than the caches this is a full rewrite to make a change that
   an in-place store would make row by row.
3. **A medium transaction spread thinly** is the worst per row among the batched cases: 4,096
   rows over all 256 partitions copy the same
   {{ n1(cow('4096 rows spread over all partitions as one transaction','bytes_copied')/1e6) }} MB as 100,000
   rows ({{ n0(cow('4096 rows spread over all partitions as one transaction','bytes_copied_per_row')) }} bytes
   per row), because every partition is touched and each is copied whole whatever fraction of
   it changes.
4. **Workloads with long readers.** A join or any long-held snapshot keeps every version
   published during it alive ({{ n0(join(8,'on','memory_held_max_mib')) }} MiB during
   {{ n0(join(8,'on','join_p50_ms')) }} ms joins here); a lock-based store has no such cost because it has
   no old versions.
5. **Indexes on low-cardinality fields of large tables** (§4.2).

Where it helps: reads never wait (p99.9 of {{ us(d1('rcu','writer_paced',8,'p999_ns_median'),2) }}–{{ us(d1('rcu','writer_max',8,'p999_ns_median'),2) }}
µs against {{ us(min(d1(v,s,8,'p999_ns_median') for v in ('global_lock','rwlock') for s in ('writer_paced','writer_paced_o0','writer_max','writer_max_o0')),1) }}–{{ us(max(d1(v,s,8,'p999_ns_median') for v in ('global_lock','rwlock') for s in ('writer_paced','writer_paced_o0','writer_max','writer_max_o0')),1) }}
µs for the lock stores at 8 readers), read throughput scales with cores, the writer cannot be
starved by readers whatever the memory layout, and multi-row consistency for a reader costs
nothing extra.

# 5. Profiling track: what the profilers do to the numbers

All figures in this section come from the profiled runs under `profiling/`; none of them is
used anywhere else as a throughput or latency result. Full table for all
{{ count("profiling/perf_stat/*/*/stdout/*.txt") }} profiled runs:
`summary/summary.md`, `summary/profiling_overhead.csv`.

**Under `perf stat` (with `/usr/bin/time`), median of 2 runs against the timing track's median
of 5, same cell and same lock placement:**

| Cell | Profiled / timing |
|---|---:|
| RCU, 8 readers, paced writer | {{ n2(oh('D.1','rcu; writer_paced; 8 reader(s)','reads/s')) }} |
| RCU, 8 readers, unthrottled writer | {{ n2(oh('D.1','rcu; writer_max; 8 reader(s)','reads/s')) }} |
| RCU, 8 readers, no writer | {{ n2(oh('D.1','rcu; readonly; 8 reader(s)','reads/s')) }} |
| global mutex at 48, 8 readers, paced writer | {{ n2(oh('D.1','global_lock; writer_paced; 8 reader(s)','reads/s')) }} |
| global mutex at 0, 8 readers, paced writer | {{ n2(oh('D.1','global_lock; writer_paced_o0; 8 reader(s)','reads/s')) }} |
| global mutex at 48, 2 readers, paced writer | {{ n2(oh('D.1','global_lock; writer_paced; 2 reader(s)','reads/s')) }} |
| global mutex at 48, 8 readers, unthrottled writer | {{ n2(oh('D.1','global_lock; writer_max; 8 reader(s)','reads/s')) }} |
| shared mutex at 48, 8 readers, paced writer | {{ n2(oh('D.1','rwlock; writer_paced; 8 reader(s)','reads/s')) }} |
| shared mutex at 0, 8 readers, paced writer | {{ n2(oh('D.1','rwlock; writer_paced_o0; 8 reader(s)','reads/s')) }} |
| shared mutex at 48, 1 / 2 / 4 readers, unthrottled writer | {{ n2(oh('D.1','rwlock; writer_max; 1 reader(s)','reads/s')) }} / {{ n2(oh('D.1','rwlock; writer_max; 2 reader(s)','reads/s')) }} / {{ n2(oh('D.1','rwlock; writer_max; 4 reader(s)','reads/s')) }} |
| stop-the-world, workload A / workload B (reads/s) | {{ n2(oh('D.2 A','stop-the-world','reads/s')) }} / {{ n2(oh('D.2 B','stop-the-world','reads/s')) }} |
| batch 64, workload A / workload B (reads/s) | {{ n2(oh('D.2 A','batch 64','reads/s')) }} / {{ n2(oh('D.2 B','batch 64','reads/s')) }} |
| transactions of 1 / 4 / 16 keys, no index (commits/s) | {{ n2(oh('D.3 txn','1 key(s); index off','commits/s')) }} / {{ n2(oh('D.3 txn','4 key(s); index off','commits/s')) }} / {{ n2(oh('D.3 txn','16 key(s); index off','commits/s')) }} |
| join, 8 joiners, writer paused / active (joins/s) | {{ n2(oh('D.3 join','8 joiner(s); writer paused','joins/s')) }} / {{ n2(oh('D.3 join','8 joiner(s); writer active','joins/s')) }} |

`perf stat` in counting mode saves and restores its counters at every context switch. The
cells it changes are the ones whose threads block in the kernel millions of times per run
(global mutex: {{ n1(d1('global_lock','writer_paced',8,'context_switches_median')/1e6) }} million context
switches in a 5.5 s run at 8 readers; RCU: {{ n0(d1('rcu','writer_paced',8,'context_switches_median')) }}),
and the effect has either sign: slowing every handover can make a lock convoy shorter. RCU,
transactions and joins move by a few percent at most.

`perf record` (1,999 Hz, frame pointers) and `perf c2c record` change the cells they observed
by 0–5 %, with two exceptions, the RCU store at 2 readers (0.90) and the shared mutex at 8
readers with an unthrottled writer (1.16–1.56): that cell is the least stable in the timing
track too (§6).

**perf stat, 8 readers, unthrottled writer, whole process:**

| Store | CPUs utilized | Instructions per cycle | Context switches per second | Peak resident set, MiB |
|---|---:|---:|---:|---:|
| global mutex at 48 | {{ n2(d1('global_lock','writer_max',8,'cpus_utilized_median')) }} | {{ n2(d1('global_lock','writer_max',8,'instructions_per_cycle_median')) }} | {{ n0(d1('global_lock','writer_max',8,'context_switches_median')/5.5) }} | {{ n0(d1('global_lock','writer_max',8,'max_rss_mib_median')) }} |
| global mutex at 0 | {{ n2(d1('global_lock','writer_max_o0',8,'cpus_utilized_median')) }} | {{ n2(d1('global_lock','writer_max_o0',8,'instructions_per_cycle_median')) }} | {{ n0(d1('global_lock','writer_max_o0',8,'context_switches_median')/5.5) }} | {{ n0(d1('global_lock','writer_max_o0',8,'max_rss_mib_median')) }} |
| shared mutex at 48 | {{ n2(d1('rwlock','writer_max',8,'cpus_utilized_median')) }} | {{ n2(d1('rwlock','writer_max',8,'instructions_per_cycle_median')) }} | {{ n0(d1('rwlock','writer_max',8,'context_switches_median')/5.5) }} | {{ n0(d1('rwlock','writer_max',8,'max_rss_mib_median')) }} |
| shared mutex at 0 | {{ n2(d1('rwlock','writer_max_o0',8,'cpus_utilized_median')) }} | {{ n2(d1('rwlock','writer_max_o0',8,'instructions_per_cycle_median')) }} | {{ n0(d1('rwlock','writer_max_o0',8,'context_switches_median')/5.5) }} | {{ n0(d1('rwlock','writer_max_o0',8,'max_rss_mib_median')) }} |
| RCU | {{ n2(d1('rcu','writer_max',8,'cpus_utilized_median')) }} | {{ n2(d1('rcu','writer_max',8,'instructions_per_cycle_median')) }} | {{ n0(d1('rcu','writer_max',8,'context_switches_median')/5.5) }} | {{ n0(d1('rcu','writer_max',8,'max_rss_mib_median')) }} |

(The RCU process also runs its cleaner; its resident set is the unreclaimed backlog of §2.5.)

**Stop-the-world, flame graph** (`rcu_r8_stw`): {{ n0(fgshare('rcu_r8_stw','lock functions')) }} % of
cycles in the shared-lock functions that every `get` must call in that mode,
{{ n0(fgshare('rcu_r8_stw','writer: store put')) }} % in the writer, {{ n0(fgshare('rcu_r8_stw','cleaner thread')) }} %
in the sweeping cleaner and {{ n0(fgshare('rcu_r8_stw','reader: store get')) }} % in the lookup.

# 6. Failed, corrected or inconclusive

- **"A bigger batch gives higher read throughput and a longer worst-case stall."** Not
  observed (§3.2), in two workloads and two independent campaigns of the timing kind.
- **"A batch of 1 reclaims more slowly and holds more memory"** (first report). Not reproduced
  without the profiler wrapper; withdrawn.
- **"The shared mutex starves its writer at 8 readers"** (first report). True at one lock
  placement, false at the other (§2.2).
- **"Owner counts share cache lines with reader data and cause the read slowdown next to a
  writer" (R8).** Only weakly supported: reader loads that hit a remotely modified line exist
  but are {{ n2(100*c2csym('rcu_r8_writer_max','VersionedStore::read','samples')/c2c('rcu_r8_writer_max','load_operations')) }} %
  of sampled loads (§2.5). The slowdown is real and its cause is not established beyond "the
  readers are stalled on memory"; no change was made to the store.
- **Why the lock's cache-line offset matters** is a hypothesis consistent with the data, not a
  demonstrated mechanism (§2.2).
- **"The steady workload is steady."** It is not at 4 or 8 readers with an unthrottled writer
  (§2.5): the backlog grows throughout.

# 7. Limitations and threats to validity

- One machine, one compiler, one C library, a laptop with two kinds of cores. The lock
  placement effect in particular is a property of this glibc's lock layouts on this CPU; the
  two placements bracket it here and may not elsewhere.
- **The lock-store numbers are two points, not one.** A program that does not control where
  its lock lands gets one or the other by accident, and can change sides when an unrelated
  string changes length. Part A's accepted results were measured that way and are left as
  they are.
- **Three sessions of one machine differ by a few percent** even where nothing was wrapped or
  moved: the supplied generator's figures in this campaign are
  {{ n2(yardstick('min')) }}–{{ n2(yardstick('max')) }}
  of the first campaign's (`summary/original_vs_corrected.md`). Differences of that size
  between campaigns mean nothing.
- **Cells that remain uncertain.** (e) Global mutex at 2 and 4 readers with the paced writer:
  the first campaign gave {{ M(cmp3('global_lock; writer_paced; 2 reader(s)','reads/s','original_wrapped')) }} and
  {{ M(cmp3('global_lock; writer_paced; 4 reader(s)','reads/s','original_wrapped')) }} M/s (lock at offset 48,
  wrapped); this one gives {{ M(d1('global_lock','writer_paced',2,'read_rps_median')) }} and
  {{ M(d1('global_lock','writer_paced',4,'read_rps_median')) }} M/s at the same offset, and under the same wrapper
  {{ n2(oh('D.1','global_lock; writer_paced; 2 reader(s)','reads/s')) }}× and
  {{ n2(oh('D.1','global_lock; writer_paced; 4 reader(s)','reads/s')) }}× of that. So the earlier, higher
  figures are explained neither by the wrapper nor by the placement, and were not reproduced
  in either later session; a lightly contended mutex is evidently sensitive to something
  else that differs between sessions. The statement that survives all three campaigns is
  the weaker one: the global mutex never exceeds about 22 M reads/s and falls at 8 readers.
  (a) Shared mutex, 8 readers, unthrottled writer, lock at
  offset 48: reads {{ M(d1('rwlock','writer_max',8,'read_rps_min')) }}–{{ M(d1('rwlock','writer_max',8,'read_rps_max')) }}
  M/s and writer {{ n0(d1('rwlock','writer_max',8,'write_rps_min')) }}–{{ n0(d1('rwlock','writer_max',8,'write_rps_max')) }}
  updates/s across 5 runs, and 16–56 % higher reads under the sampling profilers: readers and
  writer share the lock in a ratio that is not stable. (b) Stop-the-world read throughput:
  about ±15 % with heap layout (its stall is not affected). (c) Workload B read throughput:
  each setting has runs at about 37 and about 40.5 M/s; the cause was not investigated. (d)
  Global mutex at 2 readers, paced: [{{ M(d1('global_lock','writer_paced',2,'read_rps_min')) }}–{{ M(d1('global_lock','writer_paced',2,'read_rps_max')) }}]
  M/s.
- Five repetitions of 3–5 s windows. "Longest stall" is a maximum over hundreds of millions of
  samples and is sensitive to a single preemption; it is reported per run (median and worst),
  not averaged. Its floor in this session, about 140–160 µs, is the machine's.
- Throughput includes two clock reads per operation (about a third of the cycles of an
  uncontended read).
- Counters, CPU utilization, context switches and peak resident set come from profiled runs
  (2 repetitions), not from the timing runs; they are per process, not per thread, and cannot
  isolate the readers. Kernel frames in the flame graphs have no symbols
  (`kptr_restrict = 1`).
- The cleaner thread is not pinned.
- `perf c2c` samples loads; a sample count is evidence of where contention is, not a
  measurement of its cost.
- The temperature rose from {{ n0(min(condall(r'temp=(\d+)C'))) }} to {{ n0(max(condall(r'temp=(\d+)C'))) }} °C
  during the campaign; repetitions are the outer loop, so drift is spread over all cells, and
  the ranges are narrow, but thermal limiting cannot be excluded.
- Track R has no concurrent writer by construction, an 80 % hit rate and an uncontrolled lock
  placement; it is a compatibility reference, not the D.1 measurement.
- The join uses one Snapshot per store (two handles): each table is consistent, the pair is
  not an atomic cross-store snapshot.
- RCU and the lock stores differ in storage and update strategy, not only in synchronization.
- With reclamation disabled the window is 1 s; its throughput is not directly comparable with
  the 3 s runs.

# 8. Raw data

| What | Where (under `results/part_d/{{ PRIMARY }}/`) |
|---|---|
| Machine, toolchain, build flags, executable hashes, prerequisites | `env/` |
| Conditions at {{ ncond() }} checkpoints | `conditions.log` |
| **Timing track.** D.1 track C: one CSV row per run (with the lock offset); copy-on-write and reclamation counters | `d1/concurrent/runs.csv`, `cow.csv` |
| Latency histograms, one per run | `d1/concurrent/hist/`, `d2/*/hist/`, `d3/*/hist/` |
| Command line, `wrapper: none`, start, end and exit status of every run | `d*/*/stdout/` |
| D.1 track R | `d1/reference/` |
| D.2 workloads A and B | `d2/steady/`, `d2/backlog/` |
| D.3 transactions, join, copy-on-write, generator paths | `d3/txn/`, `d3/join/`, `d3/cow/`, `d3/generator/` |
| **Profiling track.** The same cells under perf stat (counters, peak RSS, their own CSVs and histograms) | `profiling/perf_stat/` |
| Flame graphs ({{ count('profiling/flamegraphs/*.svg') }} SVGs), folded stacks, compressed `perf script` output, commands | `profiling/flamegraphs/` |
| perf c2c reports ({{ count('profiling/c2c/*.c2c-report.txt') }}) | `profiling/c2c/` |
| Summary tables, CSVs, profiler effects, profile regrouping | `summary/` |
| Figures (12) | `figures/` |

Also under `results/part_d/`: `summary/` and `figures/` (copies of the above, embedded here),
`summary/original_vs_corrected.md`, the two superseded campaigns, and `README.md`.
Diagnostics outside the campaigns: `results/part_d_diagnostics/`.

Reproduce: `scripts/run_part_d.sh check`, then `scripts/run_part_d.sh all` (about an hour; a
new directory each time; refuses to run without mains power, the `performance` governor,
usable perf and an idle machine), then `scripts/publish_part_d.sh <campaign directory>`,
`scripts/render_part_d_report.py` and `scripts/build_part_d_report.sh`.
