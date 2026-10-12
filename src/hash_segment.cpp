#include "hash_segment.h"

#include "alloc.h"

#include <cstring>

namespace nova {

HashSegment::HashSegment(std::uint32_t initial_capacity, unsigned hash_shift,
                         HashFn hash_fn) noexcept
    : slots_(nullptr),
      capacity_(0),
      live_(0),
      tombstones_(0),
      initial_capacity_(initial_capacity),
      hash_shift_(hash_shift),
      hash_fn_(hash_fn),
      growths_(0),
      rebuilds_(0),
      bytes_built_(0) {}

HashSegment::~HashSegment() { free_bytes(slots_); }

void HashSegment::place(Entry* table, std::uint32_t capacity, long key, std::uint64_t h,
                        const ValueRef& ref) const noexcept {
    // `table` has no tombstones and no entry for `key`, and is never full.
    const std::uint32_t mask = capacity - 1;
    std::uint32_t i = static_cast<std::uint32_t>(h >> hash_shift_) & mask;
    while (table[i].state != SlotState::Empty) i = (i + 1) & mask;
    table[i].key = key;
    table[i].value = ref;
    table[i].state = SlotState::Occupied;
}

Entry* HashSegment::build_table(std::uint32_t new_capacity) const noexcept {
    const std::size_t bytes = sizeof(Entry) * static_cast<std::size_t>(new_capacity);
    Entry* table = static_cast<Entry*>(alloc_bytes(bytes, kCacheLine));
    if (table == nullptr) return nullptr;
    std::memset(table, 0, bytes);  // SlotState::Empty == 0
    bytes_built_ += bytes;
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        const Entry& e = slots_[i];
        if (e.state == SlotState::Occupied) {
            place(table, new_capacity, e.key, hash_fn_(e.key), e.value);
        }
    }
    return table;
}

void HashSegment::install(Entry* table, std::uint32_t capacity) noexcept {
    if (capacity_ != 0) {
        if (capacity > capacity_) ++growths_;
        else ++rebuilds_;
    }
    free_bytes(slots_);
    slots_ = table;
    capacity_ = capacity;
    tombstones_ = 0;
}

bool HashSegment::clone_from(const Entry* slots, std::uint32_t capacity, std::uint32_t live,
                             std::uint32_t tombstones) noexcept {
    if (capacity == 0) return true;
    const std::size_t bytes = sizeof(Entry) * static_cast<std::size_t>(capacity);
    Entry* table = static_cast<Entry*>(alloc_bytes(bytes, kCacheLine));
    if (table == nullptr) return false;
    std::memcpy(table, slots, bytes);
    bytes_built_ += bytes;
    slots_ = table;
    capacity_ = capacity;
    live_ = live;
    tombstones_ = tombstones;
    return true;
}

Entry* HashSegment::release_slots() noexcept {
    Entry* table = slots_;
    slots_ = nullptr;
    capacity_ = 0;
    live_ = 0;
    tombstones_ = 0;
    return table;
}

bool HashSegment::prepare_upsert(long key, std::uint64_t h, UpsertPlan& plan) const noexcept {
    plan = UpsertPlan{};

    if (capacity_ != 0) {
        const std::uint32_t mask = capacity_ - 1;
        std::uint32_t i = static_cast<std::uint32_t>(h >> hash_shift_) & mask;
        Entry* first_tombstone = nullptr;
        Entry* empty = nullptr;
        for (std::uint32_t n = 0; n < capacity_; ++n, i = (i + 1) & mask) {
            Entry& e = slots_[i];
            if (e.state == SlotState::Empty) {
                empty = &e;
                break;
            }
            if (e.state == SlotState::Occupied) {
                if (e.key == key) {
                    // The whole chain up to the first Empty slot was searched
                    // before any tombstone is reused, so a key can never be
                    // stored twice.
                    plan.target = &e;
                    plan.existing = true;
                    return true;
                }
            } else if (first_tombstone == nullptr) {
                first_tombstone = &e;
            }
        }
        if (first_tombstone != nullptr) {
            plan.target = first_tombstone;  // occupancy (live + tombstones) unchanged
            return true;
        }
        if (empty != nullptr &&
            fits(std::uint64_t{live_} + tombstones_ + 1, capacity_)) {
            plan.target = empty;
            return true;
        }
    }

    // A replacement array is needed: first allocation, growth, or tombstone
    // removal. With few tombstones, double; with many (>= 1/5 of capacity),
    // dropping them at the same capacity frees enough room.
    std::uint32_t new_capacity = capacity_ == 0 ? initial_capacity_ : capacity_;
    if (capacity_ != 0 && std::uint64_t{tombstones_} * kTombstoneRebuildDen < capacity_) {
        if (new_capacity >= kMaxSegmentCapacity) return false;
        new_capacity *= 2;
    }
    while (!fits(std::uint64_t{live_} + 1, new_capacity)) {
        if (new_capacity >= kMaxSegmentCapacity) return false;
        new_capacity *= 2;
    }

    Entry* table = build_table(new_capacity);
    if (table == nullptr) return false;
    plan.new_slots = table;
    plan.new_capacity = new_capacity;
    return true;
}

bool HashSegment::commit_upsert(long key, std::uint64_t h, const ValueRef& ref,
                                UpsertPlan& plan, ValueRef* old_ref) noexcept {
    if (plan.new_slots != nullptr) {
        install(plan.new_slots, plan.new_capacity);
        place(slots_, capacity_, key, h, ref);
        ++live_;
        plan = UpsertPlan{};
        return false;
    }
    Entry* target = plan.target;
    const bool existing = plan.existing;
    plan = UpsertPlan{};
    if (existing) {
        *old_ref = target->value;
        target->value = ref;
        return true;
    }
    if (target->state == SlotState::Tombstone) --tombstones_;
    target->key = key;
    target->value = ref;
    target->state = SlotState::Occupied;
    ++live_;
    return false;
}

void HashSegment::abort_upsert(UpsertPlan& plan) const noexcept {
    free_bytes(plan.new_slots);
    plan = UpsertPlan{};
}

bool HashSegment::erase(long key, std::uint64_t h, ValueRef* old_ref) noexcept {
    Entry* e = const_cast<Entry*>(find(key, h));
    if (e == nullptr) return false;
    *old_ref = e->value;
    // A tombstone, not Empty: later keys of this probe chain must stay reachable.
    e->state = SlotState::Tombstone;
    --live_;
    ++tombstones_;

    if (std::uint64_t{tombstones_} * kTombstoneRebuildDen >= capacity_) {
        Entry* table = build_table(capacity_);
        if (table != nullptr) install(table, capacity_);
        // On allocation failure the erase still stands; the segment remains
        // valid and a later insert or erase retries the rebuild.
    }
    return true;
}

}  // namespace nova
