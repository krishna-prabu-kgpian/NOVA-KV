# Pre-B2 audit of milestone B1 — verification record

Date: 2026-10-12. Branch `part-b/rcu`. Audited code: tag `part-b-publication` (`2d9c02c`).
Verified commit for this record: `624f5dbc50bc` (`624f5db` = `2d9c02c` + the audit document,
decisions and one added test schedule; **no change to any file under `src/` or `include/`**).

The audit itself is `docs/b1_audit_and_b2_readiness.md`. This file records what was run.

## Git preservation and push

| Check | Result |
|---|---|
| Tags `a0-bootstrap`, `part-a-core`, `part-a-final`, `part-b-publication` exist locally | PASS (`2ef5f19`, `80bc092`, `3444f23`, `2d9c02c`) |
| Working tree clean before pushing | PASS |
| Remote inspected before pushing: no unexpected refs or changes | PASS (identical to the state after the Part A push) |
| `git push origin part-b/rcu part-b-publication` (no force) | PASS, exit 0 |
| Remote hashes equal local | PASS: `refs/heads/part-b/rcu` = `2d9c02c10d9c6733044f00021725f01174f68c0a`; tag object `58fbb4314d9e5bcd6282b5eaa33abd42092b648a` → commit `2d9c02c1…` |
| `main` and the three earlier tags unchanged on the remote | PASS (`a3cccd8`; tags as before) |

A first attempt failed because the network in use blocked SSH (both ports); nothing was pushed
or altered by it. The push above succeeded after the network was changed.

## Audit results in brief

- No correctness defect found in B1 as specified.
- One ownership path (F2) is unsafe **with a concurrent cleaner**, which B1 does not have;
  reproduced in a scratch experiment, `results/B1/audit/`. Required change R1 before B2.
- Segment owner counts are safe against concurrent release as designed; demonstrated by the
  new schedule S6.
- Required changes R1–R7 and five open decisions are listed in the audit document and in
  `docs/decisions.md` (D-039, O-9…O-13).

## Verification rerun

`scripts/verify_all.sh` on a clean tree at `624f5db`, each mode from a deleted build
directory. Exit status 0; 18 of 18 mandatory gates PASS.

| Mode | Build warnings | Tests | `ctest` exit | Sanitizer reports | Wall time |
|---|---:|---|---:|---:|---:|
| Debug | 0 | 41/41 | 0 | n/a | 35.9 s |
| Release | 0 | 41/41 | 0 | n/a | 27.9 s |
| ASan + UBSan | 0 | 41/41 | 0 | 0 | 75.8 s |
| TSan | 0 | 41/41 | 0 | 0 | 131.3 s |

Specific tests asked about, each PASS in all four modes:

| Test | Result (identical output in all four modes; binaries from `build/verify/<mode>/` re-run by hand, exit 0) |
|---|---|
| `versioned_interleaving_test` (deterministic schedules S1–S4, S6; bounded stress S5) | 572 checks, 0 failed. S6: "120 older versions released while a prepared version shared 36 segments; no shared segment or chunk lost" |
| `versioned_alloc_failure_test` | Retaining: 174 writes, 862 injected failures all rolled back. Quiescent release: 174 writes, 704 injected failures all rolled back |

Part A preservation gates: `part_a_artifacts` PASS, `part_a_report_pages` PASS (4 pages),
`part_a_checkpoint_unchanged` PASS (results, report and milestone reports byte-identical to
tag `part-a-final`), `supplied_files_unchanged` PASS.

Informational gate `perf_stat_usable`: FAIL — `kernel.perf_event_paranoid` is 4 again since the
machine was rebooted. Not needed for this audit; needed for B2 measurements and Part D.

## The scratch experiment (not part of the build)

`results/B1/audit/hazard_experiment.cpp.txt` and `hazard_experiment_output.txt`. Sources of
`2d9c02c` copied to a scratch directory with one extra pause point between the arena appends
and phase 3 of `commit`. Releasing the retained versions at that point frees the chunk the
commit has appended to (`chunks_freed=1`); on resuming, the commit crashes in
`PinnedArena::pin` (`pinned_arena.cpp:198`): exit 139 in a plain build, an AddressSanitizer
SEGV report in an ASan build. This is the behaviour a B2 cleaner would trigger without R1.
The repository's sources do not contain that pause point and no repository test reaches the
window.

## Evidence

| Path | SHA-256 |
|---|---|
| `results/verification/624f5dbc50bc/summary.md` | `0c5647fdfcac40abc2278dc0d3fd52e3d5cb918762ce37106c89ef4362209f6f` |
| `…/logs/debug_ctest.log` | `8850084e3cfe13f573c0d180cd72cf90ed8b6eb8d3ce54444a7d2e19e7387f5e` |
| `…/logs/release_ctest.log` | `d66a3b04e4a44b5defebacbf2ee167094372107afe66bb93b76c66acdf702c9f` |
| `…/logs/asan_ubsan_ctest.log` | `1e4c073788130cb3f6d0a53f34fe075a8da1ddf7a202345d31adebf3ca9da63a` |
| `…/logs/tsan_ctest.log` | `ad48084ddf316355da9106bb67cf2ac53698c49cb6e1f8cda9f1ebbf3cee8a5e` |
| `results/B1/audit/hazard_experiment.cpp.txt` | `d0edd1116227c8438948bde14cba28dffa6c1185eb72c2eafe2707d25e114a5d` |
| `results/B1/audit/hazard_experiment_output.txt` | `a32e3846606c8828d927e31dcc936e82249465fcf070a866935e23a527fcf4f1` |

## Not done

- No B2 code. No change to the B1 implementation.
- The audit is a code review with tests and one experiment; it is not a formal proof.
- The memory-ordering argument remains informal.
- The cache-line hypothesis (R8) was not measured.

## Verdict

B1 acceptance stands: **PASS** for publication and ownership with reclamation disabled.
B2 is **not started** and should begin with R1.
