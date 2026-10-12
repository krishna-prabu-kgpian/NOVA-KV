# Decision log

Project decisions that are **not** dictated by the assignment PDF. Each entry states what was
chosen, why, and what it costs. Architecture decisions already fixed by the blueprint are not
repeated here; only choices the blueprint left to the implementer, and any departure from it.

Status values: `ACCEPTED` (in effect), `OPEN` (needs the user or the instructor).

---

## D-001 — Specification documents and PDF stay at the repository root (ACCEPTED, A0)

The five `NOVA_KV_*.md` documents and `assignment2.pdf` are kept where they were received and
are committed unchanged (commit `a3cccd8`). Blueprint §8.1 sketches `docs/BLUEPRINT.md`; the
documents refer to each other by their root filenames, so they are not moved or renamed.
`CLAUDE.md` indexes them.

## D-002 — Supplied generator lives in `supplied/` (ACCEPTED, A0)

`workload_gen.cpp` was moved from the root to `supplied/workload_gen.cpp` with `git mv`, per
blueprint §8.1. Content is unchanged (SHA-256 pinned in `supplied/SHA256SUMS`, checked by
`scripts/verify_all.sh`). `workload_gen.h` was not supplied as a file and is transcribed from
PDF Appendix A.2; see `supplied/PROVENANCE.md`. The assignment's flat top-level file names
(`kvstore.h`, `kvstore.cpp`, drivers) will be produced by the packaging step in the final
milestone.

## D-003 — Final-compatible public header from the first milestone (ACCEPTED, A0)

Blueprint §8.2 requires a decision on how the unmodified `workload_gen.cpp`, which needs a
complete `Transaction` type, compiles against a Part A store. Chosen: `include/kvstore.h`
declares the complete Appendix A surface (Parts A, B and C) from A0, and `kvstore.cpp` always
defines every declared symbol.

- Until a part is implemented its methods are inert and report failure/"not found"; they never
  report success for work that was not done. In particular a Part A build does **not** claim
  to implement Part C transactions (audit H02).
- Cost: the Part A→B and B→C header diffs will show only private-member changes, since the
  public declarations already exist. The source diffs still show each part's implementation.
- Alternative rejected: a separate "compat" header per part. It would mean the graded header
  differs from the one the tests pin.

## D-004 — `Snapshot` is the nested type `KVStore::Snapshot` (ACCEPTED, A0)

Appendix A.1 introduces `class Snapshot` under the comment "added to KVStore", and the
neighbouring declarations (`acquire_snapshot`, the snapshot overloads) are members. It is
therefore declared as a nested public class. No namespace-scope alias is provided because
nothing supplied needs one (audit H04: "publish type alias only if strictly needed").
`Transaction` is a separate namespace-scope class, as printed.

## D-005 — No `noexcept` on Appendix A functions (ACCEPTED, A0)

In C++17 `noexcept` is part of a function's type. To keep the declarations identical to the
printed contract, no public method carries `noexcept` except the two `Snapshot` move
operations, which the PDF prints as `noexcept`. Allocation failures will be caught at the
public boundary and translated to the documented return value (blueprint §2.2).

## D-006 — Pimpl for private state (ACCEPTED, A0; revisit with data)

`KVStore` holds `Impl* impl_`; `Transaction` holds `KVStore& store_` and `Impl* impl_`;
`Snapshot` holds `const KVStore* owner_` and an opaque `void* pin_`. This keeps the public
header free of implementation types and identical across sync variants.

- Cost: one extra pointer dereference per call. The pointer is immutable after construction,
  so it adds no synchronization. If profiling in Part B/D shows it matters on `get()`, state
  can be moved inline behind the same public API.
- Part B asks for "a private member controlling how many retired versions a reclamation pass
  frees". With pimpl that member would live in `Impl`; whether it should instead be visible
  as a private member in `kvstore.h` will be decided in B2.

## D-007 — One library per synchronization policy, selected at compile time (ACCEPTED, A0)

`src/kvstore.cpp` is compiled into `nova_kv_single`, `nova_kv_global_lock` and
`nova_kv_rwlock`, differing only in `NOVA_KV_SYNC_MODE` (`src/sync_mode.h`). An `rcu` variant
is added in Part B. This satisfies "same underlying data structure across all three drivers,
only the synchronization should differ" (PDF p. 2) and blueprint §11.8, with no runtime
dispatch on the read path. `nova_kv_sync_mode_name()` (internal, `src/build_info.h`) lets
tests and harnesses verify which variant a binary was linked with.

In A0 the three variants are behaviourally identical placeholders; the locking is added in A1.

## D-008 — Build flags (ACCEPTED, A0)

- Release: `-O3 -g -DNDEBUG`. Blueprint §9.3 specifies `-std=c++17 -O3 -g -pthread`;
  `-DNDEBUG` is added so `assert()` cannot put work on the measured hot path. Tests use
  `NOVA_CHECK`, which is independent of `NDEBUG`, so release-mode tests still check.
- Debug: `-O0 -g`.
- `-fno-omit-frame-pointer` in every mode, so `perf record -g` call graphs are usable.
- Project sources: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`. The supplied
  workload is compiled without the warning set because it cannot be edited.
- `NOVA_ENABLE_ASAN_UBSAN` and `NOVA_ENABLE_TSAN` are mutually exclusive CMake options
  (runbook §3 names).

## D-009 — Dependency-free test runner (ACCEPTED, A0)

Tests use a ~40-line header (`tests/test_util.h`) and CTest, not GoogleTest/Catch2. Reasons:
the build must work from a clean extraction with no network access (submission checklist), and
the assignment discourages external libraries in the store. Test-only scheduling hooks for the
EBR interleaving tests will be added in B2.

## D-010 — Project-local CMake (ACCEPTED, A0)

The development machine has no `cmake` and no passwordless `sudo`. `scripts/bootstrap_tools.sh`
installs `cmake==3.31.6` from PyPI into `.tools/venv` (git-ignored, removable by deleting the
directory). `CMakeLists.txt` requires only CMake ≥ 3.16, so a distribution cmake works too.
This is a tooling workaround, not a project dependency; the README states it.

## D-011 — Git workflow (ACCEPTED, A0)

`main` holds the import of the original materials. Milestone A0 was developed on the branch
`chore/bootstrap` (blueprint §8.2 names the checkpoint `chore/bootstrap`, tag `a0-bootstrap`).
Milestone evidence is committed in a second commit after the code commit, because a report
cannot contain the SHA of the commit that adds it; the tag points at the evidence commit.

---

## Open items carried forward

| ID | Item | Needed from | Blocks |
|---|---|---|---|
| O-1 | Exactly-one-Snapshot join across two `KVStore` instances (PDF p. 5 vs Appendix A two-table shape) | Instructor | Full PASS of C2 |
| O-2 | `perf_event_paranoid = 4`: unprivileged `perf` is unusable on this machine | User (root: `sudo sysctl kernel.perf_event_paranoid=1`, or run captures with privileges) | A2 profiling, D flame graphs |
| O-3 | FlameGraph scripts (`stackcollapse-perf.pl`, `flamegraph.pl`) not installed | User approval to fetch github.com/brendangregg/FlameGraph | A2, D |
| O-4 | Team roll numbers for the ZIP name | User | Final packaging |
| O-5 | Instructor's original `workload_gen.h`, if one exists as a file | User | Nothing (transcription is in use) |
| O-6 | Blueprint §14 review checklist (partition count, segment capacity, chunk size, thresholds, error contracts) not yet confirmed by the user | User | Should be confirmed before B1; A1 uses the §2.2 error contracts |
