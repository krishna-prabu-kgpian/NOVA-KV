// hash_segment.h -- one partition of the primary index: a flat open-addressed
// table with linear probing.
//
// A segment owns one contiguous slot array. Lookups stop at an Empty slot and
// skip Tombstones. Growth and tombstone removal build a new array privately and
// install it only when complete; the published array is never rehashed in
// place. There is no cross-segment probing.
//
// Synchronization is the caller's job (A1: the store lock). The class itself is
// not thread-safe.
#pragma once

#include "config.h"
#include "hash.h"
#include "value_ref.h"

#include <cstdint>

namespace nova {

enum class SlotState : std::uint8_t { Empty = 0, Occupied = 1, Tombstone = 2 };

struct Entry {
    long key;
    ValueRef value;
    SlotState state;
};

class HashSegment {
public:
    using HashFn = std::uint64_t (*)(long);

    // `hash_shift` is the number of low hash bits already consumed to pick the
    // partition. `hash_fn` must be the function the caller uses to compute the
    // `h` arguments below; it is replaceable only so unit tests can construct
    // exact collision chains. `initial_capacity` must be a power of two >= 2.
    explicit HashSegment(std::uint32_t initial_capacity = kInitialSegmentCapacity,
                         unsigned hash_shift = kPartitionBits,
                         HashFn hash_fn = &hash_key) noexcept;
    ~HashSegment();
    HashSegment(const HashSegment&) = delete;
    HashSegment& operator=(const HashSegment&) = delete;

    // Returns the occupied entry for `key`, or nullptr. h == hash_fn(key).
    const Entry* find(long key, std::uint64_t h) const noexcept {
        if (capacity_ == 0) return nullptr;
        const std::uint32_t mask = capacity_ - 1;
        std::uint32_t i = static_cast<std::uint32_t>(h >> hash_shift_) & mask;
        for (std::uint32_t n = 0; n < capacity_; ++n, i = (i + 1) & mask) {
            const Entry& e = slots_[i];
            if (e.state == SlotState::Empty) return nullptr;  // end of probe chain
            if (e.state == SlotState::Occupied && e.key == key) return &e;
            // Tombstone or another key: keep probing (INV-11).
        }
        return nullptr;
    }

    // ---- Insert / overwrite, in two phases so that every fallible step
    // happens before the first mutation.
    struct UpsertPlan {
        Entry* target = nullptr;      // slot in the current array, or null
        bool existing = false;        // target holds `key` already (overwrite)
        Entry* new_slots = nullptr;   // privately built replacement array, or null
        std::uint32_t new_capacity = 0;
    };

    // Phase 1: decides where `key` will go. May allocate and fill a private
    // replacement array (growth or tombstone removal). Does not modify the
    // segment. Returns false on allocation failure or capacity limit.
    bool prepare_upsert(long key, std::uint64_t h, UpsertPlan& plan) const noexcept;

    // Phase 2: cannot fail. Installs the replacement array if there is one and
    // stores (key -> ref). Returns true if an existing value was replaced, in
    // which case *old_ref receives the previous reference.
    bool commit_upsert(long key, std::uint64_t h, const ValueRef& ref, UpsertPlan& plan,
                       ValueRef* old_ref) noexcept;

    // Discards a prepared plan without changing the segment.
    void abort_upsert(UpsertPlan& plan) const noexcept;

    // Replaces the entry with a tombstone. Returns false if `key` is absent.
    // Never fails for lack of memory: if the tombstone-removal rebuild cannot
    // allocate it is simply retried by a later operation.
    bool erase(long key, std::uint64_t h, ValueRef* old_ref) noexcept;

    // ---- Introspection (tests, statistics, validation) ----
    std::uint32_t capacity() const noexcept { return capacity_; }
    std::uint32_t live() const noexcept { return live_; }
    std::uint32_t tombstones() const noexcept { return tombstones_; }
    std::uint64_t growths() const noexcept { return growths_; }
    std::uint64_t rebuilds() const noexcept { return rebuilds_; }
    const Entry* slots() const noexcept { return slots_; }
    std::uint64_t hash(long key) const noexcept { return hash_fn_(key); }

private:
    static bool fits(std::uint64_t occupied, std::uint64_t capacity) noexcept {
        return occupied * kMaxLoadDen <= capacity * kMaxLoadNum;
    }
    // New array of `new_capacity` slots holding every live entry, no tombstones.
    Entry* build_table(std::uint32_t new_capacity) const noexcept;
    void place(Entry* table, std::uint32_t capacity, long key, std::uint64_t h,
               const ValueRef& ref) const noexcept;
    void install(Entry* table, std::uint32_t capacity) noexcept;

    Entry* slots_;
    std::uint32_t capacity_;    // 0 until the first insert, then a power of two
    std::uint32_t live_;
    std::uint32_t tombstones_;
    std::uint32_t initial_capacity_;
    unsigned hash_shift_;
    HashFn hash_fn_;
    std::uint64_t growths_;     // installs with a larger capacity
    std::uint64_t rebuilds_;    // installs at the same capacity (tombstone removal)
};

}  // namespace nova
