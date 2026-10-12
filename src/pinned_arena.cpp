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
      mark_current_(nullptr),
      mark_used_(0),
      commit_refs_(nullptr),
      commit_bytes_(0),
      free_head_(kNoChunk) {
    for (auto& b : blocks_) b.store(nullptr, std::memory_order_relaxed);
}

PinnedArena::~PinnedArena() {
    // Precondition: writer and cleaner have stopped. Whatever is still in the
    // directory (normally just the writable chunk) is freed here.
    for (std::uint32_t b = 0; b < kChunkDirMaxBlocks; ++b) {
        DirBlock* block = blocks_[b].load(std::memory_order_relaxed);
        if (block == nullptr) continue;
        for (std::uint32_t s = 0; s < kChunkDirBlockSlots; ++s) {
            PinnedChunk* c = block->slot[s].load(std::memory_order_relaxed);
            if (c != nullptr) NOVA_COUNT_OBJECT(LiveObject::Chunk, -1);
            free_bytes(c);
        }
        free_bytes(block);
    }
}

const PinnedChunk* PinnedArena::chunk(std::uint32_t id) const noexcept {
    if (id >= next_fresh_id_) return nullptr;
    return block_of(id)->slot[id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
}

PinnedArenaStats PinnedArena::stats() const noexcept {
    PinnedArenaStats s;
    s.chunks_created = chunks_created_.load(std::memory_order_relaxed);
    s.chunks_freed = chunks_freed_.load(std::memory_order_relaxed);
    s.chunks_live = s.chunks_created - s.chunks_freed;
    s.bytes_reserved = bytes_reserved();
    s.refs_dropped = refs_dropped_.load(std::memory_order_relaxed);
    s.chunk_ids_issued = chunk_ids_issued_;
    s.ids_reused = ids_reused_;
    s.bytes_appended = bytes_appended_;
    s.rollbacks = rollbacks_;
    s.refs_taken = refs_taken_;
    return s;
}

// ---- Chunk-ID pool ---------------------------------------------------------------
//
// A Treiber stack threaded through the per-block `next_free` arrays: the link
// of a pooled ID lives in the directory block of that ID, so pushing needs no
// allocation and cannot fail.
//
// Pushers: any thread that destroys a chunk (writer or cleaner).
// Popper:  only the writer.
//
// ABA: a pop reads head H and link L = next_free[H], then CASes head H -> L.
// That is wrong only if, in between, H was popped and pushed back with a
// different link. Only the writer pops, and it is the thread executing this
// pop, so H cannot have been popped in between; concurrent pushes change the
// head and make the CAS fail, after which the pop retries. A push that
// re-reads a stale head is likewise caught by its own CAS.

void PinnedArena::push_free_id(std::uint32_t id) noexcept {
    std::atomic<std::uint32_t>& link = block_of(id)->next_free[id & (kChunkDirBlockSlots - 1)];
    std::uint32_t head = free_head_.load(std::memory_order_seq_cst);
    do {
        link.store(head, std::memory_order_seq_cst);
    } while (!free_head_.compare_exchange_weak(head, id, std::memory_order_seq_cst));
}

bool PinnedArena::pop_free_id(std::uint32_t* id) noexcept {
    std::uint32_t head = free_head_.load(std::memory_order_seq_cst);
    while (head != kNoChunk) {
        const std::uint32_t next =
            block_of(head)->next_free[head & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
        if (free_head_.compare_exchange_weak(head, next, std::memory_order_seq_cst)) {
            *id = head;
            return true;
        }
    }
    return false;
}

// ---- Creation and destruction -----------------------------------------------------

PinnedChunk* PinnedArena::new_chunk(std::uint32_t capacity) noexcept {
    // Step 1: memory for the chunk. Nothing shared has changed if this fails.
    void* raw = alloc_bytes(sizeof(PinnedChunk) + static_cast<std::size_t>(capacity),
                            alignof(PinnedChunk));
    if (raw == nullptr) return nullptr;

    // Step 2: an ID, recycled if the pool has one, otherwise fresh. A directory
    // block allocated for a fresh ID stays if this commit later fails; it is
    // not reachable from any ValueRef.
    std::uint32_t id;
    bool recycled = pop_free_id(&id);
    if (!recycled) {
        id = next_fresh_id_;
        const std::uint32_t block_index = id >> kChunkDirBlockBits;
        if (block_index >= max_blocks_) {  // directory exhausted
            free_bytes(raw);
            return nullptr;
        }
        if (blocks_[block_index].load(std::memory_order_relaxed) == nullptr) {
            void* braw = alloc_bytes(sizeof(DirBlock), alignof(DirBlock));
            if (braw == nullptr) {
                free_bytes(raw);
                return nullptr;
            }
            DirBlock* block = new (braw) DirBlock();
            for (std::uint32_t s = 0; s < kChunkDirBlockSlots; ++s) {
                block->slot[s].store(nullptr, std::memory_order_relaxed);
                block->next_free[s].store(kNoChunk, std::memory_order_relaxed);
            }
            // Installed once, fully initialized, never replaced or freed while
            // the arena lives. Nobody looks at this block before an ID inside
            // it has been issued.
            blocks_[block_index].store(block, std::memory_order_seq_cst);
        }
        ++next_fresh_id_;
        ++chunk_ids_issued_;
    } else {
        ++ids_reused_;
    }

    PinnedChunk* c = new (raw) PinnedChunk();
    NOVA_COUNT_OBJECT(LiveObject::Chunk, 1);
    c->id = id;
    c->capacity = capacity;
    c->used = 0;
    c->sealed = false;
    c->link = nullptr;
    // Born with one reference, owned by the caller (it becomes the writable
    // reference or a commit reference).
    c->refs.store(1, std::memory_order_relaxed);
    chunks_created_.fetch_add(1, std::memory_order_relaxed);
    bytes_created_.fetch_add(capacity, std::memory_order_relaxed);
    // Publish in the directory before any version whose ValueRefs name this
    // chunk. The slot is null here: fresh, or cleared by destroy() before the
    // ID entered the pool.
    block_of(id)->slot[id & (kChunkDirBlockSlots - 1)].store(c, std::memory_order_seq_cst);
    return c;
}

void PinnedArena::drop_ref(PinnedChunk* c) noexcept {
    // THE freeing rule: the decrement that reaches zero destroys. Exactly one
    // decrement observes 1, so exactly one thread destroys, and it does so
    // only when no reference of any kind is left.
    if (c->refs.fetch_sub(1, std::memory_order_seq_cst) == 1) destroy(c);
}

void PinnedArena::destroy(PinnedChunk* c) noexcept {
    const std::uint32_t id = c->id;
    const std::uint32_t capacity = c->capacity;
    // 1. Withdraw from the directory.
    block_of(id)->slot[id & (kChunkDirBlockSlots - 1)].store(nullptr, std::memory_order_seq_cst);
    // 2. Free the memory.
    NOVA_COUNT_OBJECT(LiveObject::Chunk, -1);
    c->~PinnedChunk();
    free_bytes(c);
    chunks_freed_.fetch_add(1, std::memory_order_relaxed);
    bytes_freed_.fetch_add(capacity, std::memory_order_relaxed);
    // 3. Only now may the ID be handed out again.
    push_free_id(id);
}

// ---- Commits ----------------------------------------------------------------------

void PinnedArena::begin_commit() noexcept {
    mark_current_ = current_;
    mark_used_ = current_ != nullptr ? current_->used : 0;
    commit_refs_ = nullptr;
    commit_bytes_ = 0;
}

bool PinnedArena::append(const void* data, std::uint32_t len, ValueRef* out) noexcept {
    if (len == 0) {
        *out = ValueRef{kNoChunk, 0, 0};
        return true;
    }
    if (len > chunk_bytes_) {
        // Oversized value: a dedicated chunk, sealed from the start. Its birth
        // reference is held by the commit.
        PinnedChunk* c = new_chunk(len);
        if (c == nullptr) return false;
        std::memcpy(c->data(), data, len);
        c->used = len;
        c->sealed = true;
        c->link = commit_refs_;
        commit_refs_ = c;
        commit_bytes_ += len;
        *out = ValueRef{c->id, 0, len};
        return true;
    }
    if (current_ == nullptr || current_->capacity - current_->used < len) {
        PinnedChunk* next = new_chunk(chunk_bytes_);
        if (next == nullptr) return false;
        if (current_ != nullptr) {
            // The previous writable chunk keeps its reference: what was the
            // writable reference is now a commit reference, because this commit
            // may hold unpublished ValueRefs into the chunk.
            current_->sealed = true;
            current_->link = commit_refs_;
            commit_refs_ = current_;
        }
        current_ = next;  // its birth reference is the writable reference
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
    bytes_appended_ += commit_bytes_;
    NOVA_TEST_POINT(TestPoint::WriterEndCommitBeforeRelease);
    // The new segments hold their own references now. Drop the commit's.
    for (PinnedChunk* c = commit_refs_; c != nullptr;) {
        PinnedChunk* next = c->link;
        c->link = nullptr;
        drop_ref(c);  // `c` may be gone after this line
        c = next;
    }
    commit_refs_ = nullptr;
    mark_current_ = nullptr;
}

void PinnedArena::rollback_commit() noexcept {
    ++rollbacks_;
    // Chunks the failed commit holds a reference on: everything it created
    // except possibly the current writable one, plus the chunk that was
    // writable when it began if the commit moved on from it. The latter is not
    // released: it becomes the writable chunk again and keeps its reference.
    for (PinnedChunk* c = commit_refs_; c != nullptr;) {
        PinnedChunk* next = c->link;
        c->link = nullptr;
        if (c != mark_current_) drop_ref(c);
        c = next;
    }
    commit_refs_ = nullptr;
    // A writable chunk created by this commit: drop its birth reference too.
    if (current_ != mark_current_ && current_ != nullptr) drop_ref(current_);
    current_ = mark_current_;
    if (current_ != nullptr) {
        current_->used = mark_used_;  // drop the unpublished tail
        current_->sealed = false;
    }
    mark_current_ = nullptr;
    commit_bytes_ = 0;
}

// ---- Segment references -----------------------------------------------------------

void PinnedArena::add_ref(std::uint32_t chunk_id) noexcept {
    PinnedChunk* c =
        block_of(chunk_id)->slot[chunk_id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
    c->refs.fetch_add(1, std::memory_order_seq_cst);
    ++refs_taken_;
}

void PinnedArena::release(std::uint32_t chunk_id) noexcept {
    PinnedChunk* c =
        block_of(chunk_id)->slot[chunk_id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
    refs_dropped_.fetch_add(1, std::memory_order_relaxed);
    drop_ref(c);
}

}  // namespace nova
