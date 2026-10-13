# The three Part D campaigns side by side

| | Campaign | Timed runs | Lock placement (lock-based stores) | Status |
|---|---|---|---|---|
| original | `20261013T090846-615f4357f015-all` | **wrapped in `perf stat`** | not controlled | superseded (D-070, D-071) |
| middle | `20261013T105136-146b851dc3dd-all` | started directly | **not controlled** | superseded for the lock-based stores (D-072) |
| final | `20261013T115839-fcd2938f49b6-all` | started directly | controlled: offset 48 and offset 0 | **source of every result** |

Medians of five repetitions. **This table explains the corrections; it is not a result**, and nothing in the
report is a ratio across campaigns. Three sessions of one machine, all on mains power with the `performance`
governor.

## D.1 track C, lock-based stores: reads/s (millions), with the lock's offset in its cache line

In the two earlier campaigns the offset was an accident of each cell's command line (shown in brackets). Read
each earlier figure against the final column with the same offset: 16 and 32 behave like 0.

| Store | Writer | Readers | Original, wrapped [offset] | Middle, unwrapped [offset] | Final, offset 48 | Final, offset 0 |
|---|---|---:|---:|---:|---:|---:|
| global_lock | paced | 1 | 17.00 [16] | 16.03 [32] | 16.12 | 16.10 |
| global_lock | paced | 2 | 18.47 [48] | 15.40 [0] | 15.28 | 15.73 |
| global_lock | paced | 4 | 21.73 [48] | 16.01 [0] | 16.91 | 16.31 |
| global_lock | paced | 8 | 12.39 [32] | 11.24 [48] | 11.30 | 9.30 |
| global_lock | max | 1 | 5.93 [16] | 4.95 [32] | 4.70 | 5.97 |
| global_lock | max | 2 | 7.14 [48] | 7.49 [0] | 6.69 | 7.53 |
| global_lock | max | 4 | 9.53 [48] | 8.14 [0] | 7.32 | 8.07 |
| global_lock | max | 8 | 8.37 [32] | 6.60 [48] | 6.70 | 7.15 |
| global_lock | refkeys_paced | 8 | 13.40 [32] | 14.03 [48] | 14.17 | 10.34 |
| rwlock | paced | 1 | 16.86 [16] | 15.95 [32] | 15.90 | 15.98 |
| rwlock | paced | 2 | 23.49 [48] | 21.82 [0] | 23.77 | 22.36 |
| rwlock | paced | 4 | 34.28 [48] | 28.48 [0] | 36.93 | 28.61 |
| rwlock | paced | 8 | 21.37 [32] | 34.17 [48] | 34.17 | 21.27 |
| rwlock | max | 1 | 0.14 [16] | 0.29 [0] | 0.30 | 0.29 |
| rwlock | max | 2 | 0.49 [48] | 1.06 [32] | 1.08 | 1.04 |
| rwlock | max | 4 | 1.57 [48] | 5.05 [32] | 4.08 | 4.01 |
| rwlock | max | 8 | 21.30 [32] | 21.95 [16] | 18.39 | 21.36 |
| rwlock | refkeys_paced | 8 | 22.47 [32] | 34.73 [48] | 34.68 | 22.74 |

## D.1 track C, stores without a reader-side lock: reads/s (millions)

| Store | Scenario | Readers | Original, wrapped | Middle, unwrapped | Final |
|---|---|---:|---:|---:|---:|
| rcu | readonly | 1 | 16.67 | 15.96 | 16.06 |
| rcu | readonly | 8 | 112.26 | 112.58 | 112.79 |
| rcu | refkeys_paced | 8 | 105.27 | 104.74 | 104.24 |
| rcu | writer_max | 1 | 8.36 | 8.37 | 8.41 |
| rcu | writer_max | 2 | 18.00 | 15.51 | 17.16 |
| rcu | writer_max | 4 | 28.69 | 28.38 | 28.30 |
| rcu | writer_max | 8 | 50.09 | 47.99 | 47.97 |
| rcu | writer_paced | 1 | 14.81 | 14.01 | 13.97 |
| rcu | writer_paced | 2 | 30.39 | 29.47 | 29.30 |
| rcu | writer_paced | 4 | 60.44 | 59.60 | 59.76 |
| rcu | writer_paced | 8 | 99.25 | 98.52 | 98.25 |
| single | inline_writer | 1 | 18.21 | 16.83 | 16.78 |
| single | readonly | 1 | 19.12 | 18.32 | 18.32 |

