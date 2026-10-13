# Part D compliance checklist

Requirement by requirement, from `assignment2.pdf` pp. 5–6 (Part D) and p. 7 (marks). Status
values: PASS (measured, with the evidence named), PARTIAL, FAIL, NOT RUN, NOTE (met, with a
stated qualification). All evidence is under `results/part_d/` (campaign
`20261013T090846-615f4357f015-all`, 300 runs, 5 repetitions per cell, 0 failures); the report
is `reports/part_d_report.md` / `.pdf`.

## D.1 Point-query throughput and the 100k RPS target (17 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D1.1 | "Using the workload generator (Appendix A) with its default parameters, benchmark point-get RPS for all four configurations (single thread, global lock, rwlock, and your Part B RCU store) at 1, 2, 4, and 8 reader threads" | Track R: `d1/reference/` (13 cells × 5; single thread at 1 reader only); report §2.4 | PASS |
| D1.2 | "with one background writer thread active throughout" | **The supplied generator cannot do this** (its writer finishes before readers start). Measured with the repository's own harness on the same schema and read mix: track C, `d1/concurrent/` (24 cells × 5 + 7 extra cells), writer paced and unthrottled, successful updates counted; report §2.1 | NOTE: met by track C; track R is preload-only and labelled so |
| D1.3 | "report p50, p99, and p999 latency" | both tracks; per-run histograms in `d1/concurrent/hist/`; maxima also reported | PASS |
| D1.4 | "Report the reader-thread count at which each lock-based configuration stops scaling" | global mutex: after 4 readers; shared mutex: after 4 (paced writer); with flame-graph and c2c evidence; report §2.2 | PASS |
| D1.5 | "confirm whether the RCU configuration sustains 100,000+ RPS at 8 reader threads" | 99.2 M reads/s (paced writer), 50.1 M (unthrottled), 84.2 M lookups/s (generator); minimum of 5 runs in each case far above 100,000; report §2.3 | PASS — target met |
| D1.6 | "If it doesn't, report the number … and explain the remaining bottleneck, backed by a flame graph" | not applicable (target met); 9 flame graphs taken anyway, `perf/flamegraphs/`, used in §2.2 and §2.5 | PASS (n/a) |

## D.2 Reclamation-batch tuning (17 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D2.1 | "Run the same point-get benchmark at 4 or more different reclaim-batch settings (for example, 8, 64, 256, and 1024)" | 5 settings: 1, 8, 64, 256, 1024; workload A = the D.1 benchmark; `d2/steady/` | PASS |
| D2.2 | "For each value, report … (a) sustained read RPS, and (b) the longest single reader stall you observed. A latency histogram is enough" | report §3.1; histograms `d2/*/hist/`; stall given per run (median and worst of 5) | PASS |
| D2.3 | "a bigger batch should mean higher throughput but a longer worst-case stall" | **Not observed.** Read throughput and the longest stall do not depend on the batch in either workload; the batch changes reclamation throughput and memory. Reported as measured, with the reason (§3.2); a second, backlog-building workload was added to give the batch something to trade off (`d2/backlog/`) | NOTE: measured; the expected trade-off did not appear and is not claimed |
| D2.4 | "run the same benchmark with reclamation disabled entirely" | `--reclaim disabled`, 1 s window, memory guard; both workloads | PASS |
| D2.5 | "implement a naive stop-the-world reclaim just for this comparison: freeze all readers, walk the entire retire list, and free everything at once" | `ReclaimMode::StopTheWorld` (comparator only); both workloads | PASS |
| D2.6 | "Report both numbers for each reference point too" | report §3.1, §3.3 | PASS |

## D.3 Join and multi-key write benchmarking (16 marks)

| # | Requirement | Evidence | Status |
|---|---|---|---|
| D3.1 | "Report index-nested-loop join throughput and latency at 8 reader threads with the background writer active, and compare it against the same join run with the writer paused" | `d3/join/` (also 1, 2, 4 joiners); report §4.1: 1,026 → 580 joins/s, p50 7.14 → 13.15 ms | PASS, with the Part C deviation: one Snapshot per store (two handles) |
| D3.2 | "Report multi-key transaction commit throughput as a function of transaction size (1, 4, and 16 keys per transaction)" | `d3/txn/`; report §4.2 (without and with a secondary index) | PASS |
| D3.3 | "report how much your bounded reclamation strategy reduces worst-case reader pause compared to the naive stop-the-world reclaim from D.2" | report §3.3: 28× (workload A), 87× (workload B) | PASS |
| D3.4 | "Explain a workload where that copying approach helps little or even hurts, for example a very large transaction …, and explain why" | report §4.3, backed by `d3/cow/` measurements | PASS |

## Deliverable

| Requirement | Evidence | Status |
|---|---|---|
| "results, explanations, and answers for D.1–D.3" | `reports/part_d_report.md` / `.pdf`; `results/part_d/summary/` | PASS |
| "plus the raw perf/latency-histogram data backing each of them" | one histogram, one perf stat file, one stdout (command + exit status) per run; flame graphs with folded stacks and compressed `perf script` output; perf c2c reports | PASS |

## Qualifications carried into every Part D statement

1. **Two tracks.** The supplied generator is preload-only (no writer during its measured
   phase) and 80.02 % of its lookups hit; it is never described as a concurrent-writer
   measurement. Active-writer numbers come from `benchmarks/concurrent_bench.cpp`.
2. **RCU versus the lock variants** compares two storage and update strategies, not only two
   synchronization primitives.
3. **The join uses one Snapshot per store** (decision D-060): each table is read from one
   version; the pair is not an atomic cross-store snapshot; two handles, not the PDF's single
   handle.
4. **The generator's `use_join` path is an index probe, not a join**; its measured hit rate
   (100 % in these binaries) is reported with it.
5. **With an unthrottled writer and 4 or 8 readers the RCU store is not in a steady state**:
   the cleaner falls behind and memory grows for as long as the writer runs flat out.
6. Throughput includes the harness's two clock reads per operation.
7. Every timed run was wrapped in `perf stat`; a later check shows the wrapper changes the
   global-mutex figure at 8 readers by about +20 % and halves stop-the-world read throughput
   (stalls unaffected); RCU figures move by at most 4 %
   (`results/part_d_diagnostics/perf_stat_wrapper_effect.txt`).
8. One machine; mains power and `performance` governor verified at 56 checkpoints; battery
   charging and temperature rising during the campaign.
