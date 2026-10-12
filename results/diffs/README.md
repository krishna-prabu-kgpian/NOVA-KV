# Part A -> Part B source diff

Generated with:

    git diff part-a-final HEAD -- src include > part_a_to_part_b.diff

on branch `part-b/rcu` at the commit that carries the post-B2 audit corrections (decisions
D-047 and D-048). `part-a-final` = 3444f23.

History of this file: it was first generated at `b699af9` and was identical to
`git diff part-a-final part-b-final -- src include` (19 files, 2541 insertions, 26
deletions). The audit then changed `include/kvstore.h` (the private reclaim-batch member) and
`src/epoch.*`, `src/versioned_store.*`, `src/kvstore*.{h,cpp}`, `src/config.h`, so the file was
regenerated. The tag `part-b-final` was not moved and still names the pre-audit commit
`304a8ac`. `scripts/verify_all.sh` (gate `part_b_artifacts`) fails if this file differs from
`git diff part-a-final HEAD -- src include`.

Scope: the store's sources (`src/`, `include/`). Tests, drivers, benchmarks, scripts and
documents changed as well; see `git diff --stat part-a-final HEAD`.

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
