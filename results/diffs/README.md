# Part A -> Part B source diff

Generated with:

    git diff part-a-final b699af9 -- src include > part_a_to_part_b.diff

`part-a-final` = 3444f23; `b699af9` is the B2 implementation commit. Tag `part-b-final`
is placed on a later commit that changes no file under `src/` or `include/`, so
`git diff part-a-final part-b-final -- src include` reproduces this file exactly.

Scope: the store's sources (`src/`, `include/`). Tests, drivers, benchmarks, scripts and
documents changed as well; see `git diff --stat part-a-final part-b-final`.

`include/kvstore.h` does not appear in the diff: the public header already declared
`set_reclaim_batch` (decision D-003) and Part B needed no change to it.

```
 src/alloc.cpp           |  25 ++
 src/config.h            |   3 +
 src/cow_segment.cpp     | 130 ++++++++
 src/cow_segment.h       | 123 ++++++++
 src/epoch.cpp           | 189 ++++++++++++
 src/epoch.h             | 140 +++++++++
 src/hash_segment.cpp    |  28 +-
 src/hash_segment.h      |  11 +
 src/inspect.cpp         | 153 +++++++++-
 src/inspect.h           |  85 +++++-
 src/kvstore.cpp         |  78 ++++-
 src/kvstore_impl.h      |  16 +
 src/pinned_arena.cpp    | 283 ++++++++++++++++++
 src/pinned_arena.h      | 178 +++++++++++
 src/sync_mode.h         |  13 +-
 src/sync_policy.h       |   5 +
 src/test_points.h       |  53 ++++
 src/versioned_store.cpp | 766 ++++++++++++++++++++++++++++++++++++++++++++++++
 src/versioned_store.h   | 288 ++++++++++++++++++
 19 files changed, 2541 insertions(+), 26 deletions(-)
```
