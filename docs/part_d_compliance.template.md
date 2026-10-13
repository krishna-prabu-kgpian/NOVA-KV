# Part D compliance checklist (corrected campaign)

Requirement by requirement, from `assignment2.pdf` pp. 5–6 (Part D) and p. 7 (marks). Status
values: PASS (measured, with the evidence named), PARTIAL, FAIL, NOT RUN, NOTE (met, with a
stated qualification). Generated from `docs/part_d_compliance.template.md`; its numbers are
computed from the campaign's data by `scripts/render_part_d_report.py`.

All evidence is under `results/part_d/{{ PRIMARY }}/` (campaign `{{ CAMPAIGN }}`:
{{ count('d[123]/*/stdout/*.txt') }} timing runs started directly, 5 repetitions per cell, 0 failures;
{{ count('profiling/perf_stat/*/*/stdout/*.txt') }} separate profiled runs). The report is
`reports/part_d_report.md` / `.pdf`. This campaign supersedes the one tagged `part-d-final`
(every timed run inside `perf stat`) and an intermediate unwrapped one (lock placement not
controlled); both are kept under `results/part_d/` for audit (decisions D-070, D-071, D-072).

## D.1 Point-query throughput and the 100k RPS target (17 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D1.1 | "Using the workload generator (Appendix A) with its default parameters, benchmark point-get RPS for all four configurations (single thread, global lock, rwlock, and your Part B RCU store) at 1, 2, 4, and 8 reader threads" | Track R: `d1/reference/` (13 cells × 5; single thread at 1 reader only); report §2.4 | PASS |
| D1.2 | "with one background writer thread active throughout" | **The supplied generator cannot do this** (its writer finishes before readers start). Measured with the repository's own harness on the same schema and read mix: track C, `d1/concurrent/` ({{ count('d1/concurrent/stdout/*.txt') }} runs), writer paced and unthrottled, successful updates counted; report §2.1 | NOTE: met by track C; track R is preload-only and labelled so |
| D1.3 | "report p50, p99, and p999 latency" | both tracks; per-run histograms in `d1/concurrent/hist/`; maxima also reported; every reported percentile recomputed from its histogram by the aggregator | PASS |
| D1.4 | "Report the reader-thread count at which each lock-based configuration stops scaling" | global mutex: gains nothing beyond 1 reader with a paced writer and falls at 8; shared mutex: after {{ stop_after('rwlock','writer_paced') }} readers. Given at both lock placements (offsets 48 and 0), with flame graphs and `perf c2c`; report §2.2 | PASS, with the qualification that the lock stores have two speeds (D-072) |
| D1.5 | "confirm whether the RCU configuration sustains 100,000+ RPS at 8 reader threads" | {{ M(d1('rcu','writer_paced',8,'read_rps_median')) }} M reads/s (paced writer), {{ M(d1('rcu','writer_max',8,'read_rps_median')) }} M (unthrottled), {{ M(ref('rcu',8,'rps_median')) }} M lookups/s (generator); the minimum of 5 runs is {{ n0(min(d1('rcu','writer_paced',8,'read_rps_min'),d1('rcu','writer_max',8,'read_rps_min'),ref('rcu',8,'rps_min'))) }} in the worst case; unwrapped runs; report §2.3 | PASS — target met |
| D1.6 | "If it doesn't, report the number … and explain the remaining bottleneck, backed by a flame graph" | not applicable (target met); {{ count('profiling/flamegraphs/*.svg') }} flame graphs taken anyway, `profiling/flamegraphs/`, used in §2.2, §2.5 and §5 | PASS (n/a) |

## D.2 Reclamation-batch tuning (17 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D2.1 | "Run the same point-get benchmark at 4 or more different reclaim-batch settings (for example, 8, 64, 256, and 1024)" | 5 settings: 1, 8, 64, 256, 1024; workload A = the D.1 benchmark; `d2/steady/`; no pass freed more than its batch (checked per run) | PASS |
| D2.2 | "For each value, report … (a) sustained read RPS, and (b) the longest single reader stall you observed. A latency histogram is enough" | report §3.1; histograms `d2/*/hist/`; stall given per run (median and worst of 5) | PASS |
| D2.3 | "a bigger batch should mean higher throughput but a longer worst-case stall" | **Not observed.** In two workloads, no measure taken depends on the batch: read throughput {{ M(min(batches('A','read_rps_median'))) }}–{{ M(max(batches('A','read_rps_median'))) }} M/s, longest stall {{ us(min(batches('A','max_stall_ns_median')),0) }}–{{ us(max(batches('A','max_stall_ns_median')),0) }} µs (workload A), the same as with reclamation disabled. Reported as measured, with the design reason (§3.2); a backlog-building workload was added to give the batch something to trade off (`d2/backlog/`) and shows none either | NOTE: measured; the expected trade-off did not appear and is not claimed |
| D2.4 | "run the same benchmark with reclamation disabled entirely" | `--reclaim disabled`, 1 s window, 16 GB memory guard never reached (largest: {{ n1(d2a('reclaim_disabled','memory_at_end_mib_median')/1024) }} GiB); both workloads | PASS |
| D2.5 | "implement a naive stop-the-world reclaim just for this comparison: freeze all readers, walk the entire retire list, and free everything at once" | `ReclaimMode::StopTheWorld` (comparator only; frees only versions that pass the epoch test); both workloads | PASS |
| D2.6 | "Report both numbers for each reference point too" | report §3.1, §3.3 | PASS |

## D.3 Join and multi-key write benchmarking (16 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D3.1 | "Report index-nested-loop join throughput and latency at 8 reader threads with the background writer active, and compare it against the same join run with the writer paused" | `d3/join/` (also 1, 2, 4 joiners); report §4.1: {{ n0(join(8,'off','joins_per_s')) }} → {{ n0(join(8,'on','joins_per_s')) }} joins/s, p50 {{ n2(join(8,'off','join_p50_ms')) }} → {{ n2(join(8,'on','join_p50_ms')) }} ms | PASS, with the Part C deviation: one Snapshot per store (two handles; stable per-store versions, not one atomic cross-store snapshot) |
| D3.2 | "Report multi-key transaction commit throughput as a function of transaction size (1, 4, and 16 keys per transaction)" | `d3/txn/`; report §4.2: {{ n0(txn(1,'off','commits_per_s')) }} / {{ n0(txn(4,'off','commits_per_s')) }} / {{ n0(txn(16,'off','commits_per_s')) }} commits/s (without and with a secondary index) | PASS |
| D3.3 | "report how much your bounded reclamation strategy reduces worst-case reader pause compared to the naive stop-the-world reclaim from D.2" | report §3.3: {{ n0(d2a('reclaim_stw','max_stall_ns_median')/worst_batch('A')[1]) }}× (workload A), {{ n0(d2b('reclaim_stw','max_stall_ns_median')/worst_batch('B')[1]) }}× (workload B) | PASS |
| D3.4 | "Explain a workload where that copying approach helps little or even hurts, for example a very large transaction …, and explain why" | report §4.3, backed by `d3/cow/` measurements | PASS |

## Deliverable

| Requirement | Evidence | Status |
|---|---|---|
| "results, explanations, and answers for D.1–D.3" | `reports/part_d_report.md` / `.pdf`; `results/part_d/summary/` | PASS |
| "plus the raw perf/latency-histogram data backing each of them" | timing track: one histogram and one stdout (command, `wrapper: none`, exit status) per run; profiling track: perf stat files, flame graphs with folded stacks and compressed `perf script` output, perf c2c reports | PASS |

## Method requirements of the correction (user instruction, decision D-071)

| Requirement | Evidence | Status |
|---|---|---|
| Timed runs not wrapped in a profiler or tracer | every timing run's file records its command and `wrapper: none`; aggregator and gate `part_d_artifacts` reject anything else | PASS |
| Profiling in separate sessions; profiled throughput never in a timing table | `profiling/`; report §5 only | PASS |
| Pilot before the full matrix | `results/part_d_pilot_unwrapped/` | PASS |
| Five repetitions per cell; every required cell present | aggregator: 0 problems (`summary/problems.txt` empty) | PASS |
| Percentiles correct; no duplicate or malformed record | recomputed from raw histograms for {{ count('d1/concurrent/hist/*.csv') + count('d2/*/hist/*.csv') + count('d3/txn/hist/*.csv') + count('d3/join/hist/*.csv') }} runs; aggregator audits | PASS |
| Power source and governor unchanged | {{ ncond() }} condition records, all mains + `performance` | PASS |
| Original campaign preserved | `results/part_d/original-wrapped-20261013T090846-615f4357f015/`, Git tree identical to `part-d-final:results/part_d` (gate) | PASS |
| No number carried over from superseded data | every number in the report, this checklist and the milestone report is computed from the primary campaign at render time (gate checks the rendering is current) | PASS |
| Store unchanged | `git diff part-c-final HEAD -- src include` is empty | PASS |

## Qualifications carried into every Part D statement

1. **Two workload tracks.** The supplied generator is preload-only (no writer during its
   measured phase) and {{ pct(ref('rcu',8,'expected_overall_hit_rate'),2) }} % of its lookups hit; it is never
   described as a concurrent-writer measurement. Active-writer numbers come from
   `benchmarks/concurrent_bench.cpp`.
2. **RCU versus the lock variants** compares two storage and update strategies, not only two
   synchronization primitives.
3. **The lock-based stores have two speeds**, set by where their lock object lies in its
   cache line; both are reported (offsets 48 and 0). The supplied drivers' placement cannot
   be chosen (shared-mutex driver: 0; global-mutex driver: 48). Why the placement matters is
   a hypothesis.
4. **The join uses one Snapshot per store** (decision D-060): each table is read from one
   version; the pair is not an atomic cross-store snapshot; two handles, not the PDF's single
   handle.
5. **The generator's `use_join` path is an index probe, not a join**; its measured hit rate
   ({{ pct(gen('join','probe_hit_rate')) }} % in these binaries) is reported with it.
6. **With an unthrottled writer and 4 or 8 readers the RCU store is not in a steady state**:
   the cleaner falls behind and memory grows for as long as the writer runs flat out.
7. Throughput includes the harness's two clock reads per operation.
8. One machine and one C library; mains power and `performance` governor verified at
   {{ ncond() }} checkpoints; three sessions of this machine differ by a few percent even where
   nothing was changed.
