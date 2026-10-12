// hash_segment_test.cpp -- unit tests of one index partition (src/hash_segment.h).
//
// The segment is built with an identity hash and no partition shift, so a key's
// start slot is simply (key mod capacity). That makes collision chains,
// wraparound and tombstone placement exact and deterministic rather than a
// matter of luck with the production hash.
#include "hash_segment.h"

#include "test_util.h"

#include <cstdint>
#include <unordered_map>

using nova::Entry;
using nova::HashSegment;
using nova::SlotState;
using nova::ValueRef;

namespace {

std::uint64_t identity_hash(long key) { return static_cast<std::uint64_t>(key); }

// A distinct, recognizable reference per (key, version).
ValueRef ref_for(long key, std::uint32_t version) {
    return ValueRef{static_cast<std::uint32_t>(key) * 7u + 1u, version, 16};
}

bool same(const ValueRef& a, const ValueRef& b) {
    return a.chunk_id == b.chunk_id && a.offset == b.offset && a.length == b.length;
}

// Returns true if an existing value was replaced; fails the test if the
// operation itself fails.
bool upsert(HashSegment& seg, long key, const ValueRef& ref, ValueRef* old = nullptr) {
    HashSegment::UpsertPlan plan;
    const bool prepared = seg.prepare_upsert(key, identity_hash(key), plan);
    NOVA_CHECK(prepared);
    if (!prepared) return false;
    ValueRef dummy{};
    return seg.commit_upsert(key, identity_hash(key), ref, plan, old != nullptr ? old : &dummy);
}

const Entry* find(const HashSegment& seg, long key) {
    return seg.find(key, identity_hash(key));
}

bool erase(HashSegment& seg, long key, ValueRef* old = nullptr) {
    ValueRef dummy{};
    return seg.erase(key, identity_hash(key), old != nullptr ? old : &dummy);
}

std::uint32_t slot_index(const HashSegment& seg, const Entry* e) {
    return static_cast<std::uint32_t>(e - seg.slots());
}

void test_empty_and_lazy_allocation() {
    HashSegment seg(16, 0, &identity_hash);
    NOVA_CHECK(seg.capacity() == 0);  // nothing allocated before the first insert
    NOVA_CHECK(find(seg, 1) == nullptr);
    NOVA_CHECK(!erase(seg, 1));
    NOVA_CHECK(!upsert(seg, 1, ref_for(1, 0)));
    NOVA_CHECK(seg.capacity() == 16);
    NOVA_CHECK(seg.live() == 1 && seg.tombstones() == 0);
    NOVA_CHECK(find(seg, 1) != nullptr && same(find(seg, 1)->value, ref_for(1, 0)));
    NOVA_CHECK(seg.growths() == 0 && seg.rebuilds() == 0);
}

void test_collision_chain_and_overwrite() {
    HashSegment seg(16, 0, &identity_hash);
    // 3, 19, 35, 51 all start at slot 3 and must occupy 3, 4, 5, 6.
    const long keys[] = {3, 19, 35, 51};
    for (long k : keys) NOVA_CHECK(!upsert(seg, k, ref_for(k, 0)));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const Entry* e = find(seg, keys[i]);
        NOVA_CHECK(e != nullptr);
        if (e == nullptr) continue;
        NOVA_CHECK(slot_index(seg, e) == 3 + i);
        NOVA_CHECK(e->key == keys[i]);  // full-key comparison, not just the slot
        NOVA_CHECK(same(e->value, ref_for(keys[i], 0)));
    }
    NOVA_CHECK(find(seg, 67) == nullptr);  // same start slot, never inserted

    // Overwriting the last key of the chain replaces in place: no new slot.
    ValueRef old{};
    NOVA_CHECK(upsert(seg, 51, ref_for(51, 1), &old));
    NOVA_CHECK(same(old, ref_for(51, 0)));
    NOVA_CHECK(seg.live() == 4);
    NOVA_CHECK(slot_index(seg, find(seg, 51)) == 6);
    NOVA_CHECK(same(find(seg, 51)->value, ref_for(51, 1)));
}

// Runbook section 4: "Several keys collide; erase middle key; find later key".
void test_erase_middle_keeps_chain() {
    HashSegment seg(16, 0, &identity_hash);
    for (long k : {3L, 19L, 35L, 51L}) upsert(seg, k, ref_for(k, 0));
    ValueRef old{};
    NOVA_CHECK(erase(seg, 19, &old));
    NOVA_CHECK(same(old, ref_for(19, 0)));
    NOVA_CHECK(seg.slots()[4].state == SlotState::Tombstone);
    NOVA_CHECK(seg.live() == 3 && seg.tombstones() == 1);
    NOVA_CHECK(find(seg, 19) == nullptr);
    NOVA_CHECK(!erase(seg, 19));           // already gone
    NOVA_CHECK(find(seg, 3) != nullptr);
    NOVA_CHECK(find(seg, 35) != nullptr);  // beyond the tombstone
    NOVA_CHECK(find(seg, 51) != nullptr);
}