## D.1 track R: supplied generator, PRELOAD-ONLY — never wrapped, arguments fixed (the session-to-session yardstick)



| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |
|---|---|---:|---:|---:|---:|
| global_lock, 1 reader(s) | lookups/s | 15,576,200 | 15,237,100 | 15,302,300 | 0.98 |
| global_lock, 2 reader(s) | lookups/s | 15,738,100 | 14,982,900 | 15,203,600 | 0.97 |
| global_lock, 4 reader(s) | lookups/s | 18,386,900 | 16,980,300 | 17,058,200 | 0.93 |
| global_lock, 8 reader(s) | lookups/s | 12,054,600 | 11,746,900 | 11,870,000 | 0.98 |
| rcu, 1 reader(s) | lookups/s | 15,070,800 | 14,907,800 | 14,899,100 | 0.99 |
| rcu, 2 reader(s) | lookups/s | 25,693,700 | 25,608,100 | 25,560,400 | 0.99 |
| rcu, 4 reader(s) | lookups/s | 49,292,500 | 50,640,800 | 49,790,300 | 1.01 |
| rcu, 8 reader(s) | lookups/s | 84,185,200 | 81,750,100 | 82,169,200 | 0.98 |
| rwlock, 1 reader(s) | lookups/s | 15,503,500 | 15,194,900 | 15,228,800 | 0.98 |
| rwlock, 2 reader(s) | lookups/s | 19,052,900 | 19,413,500 | 19,230,100 | 1.01 |
| rwlock, 4 reader(s) | lookups/s | 24,204,900 | 24,246,700 | 24,006,500 | 0.99 |
| rwlock, 8 reader(s) | lookups/s | 18,657,200 | 18,501,200 | 18,645,200 | 1.00 |
| single_thread, 1 reader(s) | lookups/s | 16,888,600 | 16,508,900 | 16,538,000 | 0.98 |

## D.2 workload A (RCU store)



| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |
|---|---|---:|---:|---:|---:|
| batch_1 | reads/s | 50,994,100 | 48,620,500 | 48,578,100 | 0.95 |
| batch_1 | writer updates/s | 99,939 | 108,816 | 108,776 | 1.09 |
| batch_1 | longest stall ns (median) | 226,859 | 147,127 | 142,220 | 0.63 |
| batch_8 | reads/s | 50,382,100 | 48,393,600 | 48,624,200 | 0.97 |
| batch_8 | writer updates/s | 100,781 | 108,583 | 108,180 | 1.07 |
| batch_8 | longest stall ns (median) | 129,375 | 144,452 | 149,413 | 1.15 |
| batch_64 | reads/s | 50,305,300 | 48,464,000 | 48,302,800 | 0.96 |
| batch_64 | writer updates/s | 101,109 | 105,831 | 110,122 | 1.09 |
| batch_64 | longest stall ns (median) | 55,776 | 153,168 | 146,248 | 2.62 |
| batch_256 | reads/s | 50,631,300 | 48,308,700 | 48,446,200 | 0.96 |
| batch_256 | writer updates/s | 100,803 | 108,767 | 107,915 | 1.07 |
| batch_256 | longest stall ns (median) | 67,618 | 153,038 | 164,131 | 2.43 |
| batch_1024 | reads/s | 50,011,700 | 48,371,000 | 48,461,700 | 0.97 |
| batch_1024 | writer updates/s | 102,624 | 106,720 | 108,887 | 1.06 |
| batch_1024 | longest stall ns (median) | 46,117 | 148,950 | 140,777 | 3.05 |
| reclaim_stw | reads/s | 4,616,620 | 7,047,940 | 7,184,420 | 1.56 |
| reclaim_stw | writer updates/s | 160,704 | 158,482 | 158,638 | 0.99 |
| reclaim_stw | longest stall ns (median) | 6,436,300 | 6,365,770 | 6,818,540 | 1.06 |
| reclaim_disabled | reads/s | 73,969,900 | 73,815,600 | 73,833,500 | 1.00 |
| reclaim_disabled | writer updates/s | 82,840 | 83,681 | 83,815 | 1.01 |
| reclaim_disabled | longest stall ns (median) | 95,711 | 137,108 | 140,065 | 1.46 |

