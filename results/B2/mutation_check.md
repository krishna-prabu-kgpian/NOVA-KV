# B2 mutation check (manual, 2026-10-12)

Purpose: evidence that the B2 tests fail when the reclamation protocol or the
chunk-ownership rules are wrong. Each row is one deliberate bug applied to a
scratch copy of the sources (never to the repository), followed by a rebuild
and a run of `pinned_arena_test epoch_test versioned_store_test
versioned_alloc_failure_test versioned_interleaving_test reclamation_test
reclamation_stress_test`. "Caught by" lists the tests that exited nonzero.

Debug build unless marked ASan. The unmutated baseline was built and run first
in each build: all seven tests exit 0. One mutation at a time; the file is
restored and touched afterwards. Run by hand on the tree that became the B2
code commit; not part of `scripts/verify_all.sh`; no per-run logs kept beyond
this table.

Two corrections were needed during the exercise and are reflected below:
- E4's first injection did not compile (a macro trick) and was redone by
  reading the tag into a local before the exchange.
- In the first run `pinned_arena_test` also appeared for E5 and E10, which do
  not touch the arena. Cause: an assertion in its concurrent test depended on
  the cleaner thread keeping pace on a loaded machine. The test was fixed
  (writer paced against the cleaner's progress; 60 consecutive unmutated runs
  pass) and those two entries are omitted here as not attributable to the
  mutation.

| # | Injected bug | Caught by |
|---|---|---|
| E1 | version eligible after ONE epoch advance (tag + 1) | reclamation, reclamation_stress |
| E2 | epoch advance ignores announced epochs (unconditional increment) | epoch, reclamation, reclamation_stress |
| E3 | collector scans only the first slot block | epoch, reclamation |
| E4 | retirement tag read BEFORE the root swap | reclamation (schedule "reader arrives between epoch advance and publication") |
| E5 | reader loads the root BEFORE announcing its epoch | reclamation (debug and ASan) |
| E6 | reader never clears its slot | reclamation |
| E7 | commit reference dropped when the chunk is sealed (audit finding F2 reintroduced) | pinned_arena, versioned_alloc_failure, versioned_interleaving, reclamation |
| E8 | `end_commit` keeps the commit references (leak) | pinned_arena, versioned_store, versioned_alloc_failure, versioned_interleaving, reclamation, reclamation_stress |
| E9 | rollback also drops the pre-commit writable chunk | pinned_arena, versioned_alloc_failure, reclamation |
| E10 | batch limit not applied | reclamation, reclamation_stress |
| E11 | version counted as reclaimed when dequeued | versioned_store, versioned_alloc_failure, versioned_interleaving, reclamation, reclamation_stress |
| E12 | work budget ignored inside a version | reclamation |
| E13 | segment released regardless of other owners | versioned_store, versioned_alloc_failure, versioned_interleaving, reclamation, reclamation_stress |
| E14 | chunk destroyed with one reference still outstanding (ID reused while referenced) | pinned_arena, versioned_store, versioned_alloc_failure, versioned_interleaving, reclamation, reclamation_stress |
| E15 | writer does not queue the replaced version (lost retirement) | versioned_store, versioned_alloc_failure, versioned_interleaving, reclamation, reclamation_stress |
| E16 | a pin's slot is cleared while the pin is in use | reclamation, reclamation_stress |
| E17 (ASan) | thread exit returns its slot without checking the store is alive | epoch, versioned_store, versioned_interleaving, reclamation |

17 of 17 mutations detected. Four are detected by a single test program
(E4, E5, E6, E12), all by the deterministic schedules in `reclamation_test`.

A mutation that was considered and NOT counted: removing the reader's
validation loop `[P3]`. In this design that makes a reader announce a possibly
stale (older) epoch, which only holds reclamation back further; it was not
shown to be unsafe here, so its detection (by the retry-count assertion) would
not demonstrate anything about safety. The loop is kept because the protocol
and its proof are stated with it.
