### Part B store, epoch-based reclamation, reclaim batch 64

| scenario | R | read M/s | min–max | writes/s | p50 µs | p99 µs | p99.9 µs | max µs | backlog max | cleaner CPU s | held at end MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| readonly | 1 | 6.01 | 5.15–6.58 | 0 | 0.09 | 0.61 | 0.81 | 174 | 168 | 0.04 | 9.0 |
| inline\_writer | 1 | 3.37 | 3.24–3.59 | 33,746 | 0.10 | 0.60 | 0.83 | 172 | 1 | 0.00 | 12.8 |
| writer\_paced | 1 | 5.21 | 4.54–5.42 | 10,000 | 0.12 | 0.48 | 0.72 | 163 | 441 | 0.21 | 10.1 |
| writer\_paced | 2 | 11.22 | 10.13–11.26 | 10,000 | 0.11 | 0.40 | 0.59 | 158 | 104 | 0.22 | 10.1 |
| writer\_paced | 4 | 22.94 | 22.62–23.10 | 10,000 | 0.11 | 0.29 | 0.51 | 164 | 107 | 0.24 | 10.1 |
| writer\_paced | 8 | 44.57 | 43.42–44.67 | 9,999 | 0.12 | 0.27 | 0.46 | 195 | 174 | 0.26 | 10.1 |
| writer\_max | 1 | 3.84 | 3.74–3.89 | 92,779 | 0.17 | 0.58 | 0.77 | 159 | 113 | 2.82 | 19.0 |
| writer\_max | 2 | 8.03 | 7.95–8.07 | 92,312 | 0.17 | 0.55 | 0.73 | 156 | 171 | 2.68 | 19.0 |
| writer\_max | 4 | 16.74 | 15.68–16.88 | 87,088 | 0.16 | 0.53 | 0.71 | 163 | 105 | 2.92 | 18.4 |
| writer\_max | 8 | 31.13 | 30.66–32.25 | 81,532 | 0.17 | 0.55 | 0.81 | 1,172 | 140 | 2.59 | 18.2 |

### Reclaim batch at 8 readers, unthrottled writer (3 s window unless noted)

| batch | read M/s | min–max | writes/s | p99 µs | p99.9 µs | worst max µs | median max µs | versions reclaimed | passes | max versions in a pass | backlog max | cleaner CPU s | held at end MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 29.88 | 28.99–30.78 | 74,219 | 0.58 | 0.87 | 241 | 240 | 259,346 | 271,658 | 1 | 569 | 2.69 | 17.1 |
| 8 | 30.93 | 30.75–31.83 | 81,588 | 0.55 | 0.79 | 3,014 | 158 | 282,241 | 83,223 | 8 | 195 | 2.67 | 17.8 |
| 64 | 31.48 | 29.41–31.95 | 80,489 | 0.53 | 0.75 | 1,743 | 162 | 279,215 | 62,344 | 64 | 115 | 2.49 | 17.7 |
| 256 | 31.20 | 29.60–32.07 | 81,948 | 0.54 | 0.75 | 1,069 | 205 | 283,983 | 63,030 | 232 | 244 | 2.53 | 17.8 |
| 1024 | 31.60 | 30.57–31.88 | 80,078 | 0.54 | 0.78 | 263 | 232 | 278,880 | 62,800 | 230 | 239 | 2.43 | 17.7 |
| disabled (1 s) | 29.49 | 28.66–30.42 | 34,160 | 0.71 | 1.04 | 1,227 | 171 | 0 | 5,134 | n/a | 144,632 | 0.00 | 2265.4 |
| stop-the-world (3 s) | 2.94 | 2.80–3.01 | 75,796 | 2.05 | 3.90 | 44,408 | 41,619 | 264,871 | 5,413 | n/a | 1,201 | 2.53 | 17.2 |

### Same-session references (same harness, pinning, machine state)

| store | scenario | R | window s | read M/s | min–max | writes/s | p50 µs | p99 µs | p99.9 µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Part B (B2), batch 64 | readonly | 1 | 3 | 6.01 | 5.15–6.58 | 0 | 0.09 | 0.61 | 0.81 |
| Part B (B2), batch 64 | writer\_paced | 1 | 3 | 5.21 | 4.54–5.42 | 10,000 | 0.12 | 0.48 | 0.72 |
| Part B (B2), batch 64 | writer\_paced | 8 | 3 | 44.57 | 43.42–44.67 | 9,999 | 0.12 | 0.27 | 0.46 |
| Part B (B2), batch 64 | writer\_max | 1 | 3 | 3.84 | 3.74–3.89 | 92,779 | 0.17 | 0.58 | 0.77 |
| Part B (B2), batch 64 | writer\_max | 8 | 3 | 31.13 | 30.66–32.25 | 81,532 | 0.17 | 0.55 | 0.81 |
| B1 checkpoint (no reclamation, no epoch announcement) | readonly | 1 | 3 | 6.97 | 5.88–7.20 | 0 | 0.08 | 0.39 | 0.68 |
| B1 checkpoint (no reclamation, no epoch announcement) | writer\_paced | 1 | 3 | 4.87 | 4.62–4.93 | 10,000 | 0.11 | 0.69 | 0.88 |
| B1 checkpoint (no reclamation, no epoch announcement) | writer\_paced | 8 | 3 | 44.01 | 41.05–44.58 | 10,000 | 0.11 | 0.38 | 0.60 |
| B1 checkpoint (no reclamation, no epoch announcement) | writer\_max | 1 | 1 | 3.47 | 3.44–3.49 | 44,659 | 0.18 | 0.87 | 1.17 |
| B1 checkpoint (no reclamation, no epoch announcement) | writer\_max | 8 | 1 | 29.74 | 25.61–29.76 | 24,728 | 0.17 | 0.65 | 0.92 |
| Part A rwlock | writer\_paced | 1 | 3 | 6.40 | 5.39–6.67 | 10,000 | 0.08 | 0.46 | 0.78 |
| Part A rwlock | writer\_paced | 8 | 3 | 10.69 | 10.65–10.75 | 1,407 | 0.66 | 1.57 | 10.50 |
| Part A rwlock | writer\_max | 1 | 3 | 0.18 | 0.18–0.18 | 177,401 | 4.74 | 17.92 | 31.49 |
| Part A rwlock | writer\_max | 8 | 3 | 10.60 | 10.58–10.64 | 1,852 | 0.67 | 1.57 | 11.65 |
| Part A global\_lock | writer\_paced | 1 | 3 | 6.66 | 5.24–6.67 | 10,000 | 0.08 | 0.36 | 0.66 |
| Part A global\_lock | writer\_paced | 8 | 3 | 4.70 | 3.60–4.74 | 10,000 | 0.35 | 10.24 | 17.41 |
| Part A global\_lock | writer\_max | 1 | 3 | 2.66 | 1.46–2.93 | 1,610,827 | 0.12 | 3.58 | 6.85 |
| Part A global\_lock | writer\_max | 8 | 3 | 3.00 | 2.37–3.37 | 284,738 | 1.63 | 15.49 | 27.39 |
| Part A single thread, no lock | readonly | 1 | 3 | 7.23 | 6.29–7.42 | 0 | 0.07 | 0.38 | 0.70 |

### Copy-on-write cost per publication (8 readers, unthrottled writer)

| segments cloned | index bytes copied | directory bytes copied | segment owner refs | chunk refs | value bytes |
|---|---:|---:|---:|---:|---:|
| 1.00 | 23,934 | 2,048 | 255 | 102.6 | 32 |