## D.2 workload B (RCU store)



| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |
|---|---|---:|---:|---:|---:|
| batch_1 | reads/s | 37,835,600 | 37,419,400 | 40,362,100 | 1.07 |
| batch_1 | writer updates/s | 197,961 | 224,277 | 222,705 | 1.12 |
| batch_1 | longest stall ns (median) | 234,877 | 186,902 | 149,384 | 0.64 |
| batch_8 | reads/s | 40,300,600 | 40,703,400 | 40,267,100 | 1.00 |
| batch_8 | writer updates/s | 203,182 | 220,220 | 222,679 | 1.10 |
| batch_8 | longest stall ns (median) | 274,802 | 245,632 | 197,415 | 0.72 |
| batch_64 | reads/s | 40,212,500 | 40,210,700 | 40,318,300 | 1.00 |
| batch_64 | writer updates/s | 200,660 | 225,290 | 223,468 | 1.11 |
| batch_64 | longest stall ns (median) | 288,639 | 167,696 | 180,513 | 0.63 |
| batch_256 | reads/s | 40,198,500 | 40,201,200 | 37,179,300 | 0.92 |
| batch_256 | writer updates/s | 201,676 | 224,357 | 223,382 | 1.11 |
| batch_256 | longest stall ns (median) | 203,475 | 160,151 | 149,624 | 0.74 |
| batch_1024 | reads/s | 40,222,000 | 40,415,600 | 37,405,200 | 0.93 |
| batch_1024 | writer updates/s | 199,715 | 222,098 | 225,531 | 1.13 |
| batch_1024 | longest stall ns (median) | 187,505 | 152,697 | 152,990 | 0.82 |
| reclaim_stw | reads/s | 3,415,110 | 6,930,480 | 7,228,820 | 2.12 |
| reclaim_stw | writer updates/s | 295,004 | 295,725 | 293,161 | 0.99 |
| reclaim_stw | longest stall ns (median) | 25,061,100 | 23,363,900 | 22,814,100 | 0.91 |
| reclaim_disabled | reads/s | 43,092,500 | 39,462,100 | 39,442,800 | 0.92 |
| reclaim_disabled | writer updates/s | 164,794 | 170,559 | 171,362 | 1.04 |
| reclaim_disabled | longest stall ns (median) | 53,552 | 145,384 | 151,978 | 2.84 |

## D.3 transactions (RCU store)



| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |
|---|---|---:|---:|---:|---:|
| 1 key(s), index off | commit_p50_us | 3.667 | 3.577 | 3.567 | 0.97 |
| 1 key(s), index off | commit_p99_us | 8.726 | 8.335 | 8.396 | 0.96 |
| 1 key(s), index off | commits_per_s | 238,103 | 239,561 | 236,846 | 0.99 |
| 1 key(s), index on | commit_p50_us | 16.341 | 16.341 | 16.391 | 1.00 |
| 1 key(s), index on | commit_p99_us | 29.566 | 29.144 | 29.355 | 0.99 |
| 1 key(s), index on | commits_per_s | 58,824 | 58,896 | 58,726 | 1.00 |
| 4 key(s), index off | commit_p50_us | 9.248 | 8.877 | 8.887 | 0.96 |
| 4 key(s), index off | commit_p99_us | 18.595 | 17.133 | 17.513 | 0.94 |
| 4 key(s), index off | commits_per_s | 101,298 | 102,554 | 101,070 | 1.00 |
| 4 key(s), index on | commit_p50_us | 56.977 | 57.729 | 57.068 | 1.00 |
| 4 key(s), index on | commit_p99_us | 76.214 | 76.273 | 75.724 | 0.99 |
| 4 key(s), index on | commits_per_s | 17,555 | 17,372 | 17,553 | 1.00 |
| 16 key(s), index off | commit_p50_us | 37.461 | 26.109 | 27.382 | 0.73 |
| 16 key(s), index off | commit_p99_us | 69.401 | 70.753 | 72.587 | 1.05 |
| 16 key(s), index off | commits_per_s | 26,655 | 27,341 | 26,544 | 1.00 |
| 16 key(s), index on | commit_p50_us | 161.606 | 159.611 | 160.033 | 0.99 |
| 16 key(s), index on | commit_p99_us | 207.753 | 205.086 | 208.686 | 1.00 |
| 16 key(s), index on | commits_per_s | 6,134 | 6,209 | 6,188 | 1.01 |

