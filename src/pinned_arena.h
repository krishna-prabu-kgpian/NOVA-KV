// pinned_arena.h -- value storage for the Part B (RCU) store: append-only
// chunks behind a global chunk directory that concurrent readers may consult.
//
// Differences from the Part A arena (arena.h), all required because published
// versions are immutable and several of them may be alive at once:
//
//   * Lifetime is structural, by PINS, not by a count of index entries. Every
//     published segment holds one pin on each distinct chunk it references, from
//     before it is published until it is destroyed. A chunk is freed only when
//     it has no pin, is not the writer's writable chunk and is not referenced by
//     an uncommitted batch. Because a reader can reach a chunk only through a
//     published segment, and that segment holds a pin, a chunk a reader can
//     still dereference is never freed and its ID never reused (INV-05, INV-06).
//   * The writable chunk is never reset or overwritten in place.
//   * The directory is read without any lock: each block pointer and each slot
//     is an atomic. Blocks are installed once and never freed while the arena
//     lives.
//   * Appends made on behalf of a commit that then fails are rolled back to a
//     mark; they were never visible to any reader.
//
// Threading: exactly one writer thread calls the mutating functions. data() is
// safe from any number of reader threads concurrently with the writer.
#pragma once

#include "config.h"
#include "value_ref.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nova {

struct PinnedChunk {
    std::uint32_t id;
    std::uint32_t capacity;        // payload bytes
    std::uint32_t used;            // writer-only: bytes appended so far
    bool sealed;                   // writer-only: accepts no further appends
    std::atomic<std::uint32_t> pins;  // published segments referencing this chunk
    PinnedChunk* link;             // writer-only: per-commit bookkeeping lists

    // Payload follows the header. Readers use only this address computation
    // and the bytes themselves; they never read the mutable fields above.
    std::byte* data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    const std::byte* data() const noexcept {
        return reinterpret_cast<const std::byte*>(this + 1);
    }
};

struct PinnedArenaStats {
    std::uint64_t chunks_live = 0;
    std::uint64_t chunk_ids_issued = 0;
    std::uint64_t chunks_freed = 0;
    std::uint64_t ids_reused = 0;
    std::uint64_t bytes_reserved = 0;      // payload capacity of live chunks
    std::uint64_t bytes_appended = 0;      // committed value bytes, cumulative
    std::uint64_t rollbacks = 0;           // failed commits whose appends were undone
    std::uint64_t pins_taken = 0;
    std::uint64_t pins_dropped = 0;
};

class PinnedArena {
public:
    explicit PinnedArena(std::uint32_t chunk_bytes = kChunkBytes,
                         std::uint32_t max_blocks = kChunkDirMaxBlocks) noexcept;
    ~PinnedArena();
    PinnedArena(const PinnedArena&) = delete;
    PinnedArena& operator=(const PinnedArena&) = delete;

    // ---- Writer: one commit = begin, appends, then commit or rollback --------
    void begin_commit() noexcept;
    // Copies `len` bytes and returns their reference. False on allocation
    // failure or directory exhaustion; nothing already appended is disturbed.
    // The bytes are unreachable until a version referencing them is published.
    bool append(const void* data, std::uint32_t len, ValueRef* out) noexcept;
    // Makes this commit's appends permanent. Call after the new segments have
    // taken their pins: chunks that were sealed during the commit and ended up
    // unpinned are freed here.
    void end_commit() noexcept;
    // Discards every append since begin_commit(): frees chunks created by it and
    // restores the writable chunk and its write offset. Cannot fail.
    void rollback_commit() noexcept;

    // ---- Writer: structural ownership -------------------------------------
    void pin(std::uint32_t chunk_id) noexcept;
    // Drops one pin; frees the chunk (and recycles its ID) if that was the last
    // pin and the chunk is not the writable one. Only legal once the segment
    // that held the pin can no longer be reached by any reader.
    void unpin(std::uint32_t chunk_id) noexcept;

    // ---- Readers --------------------------------------------------------------
    // Bytes of a non-empty reference obtained from a published segment.
    const std::byte* data(const ValueRef& ref) const noexcept {
        const std::atomic<PinnedChunk*>* block =
            blocks_[ref.chunk_id >> kChunkDirBlockBits].load(std::memory_order_seq_cst);
        const PinnedChunk* c =
            block[ref.chunk_id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
        return c->data() + ref.offset;
    }

    // ---- Introspection (quiescent callers: tests, statistics, validation) ---
    const PinnedChunk* chunk(std::uint32_t id) const noexcept;
    const PinnedChunk* current() const noexcept { return current_; }
    std::uint32_t chunk_bytes() const noexcept { return chunk_bytes_; }
    const PinnedArenaStats& stats() const noexcept { return stats_; }

private:
    PinnedChunk* new_chunk(std::uint32_t capacity) noexcept;
    void free_chunk(PinnedChunk* c) noexcept;
    std::atomic<PinnedChunk*>& slot(std::uint32_t id) noexcept {
        return blocks_[id >> kChunkDirBlockBits].load(std::memory_order_seq_cst)
            [id & (kChunkDirBlockSlots - 1)];
    }

    std::uint32_t chunk_bytes_;
    std::uint32_t max_blocks_;
    PinnedChunk* current_;          // the writable chunk (the writer's ownership), or null
    std::uint32_t next_fresh_id_;
    std::uint32_t* free_ids_;
    std::uint32_t free_count_;
    std::uint32_t free_capacity_;   // always >= next_fresh_id_: returning an ID never allocates

    // State of the commit in progress (meaningful between begin and end/rollback).
    PinnedChunk* mark_current_;     // writable chunk when the commit began
    std::uint32_t mark_used_;       // its write offset then
    PinnedChunk* commit_new_;       // chunks allocated by this commit (linked by `link`)
    std::uint64_t commit_bytes_;

    PinnedArenaStats stats_;
    std::atomic<std::atomic<PinnedChunk*>*> blocks_[kChunkDirMaxBlocks];
};

}  // namespace nova
