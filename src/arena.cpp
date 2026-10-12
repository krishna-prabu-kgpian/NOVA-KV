#include "arena.h"

#include "alloc.h"

#include <cstring>

namespace nova {

Arena::Arena(std::uint32_t chunk_bytes, std::uint32_t max_blocks) noexcept
    : chunk_bytes_(chunk_bytes),
      max_blocks_(max_blocks < kChunkDirMaxBlocks ? max_blocks : kChunkDirMaxBlocks),
      current_(nullptr),
      next_fresh_id_(0),
      free_ids_(nullptr),
      free_count_(0),
      free_capacity_(0),
      stats_(),
      blocks_() {}

Arena::~Arena() {
    for (std::uint32_t b = 0; b < kChunkDirMaxBlocks; ++b) {
        if (blocks_[b] == nullptr) continue;
        for (std::uint32_t s = 0; s < kChunkDirBlockSlots; ++s) free_bytes(blocks_[b][s]);
        free_bytes(blocks_[b]);
    }
    free_bytes(free_ids_);
}

const Chunk* Arena::chunk(std::uint32_t id) const noexcept {
    if (id >= next_fresh_id_) return nullptr;
    return blocks_[id >> kChunkDirBlockBits][id & (kChunkDirBlockSlots - 1)];
}

Chunk* Arena::new_chunk(std::uint32_t capacity) noexcept {
    // Step 1: make sure an ID is available. Anything allocated here (a
    // directory block, a larger free-ID stack) is harmless to keep if a later
    // step fails: it is not reachable from any ValueRef.
    const bool fresh = free_count_ == 0;
    std::uint32_t id;
    if (fresh) {
        id = next_fresh_id_;
        const std::uint32_t block = id >> kChunkDirBlockBits;
        if (block >= max_blocks_) return nullptr;  // directory exhausted
        if (blocks_[block] == nullptr) {
            const std::size_t bytes = sizeof(Chunk*) * kChunkDirBlockSlots;
            Chunk** b = static_cast<Chunk**>(alloc_bytes(bytes));
            if (b == nullptr) return nullptr;
            std::memset(b, 0, bytes);
            blocks_[block] = b;
        }
        if (free_capacity_ < id + 1) {
            // Reserve stack space for this ID now, so that returning it to the
            // free stack later can never fail.
            std::uint32_t cap = free_capacity_ < 32 ? 64 : free_capacity_ * 2;
            std::uint32_t* grown =
                static_cast<std::uint32_t*>(alloc_bytes(sizeof(std::uint32_t) * cap));
            if (grown == nullptr) return nullptr;
            if (free_count_ != 0) {
                std::memcpy(grown, free_ids_, sizeof(std::uint32_t) * free_count_);
            }
            free_bytes(free_ids_);
            free_ids_ = grown;
            free_capacity_ = cap;
        }
    } else {
        id = free_ids_[free_count_ - 1];
    }

    // Step 2: the chunk itself. The ID is consumed only once this succeeds.
    Chunk* c = static_cast<Chunk*>(
        alloc_bytes(sizeof(Chunk) + static_cast<std::size_t>(capacity)));
    if (c == nullptr) return nullptr;
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
    c->live_refs = 0;
    c->sealed = false;
    slot(id) = c;  // directory entry is published before any ValueRef uses it
    ++stats_.chunks_live;
    stats_.bytes_reserved += capacity;
    return c;
}

void Arena::free_chunk(Chunk* c) noexcept {
    slot(c->id) = nullptr;                // clear the directory slot first,
    free_ids_[free_count_++] = c->id;     // only then make the ID reusable
    --stats_.chunks_live;
    ++stats_.chunks_freed;
    stats_.bytes_reserved -= c->capacity;
    free_bytes(c);
}

bool Arena::append(const void* data, std::uint32_t len, ValueRef* out) noexcept {
    if (len == 0) {
        *out = ValueRef{kNoChunk, 0, 0};
        return true;
    }

    if (len > chunk_bytes_) {
        // Oversized value: one dedicated chunk, sealed from the start.
        Chunk* c = new_chunk(len);
        if (c == nullptr) return false;
        std::memcpy(c->data(), data, len);
        c->used = len;
        c->live_refs = 1;
        c->sealed = true;
        stats_.value_bytes_live += len;
        *out = ValueRef{c->id, 0, len};
        return true;
    }

    if (current_ == nullptr || current_->capacity - current_->used < len) {
        Chunk* next = new_chunk(chunk_bytes_);
        if (next == nullptr) return false;  // current_ and its values are untouched
        // The old writable chunk still holds live values here: an unreferenced
        // writable chunk has used == 0 (see release) and always has room.
        if (current_ != nullptr) current_->sealed = true;
        current_ = next;
    }

    const std::uint32_t offset = current_->used;
    std::memcpy(current_->data() + offset, data, len);
    current_->used = offset + len;
    ++current_->live_refs;
    stats_.value_bytes_live += len;
    *out = ValueRef{current_->id, offset, len};
    return true;
}

void Arena::release(const ValueRef& ref) noexcept {
    if (ref.length == 0) return;
    Chunk* c = slot(ref.chunk_id);
    stats_.value_bytes_live -= ref.length;
    if (--c->live_refs != 0) return;
    if (c == current_) {
        c->used = 0;  // nothing references the writable chunk: reuse its space
    } else {
        free_chunk(c);
    }
}

}  // namespace nova