// The key exists further down the chain than a tombstone. The upsert must
// overwrite it there, not create a second copy in the tombstone.
void test_no_duplicate_past_tombstone() {
    HashSegment seg(16, 0, &identity_hash);
    for (long k : {3L, 19L, 35L}) upsert(seg, k, ref_for(k, 0));
    erase(seg, 3);  // tombstone at slot 3, in front of 19 and 35
    ValueRef old{};
    NOVA_CHECK(upsert(seg, 35, ref_for(35, 1), &old));  // replaced, not inserted
    NOVA_CHECK(same(old, ref_for(35, 0)));
    NOVA_CHECK(seg.live() == 2 && seg.tombstones() == 1);
    NOVA_CHECK(slot_index(seg, find(seg, 35)) == 5);
    std::uint32_t copies = 0;
    for (std::uint32_t i = 0; i < seg.capacity(); ++i) {
        const Entry& e = seg.slots()[i];
        if (e.state == SlotState::Occupied && e.key == 35) ++copies;
    }
    NOVA_CHECK(copies == 1);

    // A genuinely new key of the same chain reuses the first tombstone.
    NOVA_CHECK(!upsert(seg, 51, ref_for(51, 0)));
    NOVA_CHECK(slot_index(seg, find(seg, 51)) == 3);
    NOVA_CHECK(seg.live() == 3 && seg.tombstones() == 0);

    // Re-inserting an erased key works and is found again.
    erase(seg, 19);
    NOVA_CHECK(find(seg, 19) == nullptr);
    NOVA_CHECK(!upsert(seg, 19, ref_for(19, 2)));
    NOVA_CHECK(same(find(seg, 19)->value, ref_for(19, 2)));
}

void test_probe_wraparound() {
    HashSegment seg(16, 0, &identity_hash);
    // All start at the last slot (15) and must wrap to 0 and 1.
    for (long k : {15L, 31L, 47L}) upsert(seg, k, ref_for(k, 0));
    NOVA_CHECK(slot_index(seg, find(seg, 15)) == 15);
    NOVA_CHECK(slot_index(seg, find(seg, 31)) == 0);
    NOVA_CHECK(slot_index(seg, find(seg, 47)) == 1);
    erase(seg, 31);  // tombstone at slot 0, in the wrapped part of the chain
    NOVA_CHECK(find(seg, 47) != nullptr);
    NOVA_CHECK(find(seg, 63) == nullptr);
}

void test_growth_preserves_entries() {
    HashSegment seg(16, 0, &identity_hash);
    // 0.70 * 16 = 11.2: eleven entries fit, the twelfth forces a doubling.
    for (long k = 0; k < 11; ++k) upsert(seg, k * 16 + 5, ref_for(k * 16 + 5, 0));  // one long chain
    NOVA_CHECK(seg.capacity() == 16 && seg.growths() == 0);
    upsert(seg, 11 * 16 + 5, ref_for(11 * 16 + 5, 0));
    NOVA_CHECK(seg.capacity() == 32);
    NOVA_CHECK(seg.growths() == 1 && seg.rebuilds() == 0);
    NOVA_CHECK(seg.live() == 12 && seg.tombstones() == 0);
    for (long k = 0; k < 12; ++k) {
        const Entry* e = find(seg, k * 16 + 5);
        NOVA_CHECK(e != nullptr && same(e->value, ref_for(k * 16 + 5, 0)));
    }
    // Keep growing through several doublings.
    for (long k = 100; k < 1100; ++k) upsert(seg, k * 3, ref_for(k * 3, 1));
    NOVA_CHECK(seg.live() == 1012);
    NOVA_CHECK(seg.capacity() == 2048);  // smallest power of two with 1012 <= 0.7 * capacity
    for (long k = 100; k < 1100; ++k) NOVA_CHECK(find(seg, k * 3) != nullptr);
    for (long k = 0; k < 12; ++k) NOVA_CHECK(find(seg, k * 16 + 5) != nullptr);
}

