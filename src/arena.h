// arena.h -- chunked, append-only value storage with a global chunk directory.
//
// Values are appended to fixed-size chunks and addressed by ValueRef
// (chunk_id, offset, length). Stored bytes are never overwritten while any
// index entry references them, and a chunk's address is stable for the lifetime
// of its allocation. The chunk directory is a fixed two-level table
// (chunk_id -> Chunk*) whose blocks are installed on demand and never freed
// while the arena lives.
//
// A1 lifetime rule (docs/decisions.md D-017): each chunk counts the index
// entries referencing it. The writer, holding the store's exclusive lock,
// frees a sealed chunk and recycles its ID when the count reaches zero. This is
// valid only while readers cannot hold a ValueRef across a writer operation and
// is replaced by segment pins + EBR in Part B.
//
// Synchronization is the caller's job. The class itself is not thread-safe.
#pragma once

#include "config.h"
#include "value_ref.h"

#include <cstddef>
#include <cstdint>

namespace nova {

struct Chunk {
    std::uint32_t id;
    std::uint32_t capacity;   // payload bytes
    std::uint32_t used;       // bytes appended so far
    std::uint32_t live_refs;  // index entries pointing into this chunk
    bool sealed;              // accepts no further appends

    // Payload follows the header in the same allocation.
    std::byte* data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    const std::byte* data() const noexcept {
        return reinterpret_cast<const std::byte*>(this + 1);
    }
};

struct ArenaStats {
    std::uint64_t chunks_live = 0;       // chunk allocations currently held
    std::uint64_t chunk_ids_issued = 0;  // distinct IDs ever handed out
    std::uint64_t chunks_freed = 0;      // chunk allocations released so far
    std::uint64_t ids_reused = 0;        // allocations that took a recycled ID
    std::uint64_t bytes_reserved = 0;    // payload capacity of live chunks
    std::uint64_t value_bytes_live = 0;  // bytes referenced by index entries
};

class Arena {
public:
    // `max_blocks` (<= kChunkDirMaxBlocks) caps the directory; tests lower it
    // and `chunk_bytes` to reach the limits cheaply.
    explicit Arena(std::uint32_t chunk_bytes = kChunkBytes,
                   std::uint32_t max_blocks = kChunkDirMaxBlocks) noexcept;
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Copies `len` bytes into the arena and returns their reference. `len` must
    // be <= kMaxValueBytes; `data` may be null only if len == 0. Returns false
    // on allocation failure or directory exhaustion, leaving every existing
    // value and reference intact.
    bool append(const void* data, std::uint32_t len, ValueRef* out) noexcept;

    // Drops one index reference. Frees the chunk (and recycles its ID) when it
    // was the last reference to a sealed chunk. Cannot fail.
    void release(const ValueRef& ref) noexcept;

    // Bytes of a non-empty reference. Valid until that reference is released.
    const std::byte* data(const ValueRef& ref) const noexcept {
        return blocks_[ref.chunk_id >> kChunkDirBlockBits]
                      [ref.chunk_id & (kChunkDirBlockSlots - 1)]
                   ->data() +
               ref.offset;
    }

    // Bounds-checked directory lookup; nullptr if the ID has no live chunk.
    const Chunk* chunk(std::uint32_t id) const noexcept;
    const Chunk* current() const noexcept { return current_; }
    std::uint32_t chunk_bytes() const noexcept { return chunk_bytes_; }
    const ArenaStats& stats() const noexcept { return stats_; }

private:
    Chunk* new_chunk(std::uint32_t capacity) noexcept;
    void free_chunk(Chunk* c) noexcept;
    Chunk*& slot(std::uint32_t id) noexcept {
        return blocks_[id >> kChunkDirBlockBits][id & (kChunkDirBlockSlots - 1)];
    }

    std::uint32_t chunk_bytes_;
    std::uint32_t max_blocks_;
    Chunk* current_;               // the one writable chunk, or null
    std::uint32_t next_fresh_id_;  // IDs below this have been issued
    std::uint32_t* free_ids_;      // stack of recyclable IDs
    std::uint32_t free_count_;
    std::uint32_t free_capacity_;  // always >= next_fresh_id_, so a push never allocates
    ArenaStats stats_;
    Chunk** blocks_[kChunkDirMaxBlocks];
};

}  // namespace nova
