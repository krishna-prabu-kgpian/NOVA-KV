# NOVA-KV — Mandatory Final Submission Gate

**Status:** Instructions for the coding agent. No implementation, benchmark, or submission has yet been verified by this checklist.

## Final-delivery requirement

The agent must **actually assemble** a single upload-ready archive, not merely describe files. Its name must be:

`Asgn2_<RollNumber1>_<RollNumber2>[_<RollNumber3>][_<RollNumber4>].zip`

Obtain team roll numbers from the project owner before choosing the final filename; do not invent them. If numbers are unavailable, create an explicitly marked provisional ZIP and list the final rename as BLOCKED. Submission to Moodle itself remains the team's responsibility unless expressly authorized and supported.

## Required contents and verification

- [ ] Part A: `kvstore.h`, `kvstore.cpp`; `kvstore_test.cpp`; `single_thread.cpp`, `global_lock.cpp`, `rwlock.cpp` (or buildable submission aliases); unchanged provided `workload_gen.cpp` and header faithfully transcribed from PDF Appendix A.2 if separately unavailable; independent NSM/DSM/PAX microbenchmark comparing at least two layouts; baseline `perf stat`, `perf record`, `perf script`, folded stacks and flame graphs at 1/2/4/8 readers for mutex and shared_mutex; Part A report **≤5 pages**, design note **≤3 of those pages**.
- [ ] Part B: Part B store source and `part_a` → `part_b` source diff; `set_reclaim_batch(int)` exposed; README describing epoch protocol and reclamation tuning; reader/writer no-torn-read correctness test and evidence; bounded reclamation verification.
- [ ] Part C: `Transaction` and `Snapshot` APIs exactly as Appendix A.1, secondary index and required keys/index_get/get snapshot-scoped behavior; Part B → C diff; design note on atomicity/isolation, write skew, lack of durability, and index-nested-loop join. **Do not claim exact-one-snapshot cross-store compliance if implementation pins two independent stores instead.** Flag unresolved deviation prominently.
- [ ] Part D: Four read configurations and 1/2/4/8 reader counts (explain single-thread baseline treatment), actual background writer where mandated, RPS and p50/p99/p999; whether 100k+ RPS achieved at 8 readers; reclaim-batch settings (at least 4), no-reclamation and stop-the-world comparators, max observed reader stall; join throughput/latency with writer active versus paused; commit throughput at transaction sizes 1/4/16; relevant flame graphs and raw perf/latency data; explanation of COW disadvantage and scalability.
- [ ] Part E (optional only if implemented): WAL sources, replay and crash tests, Part E diff, WAL write-side benchmark with/without durability, fsync/group-commit policy and half-page production-gap note. If not implemented, explicitly label `BONUS NOT ATTEMPTED` rather than implying completion.
- [ ] Build system, build instructions, dependency and tool versions, test instructions, test outputs, raw benchmark files, machine/CPU/OS/compiler metadata, scripts needed to regenerate tables/plots, and final README.
- [ ] Git tags/SHAs for Parts A–C and exported diffs; ensure all required code and external scripts needed to reproduce are packaged, or provide documented retrieval instructions for tools that must be installed.

## Pre-archive gate

1. Build from a **fresh clean checkout**, with no hidden local dependencies or stale generated files.
2. Run all functional tests, deterministic interleaving tests, sanitizer-compatible tests, and mandated benchmark targets. Record commands, exit codes, evidence paths, and failures.
3. Check every assignment rubric item against a named artifact; create `submission_manifest.md` with `Requirement | Artifact path | Evidence | PASS/FAIL/NOT RUN`.
4. Validate source contracts against the PDF, the workload source remains unchanged, and compile the actual files that will be archived.
5. Check Part A page limits and report final page counts after rendering; do not assume `.md` line counts are page counts.
6. Inspect all results: ensure no placeholder figures, invented performance data, omitted profiles, or tests that were listed as executed but actually skipped.
7. If any mandatory requirement fails, report **NOT SUBMISSION READY**; do not conceal it or mark the ZIP complete.

## Archive gate

1. Assemble the ZIP with required source, reports, plots, raw data, and scripts. Exclude secrets, unnecessary build binaries, caches, personal files, and unrelated artifacts.
2. Enumerate archive entries and run CRC test (`unzip -t`).
3. Extract into a fresh temporary directory. Build and execute the packaged smoke tests **from the extracted files**.
4. Run an automated artifact-presence validator (`scripts/verify_submission.sh`) against the extracted directory; compare with the PDF rubric and submission manifest.
5. Save `submission_manifest.md`, `submission_audit.md` (date, commit SHA, environment, evidence, failures), archive SHA-256, and verification logs within the final deliverable package wherever feasible.
6. Return the actual ZIP path plus a summary of mandatory PASS/FAIL/NOT RUN items and list of any deviations. **Only describe it as upload-ready if every mandatory gate passes.**

## User handoff

The agent must present the exact final filename, checksum, artifact summary, test/benchmark status, and any instructor-dependent interpretation. It must not claim to have uploaded to Moodle. The project owner must provide team roll numbers and independently perform the upload.