void test_tombstone_rebuild_on_erase() {
    HashSegment seg(64, 0, &identity_hash);
    for (long k = 0; k < 40; ++k) upsert(seg, k, ref_for(k, 0));
    NOVA_CHECK(seg.capacity() == 64);
    // 20% of 64 = 12.8, so the 13th tombstone triggers a same-capacity rebuild.
    for (long k = 0; k < 12; ++k) NOVA_CHECK(erase(seg, k));
    NOVA_CHECK(seg.tombstones() == 12 && seg.rebuilds() == 0);
    NOVA_CHECK(erase(seg, 12));
    NOVA_CHECK(seg.rebuilds() == 1 && seg.growths() == 0);
    NOVA_CHECK(seg.capacity() == 64);
    NOVA_CHECK(seg.tombstones() == 0 && seg.live() == 27);
    for (std::uint32_t i = 0; i < seg.capacity(); ++i) {
        NOVA_CHECK(seg.slots()[i].state != SlotState::Tombstone);
    }
    for (long k = 0; k < 13; ++k) NOVA_CHECK(find(seg, k) == nullptr);
    for (long k = 13; k < 40; ++k) {
        NOVA_CHECK(find(seg, k) != nullptr && same(find(seg, k)->value, ref_for(k, 0)));
    }
}

// An insert that would exceed the load bound while few tombstones exist doubles
// the segment and drops the tombstones on the way.
void test_insert_overflow_with_tombstones_grows() {
    HashSegment seg(16, 0, &identity_hash);
    for (long k = 0; k < 11; ++k) upsert(seg, k, ref_for(k, 0));  // slots 0..10
    erase(seg, 0);                                                // 1 tombstone, live 10
    NOVA_CHECK(seg.tombstones() == 1 && seg.capacity() == 16);
    // Key 12 starts at an Empty slot (no tombstone on its path); occupancy
    // would become 12 > 11.2, so the segment is replaced.
    NOVA_CHECK(!upsert(seg, 12, ref_for(12, 0)));
    NOVA_CHECK(seg.capacity() == 32 && seg.growths() == 1);
    NOVA_CHECK(seg.tombstones() == 0 && seg.live() == 11);
    NOVA_CHECK(find(seg, 0) == nullptr);
    for (long k = 1; k < 11; ++k) NOVA_CHECK(find(seg, k) != nullptr);
    NOVA_CHECK(find(seg, 12) != nullptr);
}

// Long random run against std::unordered_map (test-only oracle), with a key
// space small enough to force constant collisions, tombstones and rebuilds.
void test_random_against_oracle() {
    HashSegment seg(8, 0, &identity_hash);
    std::unordered_map<long, ValueRef> oracle;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    int mismatches = 0;
    for (std::uint32_t op = 0; op < 200000; ++op) {
        const long key = static_cast<long>(next() % 600);
        const std::uint64_t r = next() % 100;
        if (r < 50) {
            const ValueRef ref = ref_for(key, op);
            ValueRef old{};
            const bool replaced = upsert(seg, key, ref, &old);
            auto it = oracle.find(key);
            if (replaced != (it != oracle.end())) ++mismatches;
            if (replaced && it != oracle.end() && !same(old, it->second)) ++mismatches;
            oracle[key] = ref;
        } else if (r < 85) {
            ValueRef old{};
            const bool erased = erase(seg, key, &old);
            auto it = oracle.find(key);
            if (erased != (it != oracle.end())) ++mismatches;
            if (erased && it != oracle.end() && !same(old, it->second)) ++mismatches;
            oracle.erase(key);
        } else {
            const Entry* e = find(seg, key);
            auto it = oracle.find(key);
            if ((e != nullptr) != (it != oracle.end())) ++mismatches;
            if (e != nullptr && it != oracle.end() && !same(e->value, it->second)) ++mismatches;
        }
        if (seg.live() != oracle.size()) ++mismatches;
        // Policy bounds hold after every operation.
        const std::uint64_t cap = seg.capacity();
        if (cap != 0) {
            if ((std::uint64_t{seg.live()} + seg.tombstones()) * 10 > cap * 7) ++mismatches;
            if (std::uint64_t{seg.tombstones()} * 5 >= cap) ++mismatches;
        }
    }
    NOVA_CHECK(mismatches == 0);
    for (const auto& kv : oracle) {
        const Entry* e = find(seg, kv.first);
        NOVA_CHECK(e != nullptr && same(e->value, kv.second));
    }
    std::uint32_t occupied = 0;
    for (std::uint32_t i = 0; i < seg.capacity(); ++i) {
        if (seg.slots()[i].state == SlotState::Occupied) ++occupied;
    }
    NOVA_CHECK(occupied == oracle.size());
    NOVA_CHECK(seg.rebuilds() > 0 && seg.growths() > 0);
}

}  // namespace

int main() {
    NOVA_CHECK(sizeof(Entry) == 24);
    test_empty_and_lazy_allocation();
    test_collision_chain_and_overwrite();
    test_erase_middle_keeps_chain();
    test_no_duplicate_past_tombstone();
    test_probe_wraparound();
    test_growth_preserves_entries();
    test_tombstone_rebuild_on_erase();
    test_insert_overflow_with_tombstones_grows();
    test_random_against_oracle();
    return nova_test::finish("hash_segment_test");
}