## D.3 join (RCU store)



| Cell | Measure | Original, wrapped | Middle, unwrapped | Final | Final / original |
|---|---|---:|---:|---:|---:|
| 1 joiner(s), writer paused | join_p50_ms | 6.261 | 6.279 | 6.294 | 1.01 |
| 1 joiner(s), writer paused | join_p99_ms | 6.654 | 8.245 | 8.940 | 1.34 |
| 1 joiner(s), writer paused | joins_per_s | 158.965 | 155.307 | 154.156 | 0.97 |
| 1 joiner(s), writer active | join_p50_ms | 15.116 | 15.780 | 15.790 | 1.04 |
| 1 joiner(s), writer active | join_p99_ms | 16.456 | 17.379 | 17.348 | 1.05 |
| 1 joiner(s), writer active | joins_per_s | 65.582 | 63.489 | 62.977 | 0.96 |
| 2 joiner(s), writer paused | join_p50_ms | 6.481 | 6.455 | 6.382 | 0.98 |
| 2 joiner(s), writer paused | join_p99_ms | 6.887 | 8.279 | 8.108 | 1.18 |
| 2 joiner(s), writer paused | joins_per_s | 307.761 | 303.613 | 306.503 | 1.00 |
| 2 joiner(s), writer active | join_p50_ms | 12.858 | 13.276 | 13.239 | 1.03 |
| 2 joiner(s), writer active | join_p99_ms | 14.454 | 15.114 | 15.056 | 1.04 |
| 2 joiner(s), writer active | joins_per_s | 153.642 | 148.874 | 148.919 | 0.97 |
| 4 joiner(s), writer paused | join_p50_ms | 6.777 | 6.677 | 6.748 | 1.00 |
| 4 joiner(s), writer paused | join_p99_ms | 7.123 | 8.590 | 8.425 | 1.18 |
| 4 joiner(s), writer paused | joins_per_s | 588.730 | 583.776 | 582.955 | 0.99 |
| 4 joiner(s), writer active | join_p50_ms | 13.263 | 13.611 | 13.550 | 1.02 |
| 4 joiner(s), writer active | join_p99_ms | 14.996 | 15.392 | 15.603 | 1.04 |
| 4 joiner(s), writer active | joins_per_s | 297.758 | 290.336 | 292.541 | 0.98 |
| 8 joiner(s), writer paused | join_p50_ms | 7.139 | 6.975 | 7.006 | 0.98 |
| 8 joiner(s), writer paused | join_p99_ms | 9.337 | 9.566 | 9.514 | 1.02 |
| 8 joiner(s), writer paused | joins_per_s | 1,026 | 1,045 | 1,048 | 1.02 |
| 8 joiner(s), writer active | join_p50_ms | 13.153 | 12.797 | 13.738 | 1.04 |
| 8 joiner(s), writer active | join_p99_ms | 21.119 | 21.589 | 22.841 | 1.08 |
| 8 joiner(s), writer active | joins_per_s | 580.273 | 600.665 | 557.689 | 0.96 |
