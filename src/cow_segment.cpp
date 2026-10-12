#include "cow_segment.h"

#include "alloc.h"
#include "test_points.h"

#include <cstring>
#include <new>

namespace nova {

void destroy_segment(Segment* seg) noexcept {
    if (seg == nullptr) return;
    NOVA_COUNT_OBJECT(LiveObject::Segment, -1);
    free_bytes(seg->slots);
    seg->~Segment();
    free_bytes(seg);
}

SegmentBuilder::SegmentBuilder() noexcept
    : work_(kCowInitialSegmentCapacity),
      plan_(),
      pins_(nullptr),
      pin_count_(0),
      pin_capacity_(0),
      header_(nullptr),
      changed_(false) {}

SegmentBuilder::~SegmentBuilder() {
    work_.abort_upsert(plan_);
    free_bytes(pins_);
    free_bytes(header_);  // raw storage only: no Segment was constructed in it
}

bool SegmentBuilder::begin(const Segment* base, std::uint32_t max_ops) noexcept {
    const std::uint32_t base_pins = base != nullptr ? base->pin_count : 0;
    pin_capacity_ = base_pins + max_ops;
    if (pin_capacity_ != 0) {
        pins_ = static_cast<ChunkPin*>(alloc_bytes(sizeof(ChunkPin) * pin_capacity_));
        if (pins_ == nullptr) return false;
        if (base_pins != 0) std::memcpy(pins_, base->pins, sizeof(ChunkPin) * base_pins);
    }
    pin_count_ = base_pins;
    if (base != nullptr &&
        !work_.clone_from(base->slots, base->capacity, base->live, base->tombstones)) {
        return false;
    }
    return true;
}

bool SegmentBuilder::prepare_put(long key, std::uint64_t h) noexcept {
    return work_.prepare_upsert(key, h, plan_);
}

void SegmentBuilder::commit_put(long key, std::uint64_t h, const ValueRef& ref) noexcept {
    ValueRef old{};
    const bool replaced = work_.commit_upsert(key, h, ref, plan_, &old);
    pin_add(ref);
    if (replaced) pin_sub(old);
    changed_ = true;
}

void SegmentBuilder::abort_put() noexcept { work_.abort_upsert(plan_); }

bool SegmentBuilder::erase(long key, std::uint64_t h) noexcept {
    ValueRef old{};
    if (!work_.erase(key, h, &old)) return false;
    pin_sub(old);
    changed_ = true;
    return true;
}

void SegmentBuilder::pin_add(const ValueRef& ref) noexcept {
    if (ref.length == 0) return;  // empty values live in no chunk
    std::uint32_t lo = 0, hi = pin_count_;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (pins_[mid].chunk_id < ref.chunk_id) lo = mid + 1;
        else hi = mid;
    }
    if (lo < pin_count_ && pins_[lo].chunk_id == ref.chunk_id) {
        ++pins_[lo].refs;
        return;
    }
    // Room is guaranteed: begin() reserved one slot per put.
    std::memmove(pins_ + lo + 1, pins_ + lo, sizeof(ChunkPin) * (pin_count_ - lo));
    pins_[lo] = ChunkPin{ref.chunk_id, 1};
    ++pin_count_;
}

void SegmentBuilder::pin_sub(const ValueRef& ref) noexcept {
    if (ref.length == 0) return;
    std::uint32_t lo = 0, hi = pin_count_;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (pins_[mid].chunk_id < ref.chunk_id) lo = mid + 1;
        else hi = mid;
    }
    // Present by construction: every occupied entry is counted in the list.
    --pins_[lo].refs;  // entries that reach zero are dropped in finish()
}

bool SegmentBuilder::prepare_finish() noexcept {
    if (work_.live() == 0) return true;  // the partition becomes empty: no segment
    std::uint32_t used = 0;
    for (std::uint32_t i = 0; i < pin_count_; ++i) used += pins_[i].refs != 0 ? 1u : 0u;
    header_ = static_cast<Segment*>(
        alloc_bytes(sizeof(Segment) + sizeof(ChunkPin) * used, alignof(Segment)));
    return header_ != nullptr;
}

Segment* SegmentBuilder::finish() noexcept {
    if (work_.live() == 0) return nullptr;  // work_'s array is freed with the builder
    Segment* seg = new (header_) Segment();
    NOVA_COUNT_OBJECT(LiveObject::Segment, 1);
    header_ = nullptr;
    seg->owners.store(0, std::memory_order_relaxed);
    seg->capacity = work_.capacity();
    seg->live = work_.live();
    seg->tombstones = work_.tombstones();
    seg->pins = reinterpret_cast<ChunkPin*>(seg + 1);
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < pin_count_; ++i) {
        if (pins_[i].refs != 0) seg->pins[n++] = pins_[i];
    }
    seg->pin_count = n;
    seg->slots = work_.release_slots();
    return seg;
}

}  // namespace nova
