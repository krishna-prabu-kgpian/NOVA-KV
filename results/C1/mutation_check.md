# C1 mutation check: can the C1 tests fail?

Run by hand on 2026-10-12 against commit `1239978` (debug build, `build/verify/debug`), not part
of `scripts/verify_all.sh`. Each row is one deliberate bug injected into the source, the
listed test programs rebuilt and run, and the source restored (`git status` clean afterwards).
"Detected" means at least one listed program exited non-zero. A negative exit status is a
signal (-11 segmentation fault, -6 abort).

**Result: 20 of 20 detected.**

| ID | Injected bug | File | Result | Detected by |
|---|---|---|---|---|
| C1 | stop-the-world sweep frees regardless of the epoch rule | `src/versioned_store.cpp` | DETECTED | reclamation_test: exit 1, 5 failed checks; snapshot_test_rcu: exit -11, 11 failed checks |
| C2 | Snapshot move constructor leaves the source armed (protection duplicated) | `src/kvstore.cpp` | DETECTED | snapshot_test_rcu: exit -11, 3 failed checks |
| C3 | Snapshot destruction does not release its pin | `src/kvstore.cpp` | DETECTED | snapshot_test_rcu: exit 1, 228 failed checks |
| C4 | move assignment does not release the pin it overwrites | `src/kvstore.cpp` | DETECTED | snapshot_test_rcu: exit 1, 5 failed checks |
| C5 | owner check removed: a handle is accepted by any store | `src/kvstore.cpp` | DETECTED | snapshot_test_rcu: exit 1, 18 failed checks |
| C6 | snapshot-scoped get reads the CURRENT version | `src/kvstore.cpp` | DETECTED | snapshot_test_rcu: exit 1, 3373 failed checks; txn_concurrency_test: exit 1, 22 failed checks |
| C7 | keys() reports tombstones as keys | `src/versioned_store.cpp` | DETECTED | snapshot_test_rcu: exit 1, 11 failed checks |
| C8 | keys() ignores the caller's capacity | `src/versioned_store.cpp` | DETECTED | snapshot_test_rcu: exit -6, 1 failed checks |
| C9 | Transaction publishes each staged write separately | `src/kvstore.cpp` | DETECTED | transaction_test_rcu: exit 1, 12 failed checks; txn_concurrency_test: exit 1, 139 failed checks |
| C10 | commit() can be repeated (finished flag not set) | `src/kvstore.cpp` | DETECTED | transaction_test_rcu: exit 1, 5 failed checks |
| C11 | staged writes are not cleared after commit and can be published again (finished not set, set kept) | `src/kvstore.cpp` | DETECTED | transaction_test_rcu: exit 1, 5 failed checks; txn_failure_test_rcu: exit 1, 1007 failed checks |
| C12 | an invalid put is skipped instead of failing the transaction | `src/kvstore.cpp` | DETECTED | transaction_test_rcu: exit 1, 7 failed checks; txn_failure_test_rcu: exit 1, 28 failed checks |
| C13 | staged value bytes are not copied | `src/staged_writes.cpp` | DETECTED | transaction_test_rcu: exit 1, 46789 failed checks; transaction_test_single: exit 1, 46776 failed checks |
| C14 | staging growth loses earlier operations (old array not copied) | `src/staged_writes.cpp` | DETECTED | transaction_test_rcu: exit 1, 46289 failed checks |
| C15 | lock baseline: a failed commit is not undone | `src/kvstore.cpp` | DETECTED | txn_failure_test_single: exit 1, 198 failed checks |
| C16 | lock baseline: replaced values released before the batch is complete | `src/kvstore.cpp` | DETECTED | txn_failure_test_single: exit 1, 70 failed checks |
| C17 | rcu: commit reports success when its operation array cannot be allocated | `src/kvstore.cpp` | DETECTED | txn_failure_test_rcu: exit 1, 10 failed checks |
| C18 | a pin's version is not recorded in its slot (handle resolves to nothing) | `src/versioned_store.cpp` | DETECTED | snapshot_test_rcu: exit -11, 0 failed checks |
| C19 | writer mutex not taken by commit | `src/versioned_store.cpp` | DETECTED | txn_concurrency_test: exit -11, 0 failed checks |
| C20 | acquire_snapshot loads the root BEFORE announcing | `src/versioned_store.cpp` | DETECTED | txn_concurrency_test: exit 1, 7 failed checks |

Notes.

- C1 is the defect the B2 audit recorded (D-049); its output against the code before the fix
  is `results/C1/stw_pin_reproducer_before_fix.txt`.
- C19 (no writer mutex) was detected in the debug build by a crash of the four-writer stress
  test; that is timing-dependent. The systematic detector for it is the TSan build.
- Not mutated: last-write-wins inside the engine's commit (B1 code, covered by
  `versioned_store_test`), and the secondary-index stubs (C2).
