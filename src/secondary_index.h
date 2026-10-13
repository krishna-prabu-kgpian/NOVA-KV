// secondary_index.h -- the secondary index of the Part C store (rcu build).
//
// One index per store, over one fixed byte range [field_offset, field_offset +
// field_len) of each stored value. It maps the bytes found there to the primary
// keys of the values that contain them. A value too short to contain the whole
// range is not in the index.
//
// The index is PART OF A VERSION. A Version holds kIndexPartitionCount pointers
// to immutable IndexSegments next to its primary directory, and a write
// publishes its primary changes and the corresponding index changes in the one
// root swap of its commit. So every reader, and every Snapshot, sees a primary
// state and the index that describes exactly that state.
//
// SHAPE OF AN INDEX SEGMENT. One allocation, immutable once published:
//
//   header | buckets[bucket_capacity] | pairs[pair_count]
//
//   pairs    every (field hash, primary key) of this partition, sorted by
//            (hash, key). The keys of one field value are therefore contiguous
//            and ascending: a posting list.
//   buckets  an open-addressed, linearly probed table over the DISTINCT hashes;
//            each occupied bucket names the range of pairs with its hash.
//
// A segment holds only hashes and keys, no field bytes and no pointers into the
// store. A lookup finds the posting list of the probe's hash and then checks
// each candidate key against the primary data OF THE SAME VERSION (the caller
// does that), so two different field values with equal hashes share a bucket
// harmlessly: the wrong ones are filtered out by the byte comparison.
//
// OWNERSHIP is that of primary segments (docs/versioning.md section 5):
// `owners` counts the versions whose index directory points at the segment; a
// commit that does not change a partition shares its segment (owners + 1); the
// cleaner drops a version's references when it destroys the version, and the
// decrement that reaches zero frees the segment. A segment references nothing
// else, so freeing it is one deallocation.
//
// UPDATES never modify a published segment. A commit records what it adds and
// removes (IndexDelta), and for each partition it touches builds a replacement
// by merging the published pairs with those deltas.
#pragma once

#include "config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nova {

constexpr unsigned kIndexPartitionBits = 6;
constexpr std::uint32_t kIndexPartitionCount = 1u << kIndexPartitionBits;

struct IndexPair {
    std::uint64_t hash;   // hash of the field bytes
    long key;             // primary key of a value containing those bytes
};

struct IndexBucket {
    std::uint64_t hash;
    std::uint32_t first;  // index of the first pair with this hash
    std::uint32_t count;  // number of pairs with this hash; 0 = empty bucket
};

struct IndexSegment {
    std::atomic<std::uint32_t> owners;  // versions referencing this segment
    std::uint32_t bucket_capacity;      // power of two, > 0
    std::uint32_t distinct;             // occupied buckets
    std::uint32_t pair_count;
    IndexBucket* buckets;               // stored after the header
    IndexPair* pairs;                   // stored after the buckets
};

// One change to the index made by a commit. Several deltas of one commit may
// name the same (hash, key); the one with the highest `seq` decides.
struct IndexDelta {
    std::uint64_t hash;
    long key;
    std::uint64_t seq;    // order within the commit; unique per delta
    bool add;             // true: the pair is present afterwards; false: absent
};

// Hash of `len` field bytes (any bytes, including zeros).
std::uint64_t index_hash(const void* bytes, std::size_t len) noexcept;

inline std::uint32_t index_partition_of(std::uint64_t h) noexcept {
    return static_cast<std::uint32_t>(h) & (kIndexPartitionCount - 1);
}

// The pairs of `seg` whose hash is `h`: sets *count and returns the first, or
// returns nullptr (count 0) if there are none. `seg` may be null.
const IndexPair* index_find(const IndexSegment* seg, std::uint64_t h, std::uint32_t* count) noexcept;

// Builds a segment from `count` pairs sorted by (hash, key) without duplicates.
// Returns nullptr if `count` is 0 (an empty partition has no segment). On
// allocation failure returns nullptr and sets *failed. owners == 0.
IndexSegment* index_build(const IndexPair* sorted, std::uint32_t count, bool* failed) noexcept;

// Sorts `deltas` (n of them) by partition, then (hash, key, seq).
void index_sort_deltas(IndexDelta* deltas, std::size_t n) noexcept;

// The replacement for `base` (may be null) after applying `deltas`, which must
// all belong to this partition and be sorted by (hash, key, seq). Returns the
// new segment, or nullptr if the partition becomes empty. On allocation failure
// returns nullptr and sets *failed; `base` is never modified.
IndexSegment* index_merge(const IndexSegment* base, const IndexDelta* deltas, std::size_t n,
                          bool* failed) noexcept;

// Bytes of the segment's single allocation (0 for null).
std::uint64_t index_segment_bytes(const IndexSegment* seg) noexcept;

// Frees a segment. Null is allowed.
void index_destroy(IndexSegment* seg) noexcept;

#ifdef NOVA_KV_TEST_HOOKS
namespace test_hooks {
// Keeps only the low `bits` bits of every index hash (64 = normal), so that
// tests can make different field values collide. Set before creating an index.
void set_index_hash_bits(unsigned bits) noexcept;
}  // namespace test_hooks
#endif

}  // namespace nova
