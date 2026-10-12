# A1 mutation check (manual, 2026-10-12)

Purpose: evidence that the A1 tests fail when the store is wrong, not only that
they pass when it is right. Each row is one deliberate bug applied to a scratch
copy of the sources (never to the repository), followed by a rebuild and a run
of the listed tests. "Caught by" lists the tests that exited nonzero.

Method: sources copied to a scratch directory; unmutated baseline built and run
first (all tests exit 0 in the debug and TSan builds); one mutation at a time,
file restored and touched afterwards. Debug build for M1-M7 and M10-M14 with
tests `hash_segment_test arena_test alloc_failure_test kvstore_test_single`;
TSan build for M8-M9 with `concurrent_lock_test_rwlock
concurrent_lock_test_global_lock`. This was run by hand on the tree that became
commit 565a885; it is not part of `scripts/verify_all.sh` and no per-run logs
were kept beyond this table.

A first attempt at this check was invalid (restored files kept their old
timestamps, so mutations accumulated between runs) and was discarded; the table
is from the corrected rerun.

| # | Injected bug | Caught by |
|---|---|---|
| M1 | `erase` writes Empty instead of Tombstone | hash_segment_test, alloc_failure_test, kvstore_test_single |
| M2 | `get` copies a truncated value into a too-small buffer | alloc_failure_test, kvstore_test_single |
| M3 | upsert stops at the first tombstone without scanning the rest of the chain (duplicate key) | hash_segment_test, kvstore_test_single |
| M4 | chunk freed on the first release instead of the last | alloc_failure_test, kvstore_test_single (and arena_test after `test_shared_chunk_survives_partial_release` was added because of this result) |
| M5 | load bound ignored (segment grows only when full) | hash_segment_test, alloc_failure_test |
| M6 | zero-length value reported as missing | alloc_failure_test, kvstore_test_single |
| M7 | failed arena append commits an empty value instead of failing | alloc_failure_test |
| M8 | `put` without the lock (TSan build) | concurrent_lock_test_rwlock, concurrent_lock_test_global_lock |
| M9 | `get` without the lock (TSan build) | concurrent_lock_test_rwlock, concurrent_lock_test_global_lock |
| M10 | oversized value forced through the normal chunk path | arena_test, alloc_failure_test, kvstore_test_single |
| M11 | old value not released on overwrite | alloc_failure_test, kvstore_test_single |
| M12 | `nullptr` with length > 0 accepted | kvstore_test_single |
| M13 | tombstone rebuild never triggered | hash_segment_test, alloc_failure_test, kvstore_test_single |
| M14 | chunk directory slot not cleared before the ID is reused | arena_test, alloc_failure_test, kvstore_test_single |

14 of 14 mutations were detected. Observation: M5 was not detected through the
public API (`kvstore_test`), only by the unit test and the injection test; a
weaker load policy does not change observable results, only probe lengths.
