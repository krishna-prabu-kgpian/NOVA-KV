#include "secondary_index.h"

#include "alloc.h"
#include "test_points.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace nova {

#ifdef NOVA_KV_TEST_HOOKS
namespace {
std::atomic<std::uint64_t> g_index_hash_mask{~std::uint64_t{0}};
}
namespace test_hooks {
void set_index_hash_bits(unsigned bits) noexcept {
    g_index_hash_mask.store(bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1));
}
}  // namespace test_hooks
#endif

std::uint64_t index_hash(const void* bytes, std::size_t len) noexcept {
    // FNV-1a over the bytes, then the same finalizer as hash_key so that both
    // the low bits (partition) and the bits above them (bucket) are mixed.
    const unsigned char* p = static_cast<const unsigned char*>(bytes);
    std::uint64_t x = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < len; ++i) {
        x ^= p[i];
        x *= 0x100000001b3ULL;
    }
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
#ifdef NOVA_KV_TEST_HOOKS
    x &= g_index_hash_mask.load(std::memory_order_relaxed);
#endif
    return x;
}

namespace {

std::uint32_t bucket_start(std::uint64_t h, std::uint32_t mask) noexcept {
    return static_cast<std::uint32_t>(h >> kIndexPartitionBits) & mask;
}

bool pair_less(const IndexPair& a, const IndexPair& b) noexcept {
    return a.hash != b.hash ? a.hash < b.hash : a.key < b.key;
}

}  // namespace

const IndexPair* index_find(const IndexSegment* seg, std::uint64_t h, std::uint32_t* count) noexcept {
    *count = 0;
    if (seg == nullptr) return nullptr;
    const std::uint32_t mask = seg->bucket_capacity - 1;
    std::uint32_t i = bucket_start(h, mask);
    for (std::uint32_t n = 0; n < seg->bucket_capacity; ++n, i = (i + 1) & mask) {
        const IndexBucket& b = seg->buckets[i];
        if (b.count == 0) return nullptr;  // end of the probe chain
        if (b.hash == h) {
            *count = b.count;
            return seg->pairs + b.first;
        }
    }
    return nullptr;
}

std::uint64_t index_segment_bytes(const IndexSegment* seg) noexcept {
    if (seg == nullptr) return 0;
    return sizeof(IndexSegment) + sizeof(IndexBucket) * std::uint64_t{seg->bucket_capacity} +
           sizeof(IndexPair) * std::uint64_t{seg->pair_count};
}

void index_destroy(IndexSegment* seg) noexcept {
    if (seg == nullptr) return;
    NOVA_COUNT_OBJECT(LiveObject::IndexSegment, -1);
    seg->~IndexSegment();
    free_bytes(seg);
}

IndexSegment* index_build(const IndexPair* sorted, std::uint32_t count, bool* failed) noexcept {
    if (count == 0) return nullptr;
    std::uint32_t distinct = 1;
    for (std::uint32_t i = 1; i < count; ++i) distinct += sorted[i].hash != sorted[i - 1].hash ? 1u : 0u;
    // Load at most kMaxLoadNum / kMaxLoadDen, as for the primary segments.
    std::uint32_t capacity = 4;
    while (std::uint64_t{distinct} * kMaxLoadDen > std::uint64_t{capacity} * kMaxLoadNum) capacity *= 2;

    const std::size_t bytes = sizeof(IndexSegment) + sizeof(IndexBucket) * std::size_t{capacity} +
                              sizeof(IndexPair) * std::size_t{count};
    void* raw = alloc_bytes(bytes, alignof(IndexSegment));
    if (raw == nullptr) {
        *failed = true;
        return nullptr;
    }
    IndexSegment* seg = new (raw) IndexSegment();
    NOVA_COUNT_OBJECT(LiveObject::IndexSegment, 1);
    seg->owners.store(0, std::memory_order_relaxed);
    seg->bucket_capacity = capacity;
    seg->distinct = distinct;
    seg->pair_count = count;
    seg->buckets = reinterpret_cast<IndexBucket*>(seg + 1);
    seg->pairs = reinterpret_cast<IndexPair*>(seg->buckets + capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) seg->buckets[i] = IndexBucket{0, 0, 0};
    std::memcpy(seg->pairs, sorted, sizeof(IndexPair) * std::size_t{count});

    const std::uint32_t mask = capacity - 1;
    for (std::uint32_t first = 0; first < count;) {
        std::uint32_t end = first + 1;
        while (end < count && sorted[end].hash == sorted[first].hash) ++end;
        std::uint32_t i = bucket_start(sorted[first].hash, mask);
        while (seg->buckets[i].count != 0) i = (i + 1) & mask;
        seg->buckets[i] = IndexBucket{sorted[first].hash, first, end - first};
        first = end;
    }
    return seg;
}

void index_sort_deltas(IndexDelta* deltas, std::size_t n) noexcept {
    std::sort(deltas, deltas + n, [](const IndexDelta& a, const IndexDelta& b) {
        const std::uint32_t pa = index_partition_of(a.hash), pb = index_partition_of(b.hash);
        if (pa != pb) return pa < pb;
        if (a.hash != b.hash) return a.hash < b.hash;
        if (a.key != b.key) return a.key < b.key;
        return a.seq < b.seq;
    });
}

IndexSegment* index_merge(const IndexSegment* base, const IndexDelta* deltas, std::size_t n,
                          bool* failed) noexcept {
    const std::uint32_t base_count = base != nullptr ? base->pair_count : 0;
    // At most every base pair plus one pair per delta.
    const std::size_t room = std::size_t{base_count} + n;
    if (room == 0) return nullptr;
    if (room > 0xFFFFFFFFu) {
        *failed = true;
        return nullptr;
    }
    IndexPair* merged = static_cast<IndexPair*>(alloc_bytes(sizeof(IndexPair) * room, alignof(IndexPair)));
    if (merged == nullptr) {
        *failed = true;
        return nullptr;
    }
    // Both inputs are sorted by (hash, key): one linear merge. For a pair named
    // by deltas, the LAST delta decides whether it is present.
    std::uint32_t out = 0;
    std::uint32_t b = 0;
    std::size_t d = 0;
    while (b < base_count || d < n) {
        if (d == n) {
            merged[out++] = base->pairs[b++];
            continue;
        }
        const IndexPair dp{deltas[d].hash, deltas[d].key};
        if (b < base_count && pair_less(base->pairs[b], dp)) {
            merged[out++] = base->pairs[b++];
            continue;
        }
        std::size_t last = d;
        while (last + 1 < n && deltas[last + 1].hash == dp.hash && deltas[last + 1].key == dp.key) ++last;
        if (b < base_count && !pair_less(dp, base->pairs[b])) ++b;  // the same pair in the base: superseded
        if (deltas[last].add) merged[out++] = dp;
        d = last + 1;
    }
    IndexSegment* seg = index_build(merged, out, failed);
    free_bytes(merged);
    return seg;
}

}  // namespace nova
