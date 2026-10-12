# Source diffs between parts

Scope of both: the store's sources (`src/`, `include/`). Tests, drivers, benchmarks, scripts
and documents changed as well; see `git diff --stat <tag> <tag>`.
`scripts/verify_all.sh` fails if either file is stale.

## Part A -> Part B: `part_a_to_part_b.diff`

    git diff part-a-final part-b-audited -- src include

`part-a-final` = 3444f23. `part-b-audited` = 87d38aa is the Part B baseline (decision D-050):
B2 plus the two corrections of the post-B2 audit (the private reclaim-batch member in
`kvstore.h`; reader-slot teardown). The earlier tag `part-b-final` (304a8ac) predates those
corrections and is kept unchanged; the diff against it was 19 files, 2541 insertions, 26
deletions.

```
 include/kvstore.h       |   7 +
 src/alloc.cpp           |  25 ++
 src/config.h            |   6 +
 src/cow_segment.cpp     | 130 ++++++++
 src/cow_segment.h       | 123 ++++++++
 src/epoch.cpp           | 245 ++++++++++++++++
 src/epoch.h             | 151 ++++++++++
 src/hash_segment.cpp    |  28 +-
 src/hash_segment.h      |  11 +
 src/inspect.cpp         | 153 +++++++++-
 src/inspect.h           |  85 +++++-
 src/kvstore.cpp         |  83 +++++-
 src/kvstore_impl.h      |  19 ++
 src/pinned_arena.cpp    | 283 ++++++++++++++++++
 src/pinned_arena.h      | 178 +++++++++++
 src/sync_mode.h         |  13 +-
 src/sync_policy.h       |   5 +
 src/test_points.h       |  53 ++++
 src/versioned_store.cpp | 767 ++++++++++++++++++++++++++++++++++++++++++++++++
 src/versioned_store.h   | 293 ++++++++++++++++++
 20 files changed, 2631 insertions(+), 27 deletions(-)
```

## Part B -> Part C, state at milestone C1: `part_b_to_part_c1.diff`

    git diff part-b-audited part-c-transactions -- src include

Interim: Part C.1 only (transactions, snapshots, `keys()`, the pin-aware stop-the-world
comparator, writer serialization). The diff the assignment asks for, Part B against the
complete Part C, is exported at the end of milestone C2.

```
 src/alloc.cpp           |  25 ++--
 src/alloc.h             |   4 +-
 src/epoch.h             |   4 +
 src/kvstore.cpp         | 299 +++++++++++++++++++++++++++++++++++++++---------
 src/kvstore_impl.h      |   6 +
 src/staged_writes.cpp   |  72 ++++++++++++
 src/staged_writes.h     |  52 +++++++++
 src/versioned_store.cpp |  72 ++++++++++--
 src/versioned_store.h   |  23 +++-
 9 files changed, 484 insertions(+), 73 deletions(-)
```
