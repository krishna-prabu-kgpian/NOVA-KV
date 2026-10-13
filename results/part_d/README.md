# results/part_d — Part D evidence

| Directory | What it is | Use |
|---|---|---|
| `unwrapped-20261013T115839-fcd2938f49b6/` | **Corrected campaign** `20261013T115839-fcd2938f49b6-all`: timing runs started directly (no profiler); separate profiling track under `profiling/` | the source of every Part D result |
| `original-wrapped-20261013T090846-615f4357f015/` | Original campaign `20261013T090846-615f4357f015-all`: every timed run wrapped in `perf stat` | audit only; superseded for timing (decisions D-070, D-071). Identical to `results/part_d/` at tag `part-d-final` |
| `unwrapped-20261013T105136-146b851dc3dd/` | Superseded unwrapped campaign `20261013T105136-146b851dc3dd-all`: timing unwrapped, but the lock objects' cache-line placement was not controlled | audit only: it is the campaign that exposed the placement effect (decision D-072) |
| `summary/`, `figures/` | copies of the corrected campaign's `summary/` and `figures/`, plus `summary/original_vs_corrected.md` | what the report embeds |

`PRIMARY` names the corrected campaign. Regenerate with `scripts/publish_part_d.sh results/part_d/unwrapped-20261013T115839-fcd2938f49b6`.
Inside the corrected campaign: `d1/ d2/ d3/` are the TIMING track; `profiling/perf_stat`, `profiling/flamegraphs`,
`profiling/c2c` are the PROFILING track (never a source of throughput or latency).
