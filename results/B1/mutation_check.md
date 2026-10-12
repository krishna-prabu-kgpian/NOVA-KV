# B1 mutation check (manual, 2026-10-12)

Purpose: evidence that the B1 tests fail when the Part B engine is wrong. Each
row is one deliberate bug applied to a scratch copy of the sources (never to the
repository), followed by a rebuild (debug) and a run of
`pinned_arena_test cow_segment_test versioned_store_test
versioned_alloc_failure_test versioned_interleaving_test kvstore_test_rcu`.
"Caught by" lists the tests that exited nonzero (assertion failure or crash).

The unmutated baseline was built and run first: all six tests exit 0. One
mutation at a time; the file is restored and touched afterwards. Run by hand on
the tree that became commit 55240b4; not part of `scripts/verify_all.sh`; no
per-run logs kept beyond this table. B12's first attempt did not compile
(-Werror, unused variable) and was repeated with the unused names voided.

| # | Injected bug | Caught by |
|---|---|---|
| B1 | new segment does not pin its chunks | versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B2 | shared segments gain no owner reference | versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B3 | partial publication: root stored before the new segments are installed | versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B4 | a published segment is modified after it has been replaced | versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B5 | replaced version freed at once instead of retained | versioned_store, versioned_alloc_failure, versioned_interleaving |
| B6 | failed commit does not roll the arena back | versioned_alloc_failure |
| B7 | arena rollback keeps the unpublished tail | pinned_arena, versioned_alloc_failure |
| B8 | chunk freed at commit end while still pinned | pinned_arena, versioned_store, versioned_alloc_failure, kvstore_test_rcu |
| B9 | unpin frees the writable chunk | pinned_arena, kvstore_test_rcu |
| B10 | chunk freed on the first unpin instead of the last | pinned_arena, versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B11 | builder edits the published slot array (no private copy) | cow_segment, versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B12 | pin list not updated when an entry is replaced | cow_segment, versioned_store, versioned_alloc_failure, kvstore_test_rcu |
| B13 | a commit that changes nothing still publishes a version | versioned_store |
| B14 | segment destroyed while another version still owns it | versioned_store, versioned_alloc_failure, versioned_interleaving, kvstore_test_rcu |
| B15 | two root stores for one batch | versioned_store, versioned_alloc_failure, versioned_interleaving |

15 of 15 mutations detected. Three are detected by a single test only (B6, B13,
and B9 by two): the failure-injection test is the only guard for arena rollback
on the commit path, and `versioned_store_test` the only one for "no publication
without change".

A check added because of this exercise: B7 was initially detectable only at
arena level; `versioned_alloc_failure_test` now also compares the writable
chunk and its write offset before and after every failed commit.
