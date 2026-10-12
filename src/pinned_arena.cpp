#include "pinned_arena.h"

#include "alloc.h"
#include "test_points.h"

#include <cstring>
#include <new>

namespace nova {

PinnedArena::PinnedArena(std::uint32_t chunk_bytes, std::uint32_t max_blocks) noexcept
    : chunk_bytes_(chunk_bytes),
      max_blocks_(max_blocks < kChunkDirMaxBlocks ? max_blocks : kChunkDirMaxBlocks),
      current_(nullptr),
      next_fresh_id_(0),
      free_ids_(nullptr),
      free_count_(0),
      free_capacity_(0),
      mark_current_(nullptr),
      mark_used_(0),
      commit_new_(nullptr),
      commit_bytes_(0),
      stats_() {
    for (auto& b : blocks_) b.store(nullptr, std::memory_order_relaxed);
}

PinnedArena::~PinnedArena() {
    for (std::uint32_t b = 0; b < kChunkDirMaxBlocks; ++b) {
        std::atomic<PinnedChunk*>* block = blocks_[b].load(std::memory_order_relaxed);
        if (block == nullptr) continue;
        for (std::uint32_t s = 0; s < kChunkDirBlockSlots; ++s) {
            PinnedChunk* c = block[s].load(std::memory_order_relaxed);
            if (c != nullptr) NOVA_COUNT_OBJECT(LiveObject::Chunk, -1);
            free_bytes(c);
        }
        free_bytes(block);
    }
    free_bytes(free_ids_);
}

const PinnedChunk* PinnedArena::chunk(std::uint32_t id) const noexcept {
    if (id >= next_fresh_id_) return nullptr;
    return blocks_[id >> kChunkDirBlockBits].load(std::memory_order_seq_cst)
        [id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
}

PinnedChunk* PinnedArena::new_chunk(std::uint32_t capacity) noexcept {
    // Step 1: an ID. A directory block or a larger free-ID stack allocated here
    // stays if a later step fails; neither is reachable from any ValueRef.
    const bool fresh = free_count_ == 0;
    std::uint32_t id;
    if (fresh) {
        id = next_fresh_id_;
        const std::uint32_t block_index = id >> kChunkDirBlockBits;
        if (block_index >= max_blocks_) return nullptr;  // directory exhausted
        if (blocks_[block_index].load(std::memory_order_relaxed) == nullptr) {
            void* raw = alloc_bytes(sizeof(std::atomic<PinnedChunk*>) * kChunkDirBlockSlots);
            if (raw == nullptr) return nullptr;
            auto* block = static_cast<std::atomic<PinnedChunk*>*>(raw);
            for (std::uint32_t s = 0; s < kChunkDirBlockSlots; ++s) {
                new (&block[s]) std::atomic<PinnedChunk*>(nullptr);
            }
            // Installed once, fully initialized, never replaced or freed while
            // the arena lives. No reader looks at this block before a version
            // that references one of its chunks has been published.
            blocks_[block_index].store(block, std::memory_order_seq_cst);
        }
        if (free_capacity_ < id + 1) {
            const std::uint32_t cap = free_capacity_ < 32 ? 64 : free_capacity_ * 2;
            auto* grown = static_cast<std::uint32_t*>(alloc_bytes(sizeof(std::uint32_t) * cap));
            if (grown == nullptr) return nullptr;
            if (free_count_ != 0) std::memcpy(grown, free_ids_, sizeof(std::uint32_t) * free_count_);
            free_bytes(free_ids_);
            free_ids_ = grown;
            free_capacity_ = cap;
        }
    } else {
        id = free_ids_[free_count_ - 1];
    }

    // Step 2: the chunk. The ID is consumed only once this succeeds.
    void* raw = alloc_bytes(sizeof(PinnedChunk) + static_cast<std::size_t>(capacity),
                            alignof(PinnedChunk));
    if (raw == nullptr) return nullptr;
    PinnedChunk* c = new (raw) PinnedChunk();
    NOVA_COUNT_OBJECT(LiveObject::Chunk, 1);
    if (fresh) {
        ++next_fresh_id_;
        ++stats_.chunk_ids_issued;
    } else {
        --free_count_;
        ++stats_.ids_reused;
    }
    c->id = id;
    c->capacity = capacity;
    c->used = 0;
    c->sealed = false;
    c->pins.store(0, std::memory_order_relaxed);
    c->link = commit_new_;
    commit_new_ = c;
    // The directory entry is published before any version whose ValueRefs name
    // this chunk; the slot held nullptr until now (fresh, or cleared when the
    // previous owner of this ID was freed).
    slot(id).store(c, std::memory_order_seq_cst);
    ++stats_.chunks_live;
    stats_.bytes_reserved += capacity;
    return c;
}

void PinnedArena::free_chunk(PinnedChunk* c) noexcept {
    slot(c->id).store(nullptr, std::memory_order_seq_cst);  // clear the slot first,
    free_ids_[free_count_++] = c->id;                       // then make the ID reusable
    --stats_.chunks_live;
    ++stats_.chunks_freed;
    stats_.bytes_reserved -= c->capacity;
    NOVA_COUNT_OBJECT(LiveObject::Chunk, -1);
    c->~PinnedChunk();
    free_bytes(c);
}

void PinnedArena::begin_commit() noexcept {
    mark_current_ = current_;
    mark_used_ = current_ != nullptr ? current_->used : 0;
    commit_new_ = nullptr;
    commit_bytes_ = 0;
}

bool PinnedArena::append(const void* data, std::uint32_t len, ValueRef* out) noexcept {
    if (len == 0) {
        *out = ValueRef{kNoChunk, 0, 0};
        return true;
    }
    if (len > chunk_bytes_) {
        // Oversized value: a dedicated chunk, sealed from the start.
        PinnedChunk* c = new_chunk(len);
        if (c == nullptr) return false;
        std::memcpy(c->data(), data, len);
        c->used = len;
        c->sealed = true;
        commit_bytes_ += len;
        *out = ValueRef{c->id, 0, len};
        return true;
    }
    if (current_ == nullptr || current_->capacity - current_->used < len) {
        PinnedChunk* next = new_chunk(chunk_bytes_);
        if (next == nullptr) return false;
        if (current_ != nullptr) current_->sealed = true;
        current_ = next;
    }
    const std::uint32_t offset = current_->used;
    // Bytes beyond every published reference: no reader can be looking here.
    std::memcpy(current_->data() + offset, data, len);
    current_->used = offset + len;
    commit_bytes_ += len;
    *out = ValueRef{current_->id, offset, len};
    return true;
}

void PinnedArena::end_commit() noexcept {
    stats_.bytes_appended += commit_bytes_;
    // A chunk that stopped being the writable one during this commit and that
    // no segment pinned (its values were all superseded inside the same commit,
    // or it was writable and empty of live values) is unreachable: free it.
    if (mark_current_ != nullptr && mark_current_ != current_ &&
        mark_current_->pins.load(std::memory_order_seq_cst) == 0) {
        free_chunk(mark_current_);
    }
    for (PinnedChunk* c = commit_new_; c != nullptr;) {
        PinnedChunk* next = c->link;
        c->link = nullptr;
        if (c != current_ && c->pins.load(std::memory_order_seq_cst) == 0) free_chunk(c);
        c = next;
    }
    commit_new_ = nullptr;
    mark_current_ = nullptr;
}

void PinnedArena::rollback_commit() noexcept {
    ++stats_.rollbacks;
    // Chunks created by the failed commit were never named by a published
    // ValueRef and hold no pins.
    for (PinnedChunk* c = commit_new_; c != nullptr;) {
        PinnedChunk* next = c->link;
        free_chunk(c);
        c = next;
    }
    commit_new_ = nullptr;
    current_ = mark_current_;
    if (current_ != nullptr) {
        current_->used = mark_used_;  // drop the unpublished tail
        current_->sealed = false;
    }
    mark_current_ = nullptr;
    commit_bytes_ = 0;
}

void PinnedArena::pin(std::uint32_t chunk_id) noexcept {
    slot(chunk_id).load(std::memory_order_seq_cst)->pins.fetch_add(1, std::memory_order_seq_cst);
    ++stats_.pins_taken;
}

void PinnedArena::unpin(std::uint32_t chunk_id) noexcept {
    PinnedChunk* c = slot(chunk_id).load(std::memory_order_seq_cst);
    ++stats_.pins_dropped;
    if (c->pins.fetch_sub(1, std::memory_order_seq_cst) != 1) return;
    if (c != current_) free_chunk(c);  // the writable chunk stays: the writer owns it
}

}  // namespace nova
