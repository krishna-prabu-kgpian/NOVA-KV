// cow_segment.h -- immutable published index segments and their private builder
// (Part B).
//
// A Segment is one partition of the primary index as seen by readers: an
// open-addressed, linearly probed slot array that never changes after it has
// been published. A write that touches a partition does not modify its
// Segment; it builds a replacement with SegmentBuilder (a private copy that is
// edited in place where no reader can see it) and the replacement becomes
// visible only as part of a newly published Version.
//
// Ownership recorded in a Segment (blueprint 3.3, 3.4):
//   owners  number of Versions whose directory points at this segment. Several
//           versions share an unchanged segment.
//   pins    the distinct chunk IDs referenced by the segment's occupied slots.
//           The segment holds one arena pin per listed chunk for as long as it
//           exists, which is what keeps value bytes alive for every version
//           that can still reach them.
#pragma once

#include "config.h"
#include "hash.h"
#include "hash_segment.h"
#include "value_ref.h"

#include <atomic>
#include <cstdint>

namespace nova {

// One distinct chunk referenced by a segment. `refs` (how many of the segment's
// entries point into the chunk) lets a builder keep the set exact when entries
// are replaced or erased; the arena is pinned once per ChunkPin, not per entry.
struct ChunkPin {
    std::uint32_t chunk_id;
    std::uint32_t refs;
};

struct Segment {
    std::atomic<std::uint32_t> owners;  // versions referencing this segment (writer/cleaner side)
    std::uint32_t capacity;             // power of two, > 0
    std::uint32_t live;
    std::uint32_t tombstones;
    std::uint32_t pin_count;
    Entry* slots;                       // immutable once published
    ChunkPin* pins;                     // sorted by chunk_id; stored after this header
};

// Lookup in a published (or any) segment. Same rule as Part A: stop at Empty,
// skip Tombstone, match on the full key. `seg` may be null (empty partition).
inline const Entry* segment_find(const Segment* seg, long key, std::uint64_t h) noexcept {
    if (seg == nullptr) return nullptr;
    const std::uint32_t mask = seg->capacity - 1;
    std::uint32_t i = static_cast<std::uint32_t>(h >> kPartitionBits) & mask;
    for (std::uint32_t n = 0; n < seg->capacity; ++n, i = (i + 1) & mask) {
        const Entry& e = seg->slots[i];
        if (e.state == SlotState::Empty) return nullptr;
        if (e.state == SlotState::Occupied && e.key == key) return &e;
    }
    return nullptr;
}

// Frees a segment's memory. Its arena pins must already have been dropped (or
// never taken). Null is allowed.
void destroy_segment(Segment* seg) noexcept;

// Builds the replacement for one partition, privately.
//
//   begin()                      private copy of the published segment
//   prepare_put / commit_put     insert or overwrite (two phases so the value
//                                can be appended to the arena in between)
//   erase                        remove
//   prepare_finish()             last fallible step
//   finish()                     hands over the immutable Segment; cannot fail
//
// Everything a builder allocates is released by its destructor unless finish()
// handed it over, so abandoning a commit at any point is just destroying the
// builders. A builder takes no arena pin and touches no shared counter: all
// ownership is acquired by the caller after the last fallible step.
class SegmentBuilder {
public:
    SegmentBuilder() noexcept;
    ~SegmentBuilder();
    SegmentBuilder(const SegmentBuilder&) = delete;
    SegmentBuilder& operator=(const SegmentBuilder&) = delete;

    // `base` may be null. `max_ops` bounds the puts that will follow (each can
    // introduce at most one new chunk). False on allocation failure.
    bool begin(const Segment* base, std::uint32_t max_ops) noexcept;

    bool prepare_put(long key, std::uint64_t h) noexcept;  // false: allocation failure
    void commit_put(long key, std::uint64_t h, const ValueRef& ref) noexcept;
    void abort_put() noexcept;

    // True if the key was present and is now removed. Needs no memory.
    bool erase(long key, std::uint64_t h) noexcept;

    bool changed() const noexcept { return changed_; }
    // The entry of `key` in the private copy as it stands now, or null. Valid
    // only until the next put or erase on this builder.
    const Entry* find(long key, std::uint64_t h) const noexcept { return work_.find(key, h); }

    // Allocates the Segment header for the final contents. False on failure.
    bool prepare_finish() noexcept;
    // Returns the finished segment (owners == 0, no pins taken yet), or null if
    // the partition ended up with no live key. Requires prepare_finish().
    Segment* finish() noexcept;

    // Slot bytes copied or rebuilt on behalf of this builder so far.
    std::uint64_t bytes_built() const noexcept { return work_.bytes_built(); }
    std::uint64_t growths() const noexcept { return work_.growths(); }
    std::uint64_t rebuilds() const noexcept { return work_.rebuilds(); }

private:
    void pin_add(const ValueRef& ref) noexcept;
    void pin_sub(const ValueRef& ref) noexcept;

    HashSegment work_;          // the private, editable copy
    HashSegment::UpsertPlan plan_;
    ChunkPin* pins_;            // sorted by chunk_id; entries may drop to refs == 0
    std::uint32_t pin_count_;
    std::uint32_t pin_capacity_;
    Segment* header_;           // allocated by prepare_finish()
    bool changed_;
};

}  // namespace nova
