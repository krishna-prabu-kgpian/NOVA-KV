# C2 mutation check: can the C2 tests fail?

Run by hand on 2026-10-13 against commit `7889718` (debug build, `build/verify/debug`), not part
of `scripts/verify_all.sh`. Each row is one deliberate bug injected into the source, the
listed test programs rebuilt and run, and the source restored (`git status` clean afterwards).
"Detected" means at least one listed program exited non-zero. A negative exit status is a
signal (-11 segmentation fault, -6 abort).

**Result: 21 of 21 detected.**

| ID | Injected bug | File | Result | Detected by |
|---|---|---|---|---|
| D1 | index_get does not compare the field bytes (trusts the hash) | `src/versioned_store.cpp` | DETECTED | index_failure_test: exit 1, 148 failed checks |
| D2 | a put does not update the index | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit -6, 220 failed checks; index_concurrency_test: exit 1, 61 failed checks |
| D3 | an erase leaves its index entry behind | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 114 failed checks |
| D4 | an overwrite is assumed never to change the indexed bytes | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit -6, 46 failed checks; join_test: exit 1, 9 failed checks |
| D5 | snapshot-scoped index_get uses the CURRENT version | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 3458 failed checks; index_concurrency_test: exit 1, 95 failed checks; join_test: exit 1, 7 failed checks |
| D6 | create_index publishes an index without backfilling it | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 463 failed checks |
| D7 | values too short for the field are indexed (bytes read beyond the value) | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 138 failed checks |
| D8 | the cleaner does not release a version's index segments | `src/versioned_store.cpp` | DETECTED | index_failure_test: exit 1, 504 failed checks; index_test_rcu: exit 1, 9 failed checks |
| D9 | a shared index segment gets no reference from the new version | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit -11, 0 failed checks |
| D10 | index_get ignores max_keys | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit -11, 4 failed checks |
| D11 | merge: the FIRST delta for a pair decides instead of the last | `src/secondary_index.cpp` | DETECTED | index_test_rcu: exit 1, 245 failed checks; index_failure_test: exit 1, 255 failed checks |
| D12 | a failed commit does not free the index segments it built | `src/versioned_store.cpp` | DETECTED | index_failure_test: exit 1, 7 failed checks |
| D13 | index_get accepts a snapshot of another store | `src/kvstore.cpp` | DETECTED | index_test_rcu: exit 1, 2 failed checks |
| D15 | create_index accepts a second, different range | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 2 failed checks |
| D16 | the index is one commit behind: replaced index segments are not installed | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit -6, 221 failed checks; index_concurrency_test: exit 1, 65 failed checks |
| D17 | join probes the inner index in the CURRENT version, not the inner snapshot | `benchmarks/join.h` | DETECTED | join_test: exit 1, 245 failed checks |
| D18 | join enumerates the outer table with a NEW snapshot instead of the join's | `benchmarks/join.h` | DETECTED | join_test: exit 1, 300 failed checks |
| D19 | join emits only the first match of each probe | `benchmarks/join.h` | DETECTED | join_test: exit 1, 9 failed checks |
| D20 | join does not grow the match buffer (silently truncates) | `benchmarks/join.h` | DETECTED | join_test: exit 1, 5 failed checks |
| D21 | create_index builds some segments and publishes before the rest (index visible while incomplete) | `src/versioned_store.cpp` | DETECTED | index_test_rcu: exit 1, 246 failed checks; index_concurrency_test: exit 1, 16 failed checks |
| D22 | bucket table: lookups stop at the first bucket (no probing) | `src/secondary_index.cpp` | DETECTED | index_test_rcu: exit 1, 4075 failed checks |

Notes.

- D1 is detected by the forced-collision test only (with the full 64-bit hash two different
  field values practically never collide), which is why that test exists.
- D17-D20 mutate the join (`benchmarks/join.h`), not the store.
- There is no D14 (a planned mutation that turned out to be a no-op was dropped before the run).
