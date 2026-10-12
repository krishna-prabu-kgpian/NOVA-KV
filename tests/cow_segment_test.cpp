// cow_segment_test.cpp -- unit tests of immutable segments and their private
// builder (src/cow_segment.h).
#include "cow_segment.h"

#include "test_points.h"
#include "test_util.h"

#include <cstring>
#include <vector>

using nova::ChunkPin;
using nova::Entry;
using nova::Segment;
using nova::SegmentBuilder;
using nova::SlotState;
using nova::ValueRef;

namespace {

ValueRef ref(std::uint32_t chunk, std::uint32_t offset) { return ValueRef{chunk, offset, 8}; }
std::uint64_t h(long key) { return nova::hash_key(key); }

bool put(SegmentBuilder& b, long key, const ValueRef& r) {
    if (!b.prepare_put(key, h(key))) return false;
    b.commit_put(key, h(key), r);
    return true;
}

Segment* finish(SegmentBuilder& b) {
    NOVA_CHECK(b.prepare_finish());
    return b.finish();
}

// Keys that all fall into partition 0, so they can share one segment.
std::vector<long> same_partition_keys(std::size_t n) {
    std::vector<long> keys;
    for (long k = 0; keys.size() < n; ++k) {
        if (nova::partition_of(h(k)) == 0) keys.push_back(k);
    }
    return keys;
}

bool has(const Segment* s, long key, const ValueRef& want) {
    const Entry* e = nova::segment_find(s, key, h(key));
    return e != nullptr && e->value.chunk_id == want.chunk_id && e->value.offset == want.offset &&
           e->value.length == want.length;
}

bool pin_is(const Segment* s, std::uint32_t index, std::uint32_t chunk, std::uint32_t refs) {
    return index < s->pin_count && s->pins[index].chunk_id == chunk && s->pins[index].refs == refs;
}

void test_build_from_nothing() {
    const auto keys = same_partition_keys(5);
    SegmentBuilder b;
    NOVA_CHECK(b.begin(nullptr, 5));
    NOVA_CHECK(!b.changed());
    NOVA_CHECK(put(b, keys[0], ref(7, 0)));
    NOVA_CHECK(put(b, keys[1], ref(7, 8)));
    NOVA_CHECK(put(b, keys[2], ref(3, 0)));
    NOVA_CHECK(put(b, keys[3], ValueRef{nova::kNoChunk, 0, 0}));  // empty value: no chunk
    NOVA_CHECK(b.changed());
    Segment* s = finish(b);
    NOVA_CHECK(s != nullptr);
    NOVA_CHECK(s->capacity == nova::kCowInitialSegmentCapacity && s->live == 4 && s->tombstones == 0);
    NOVA_CHECK(s->owners.load() == 0);  // ownership is the caller's to take
    // Distinct chunks, sorted, with per-chunk entry counts; the empty value pins nothing.
    NOVA_CHECK(s->pin_count == 2 && pin_is(s, 0, 3, 1) && pin_is(s, 1, 7, 2));
    NOVA_CHECK(has(s, keys[0], ref(7, 0)) && has(s, keys[2], ref(3, 0)));
    NOVA_CHECK(nova::segment_find(s, keys[4], h(keys[4])) == nullptr);
    NOVA_CHECK(nova::segment_find(nullptr, keys[0], h(keys[0])) == nullptr);  // empty partition
    nova::destroy_segment(s);
}

// The published segment a builder starts from is never touched.
void test_base_is_untouched() {
    const auto keys = same_partition_keys(40);
    SegmentBuilder b0;
    NOVA_CHECK(b0.begin(nullptr, 8));
    for (int i = 0; i < 8; ++i) NOVA_CHECK(put(b0, keys[static_cast<std::size_t>(i)], ref(1, static_cast<std::uint32_t>(i) * 8)));
    Segment* base = finish(b0);
    const std::size_t slot_bytes = sizeof(Entry) * base->capacity;
    std::vector<unsigned char> slots_before(slot_bytes);
    std::memcpy(slots_before.data(), base->slots, slot_bytes);
    const Entry* const slots_ptr = base->slots;
    const std::uint32_t cap = base->capacity, live = base->live, pins = base->pin_count;

    // Overwrite, erase, insert enough to force growth: all on the private copy.
    SegmentBuilder b1;
    NOVA_CHECK(b1.begin(base, 40));
    NOVA_CHECK(put(b1, keys[0], ref(2, 0)));
    NOVA_CHECK(b1.erase(keys[1], h(keys[1])));
    NOVA_CHECK(!b1.erase(keys[39], h(keys[39])));  // absent
    for (int i = 8; i < 38; ++i) NOVA_CHECK(put(b1, keys[static_cast<std::size_t>(i)], ref(2, static_cast<std::uint32_t>(i) * 8)));
    Segment* next = finish(b1);

    NOVA_CHECK(base->slots == slots_ptr && base->capacity == cap && base->live == live && base->pin_count == pins);
    NOVA_CHECK(std::memcmp(base->slots, slots_before.data(), slot_bytes) == 0);  // byte for byte
    NOVA_CHECK(has(base, keys[0], ref(1, 0)) && has(base, keys[1], ref(1, 8)));
    NOVA_CHECK(pin_is(base, 0, 1, 8));

    NOVA_CHECK(next != base && next->slots != base->slots);
    NOVA_CHECK(next->live == 37 && next->capacity > cap);  // grew independently of the base
    NOVA_CHECK(has(next, keys[0], ref(2, 0)));
    NOVA_CHECK(nova::segment_find(next, keys[1], h(keys[1])) == nullptr);
    for (int i = 2; i < 8; ++i) NOVA_CHECK(has(next, keys[static_cast<std::size_t>(i)], ref(1, static_cast<std::uint32_t>(i) * 8)));
    // chunk 1: 8 entries - overwritten - erased = 6; chunk 2: 1 + 30 = 31
    NOVA_CHECK(next->pin_count == 2 && pin_is(next, 0, 1, 6) && pin_is(next, 1, 2, 31));
    nova::destroy_segment(next);
    nova::destroy_segment(base);
}

// A chunk leaves the pin list exactly when the segment's last entry in it goes.
void test_pin_set_follows_entries() {
    const auto keys = same_partition_keys(3);
    SegmentBuilder b0;
    NOVA_CHECK(b0.begin(nullptr, 3));
    put(b0, keys[0], ref(5, 0));
    put(b0, keys[1], ref(5, 8));
    put(b0, keys[2], ref(9, 0));
    Segment* s0 = finish(b0);
    NOVA_CHECK(s0->pin_count == 2 && pin_is(s0, 0, 5, 2) && pin_is(s0, 1, 9, 1));

    SegmentBuilder b1;
    NOVA_CHECK(b1.begin(s0, 2));
    put(b1, keys[2], ref(4, 0));               // chunk 9 loses its only entry
    NOVA_CHECK(b1.erase(keys[0], h(keys[0]))); // chunk 5 keeps one entry
    Segment* s1 = finish(b1);
    NOVA_CHECK(s1->pin_count == 2 && pin_is(s1, 0, 4, 1) && pin_is(s1, 1, 5, 1));

    SegmentBuilder b2;
    NOVA_CHECK(b2.begin(s1, 2));
    put(b2, keys[1], ref(4, 8));               // chunk 5's last entry is replaced
    Segment* s2 = finish(b2);
    NOVA_CHECK(s2->pin_count == 1 && pin_is(s2, 0, 4, 2));

    // Same key written twice in one build: the last value wins, counts stay exact.
    SegmentBuilder b3;
    NOVA_CHECK(b3.begin(s2, 2));
    put(b3, keys[0], ref(6, 0));
    put(b3, keys[0], ref(8, 0));
    Segment* s3 = finish(b3);
    NOVA_CHECK(has(s3, keys[0], ref(8, 0)) && s3->live == 3);
    NOVA_CHECK(s3->pin_count == 2 && pin_is(s3, 0, 4, 2) && pin_is(s3, 1, 8, 1));

    nova::destroy_segment(s0);
    nova::destroy_segment(s1);
    nova::destroy_segment(s2);
    nova::destroy_segment(s3);
}

void test_empty_result_and_tombstones() {
    const auto keys = same_partition_keys(12);
    SegmentBuilder b0;
    NOVA_CHECK(b0.begin(nullptr, 10));
    for (int i = 0; i < 10; ++i) put(b0, keys[static_cast<std::size_t>(i)], ref(1, static_cast<std::uint32_t>(i) * 8));
    Segment* s0 = finish(b0);
    NOVA_CHECK(s0->capacity == 16 && s0->live == 10);

    // One erase leaves a tombstone in the replacement (base keeps the key).
    SegmentBuilder b1;
    NOVA_CHECK(b1.begin(s0, 0));
    NOVA_CHECK(b1.erase(keys[3], h(keys[3])));
    Segment* s1 = finish(b1);
    NOVA_CHECK(s1->live == 9 && s1->tombstones == 1);
    NOVA_CHECK(nova::segment_find(s1, keys[3], h(keys[3])) == nullptr);
    NOVA_CHECK(nova::segment_find(s0, keys[3], h(keys[3])) != nullptr);
    for (int i = 0; i < 10; ++i) {
        if (i != 3) NOVA_CHECK(has(s1, keys[static_cast<std::size_t>(i)], ref(1, static_cast<std::uint32_t>(i) * 8)));
    }

    // Enough erases to cross the 20 % threshold: rebuilt without tombstones.
    SegmentBuilder b2;
    NOVA_CHECK(b2.begin(s1, 0));
    for (int i = 0; i < 3; ++i) NOVA_CHECK(b2.erase(keys[static_cast<std::size_t>(i)], h(keys[static_cast<std::size_t>(i)])));
    Segment* s2 = finish(b2);
    NOVA_CHECK(s2->live == 6 && s2->tombstones == 0 && b2.rebuilds() == 1);

    // Erasing everything yields no segment at all.
    SegmentBuilder b3;
    NOVA_CHECK(b3.begin(s2, 0));
    for (int i = 4; i < 10; ++i) NOVA_CHECK(b3.erase(keys[static_cast<std::size_t>(i)], h(keys[static_cast<std::size_t>(i)])));
    NOVA_CHECK(b3.changed());
    NOVA_CHECK(finish(b3) == nullptr);

    nova::destroy_segment(s0);
    nova::destroy_segment(s1);
    nova::destroy_segment(s2);
}

// Abandoning a builder at any stage releases what it allocated and leaves the
// base alone (ASan/LeakSanitizer verify the release).
void test_abandon() {
    const auto keys = same_partition_keys(30);
    SegmentBuilder b0;
    NOVA_CHECK(b0.begin(nullptr, 4));
    for (int i = 0; i < 4; ++i) put(b0, keys[static_cast<std::size_t>(i)], ref(1, static_cast<std::uint32_t>(i)));
    Segment* base = finish(b0);
    {
        SegmentBuilder b;  // never begun
    }
    {
        SegmentBuilder b;
        NOVA_CHECK(b.begin(base, 30));
    }
    {
        SegmentBuilder b;
        NOVA_CHECK(b.begin(base, 30));
        for (int i = 4; i < 30; ++i) put(b, keys[static_cast<std::size_t>(i)], ref(2, static_cast<std::uint32_t>(i)));  // grows
        NOVA_CHECK(b.prepare_put(keys[0], h(keys[0])));  // prepared, never committed
    }
    {
        SegmentBuilder b;
        NOVA_CHECK(b.begin(base, 1));
        put(b, keys[5], ref(3, 0));
        NOVA_CHECK(b.prepare_finish());  // header allocated, never finished
    }
    NOVA_CHECK(base->live == 4 && has(base, keys[0], ref(1, 0)));
    nova::destroy_segment(base);
}

}  // namespace

int main() {
    test_build_from_nothing();
    test_base_is_untouched();
    test_pin_set_follows_entries();
    test_empty_result_and_tombstones();
    test_abandon();
#ifdef NOVA_KV_TEST_HOOKS
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
#endif
    return nova_test::finish("cow_segment_test");
}
