// pinned_arena.h -- value storage for the Part B (RCU) store: append-only
// chunks behind a global chunk directory that concurrent readers may consult.
//
// LIFETIME: ONE REFERENCE COUNT PER CHUNK, ONE RULE.
//
// Every reason a chunk must stay alive is one counted reference (`refs`):
//
//   segment reference   each existing Segment that lists the chunk holds one,
//                       taken before the segment is published and dropped when
//                       the segment is destroyed (by the cleaner, after the
//                       grace period of every version that owned the segment);
//   writable reference  the writer holds one on the chunk it appends to, for as
//                       long as that chunk is the writable one;
//   commit reference    an open commit holds one on every chunk it created and
//                       on a chunk that stopped being writable during it, until
//                       the commit has ended (its new segments have taken their
//                       own references) or has been rolled back.
//
// A chunk is destroyed by exactly one event: the `fetch_sub` that takes `refs`
// from 1 to 0. Whichever thread performs that decrement (writer or cleaner)
// destroys the chunk; no thread ever inspects the count and then decides to
// free. Destruction clears the directory slot, frees the memory and only then
// returns the chunk ID to the pool, so an ID is reusable only after nothing can
// reference its previous chunk.
//
// Because a reader can reach a chunk only through a Segment of a version it is
// protected in, and that Segment holds a reference for as long as it exists, a
// chunk a reader can still dereference is never destroyed and its ID never
// reused (INV-05, INV-06). The writer never rewinds or overwrites a chunk.
//
// THREADS: one writer calls begin/append/end/rollback/add_ref. release() may be
// called by the writer and by the cleaner concurrently. data() is safe from any
// number of readers concurrently with both.
//
// The chunk-ID pool is a lock-free stack with any number of pushers (whoever
// destroys a chunk) and a single popper (the writer); see pinned_arena.cpp for
// why a single popper makes it ABA-free.
#pragma once

#include "config.h"
#include "value_ref.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nova {

struct PinnedChunk {
    std::uint32_t id;                  // immutable
    std::uint32_t capacity;            // immutable: payload bytes
    std::uint32_t used;                // writer-only: bytes appended so far
    bool sealed;                       // writer-only: accepts no further appends
    std::atomic<std::uint32_t> refs;   // all references, see the header comment
    PinnedChunk* link;                 // writer-only: list of the open commit's references

    // Payload follows the header. Readers use only this address computation
    // and the bytes themselves; they never read the fields above.
    std::byte* data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    const std::byte* data() const noexcept {
        return reinterpret_cast<const std::byte*>(this + 1);
    }
};

struct PinnedArenaStats {
    std::uint64_t chunks_live = 0;
    std::uint64_t chunk_ids_issued = 0;    // distinct IDs ever handed out
    std::uint64_t chunks_created = 0;
    std::uint64_t chunks_freed = 0;
    std::uint64_t ids_reused = 0;          // creations that took a recycled ID
    std::uint64_t bytes_reserved = 0;      // payload capacity of live chunks
    std::uint64_t bytes_appended = 0;      // committed value bytes, cumulative
    std::uint64_t rollbacks = 0;           // commits whose appends were undone
    std::uint64_t refs_taken = 0;          // segment references taken
    std::uint64_t refs_dropped = 0;        // segment references dropped
};

class PinnedArena {
public:
    explicit PinnedArena(std::uint32_t chunk_bytes = kChunkBytes,
                         std::uint32_t max_blocks = kChunkDirMaxBlocks) noexcept;
    ~PinnedArena();
    PinnedArena(const PinnedArena&) = delete;
    PinnedArena& operator=(const PinnedArena&) = delete;

    // ---- Writer: one commit = begin, appends, then end or rollback ----------
    void begin_commit() noexcept;
    // Copies `len` bytes and returns their reference. False on allocation
    // failure or directory exhaustion; nothing already appended is disturbed.
    // The chunk holding the bytes is protected by a writable or commit
    // reference until end_commit() / rollback_commit().
    bool append(const void* data, std::uint32_t len, ValueRef* out) noexcept;
    // Ends the commit: drops its commit references. Call AFTER the new segments
    // have taken their references with add_ref(); a chunk nobody else
    // references is destroyed by that drop.
    void end_commit() noexcept;
    // Discards every append since begin_commit(): drops the references on
    // chunks the commit created and restores the writable chunk and its write
    // offset. Cannot fail.
    void rollback_commit() noexcept;

    // ---- Segment references --------------------------------------------------
    // Writer only. The caller must already be protecting the chunk: it is
    // listed by a segment of the current version, or covered by a writable or
    // commit reference. Never an increment from zero.
    void add_ref(std::uint32_t chunk_id) noexcept;
    // Writer or cleaner. Drops one segment reference; destroys the chunk if it
    // was the last reference of any kind.
    void release(std::uint32_t chunk_id) noexcept;

    // ---- Readers --------------------------------------------------------------
    // Bytes of a non-empty reference obtained from a Segment the caller is
    // protected in.
    const std::byte* data(const ValueRef& ref) const noexcept {
        const DirBlock* block =
            blocks_[ref.chunk_id >> kChunkDirBlockBits].load(std::memory_order_seq_cst);
        const PinnedChunk* c =
            block->slot[ref.chunk_id & (kChunkDirBlockSlots - 1)].load(std::memory_order_seq_cst);
        return c->data() + ref.offset;
    }

    // ---- Introspection --------------------------------------------------------
    // chunk(), current(): only when neither the writer nor the cleaner is running.
    const PinnedChunk* chunk(std::uint32_t id) const noexcept;
    const PinnedChunk* current() const noexcept { return current_; }
    std::uint32_t chunk_bytes() const noexcept { return chunk_bytes_; }
    // Consistent when writer and cleaner are quiet; while they run, each field
    // is individually valid but the set is not a snapshot.
    PinnedArenaStats stats() const noexcept;
    // Payload bytes of live chunks. Safe from any thread at any time.
    std::uint64_t bytes_reserved() const noexcept {
        return bytes_created_.load(std::memory_order_relaxed) -
               bytes_freed_.load(std::memory_order_relaxed);
    }

private:
    struct DirBlock {
        std::atomic<PinnedChunk*> slot[kChunkDirBlockSlots];
        std::atomic<std::uint32_t> next_free[kChunkDirBlockSlots];  // ID-pool links
    };

    PinnedChunk* new_chunk(std::uint32_t capacity) noexcept;
    void drop_ref(PinnedChunk* c) noexcept;   // any thread
    void destroy(PinnedChunk* c) noexcept;    // the thread whose drop reached zero
    DirBlock* block_of(std::uint32_t id) const noexcept {
        return blocks_[id >> kChunkDirBlockBits].load(std::memory_order_seq_cst);
    }
    void push_free_id(std::uint32_t id) noexcept;    // any thread
    bool pop_free_id(std::uint32_t* id) noexcept;    // writer only

    const std::uint32_t chunk_bytes_;
    const std::uint32_t max_blocks_;

    // ---- Writer-only state ---------------------------------------------------
    PinnedChunk* current_;          // the writable chunk (holds the writable reference), or null
    std::uint32_t next_fresh_id_;
    PinnedChunk* mark_current_;     // writable chunk when the open commit began
    std::uint32_t mark_used_;       // its write offset then
    PinnedChunk* commit_refs_;      // chunks on which the open commit holds a reference
    std::uint64_t commit_bytes_;
    std::uint64_t chunk_ids_issued_ = 0;
    std::uint64_t ids_reused_ = 0;
    std::uint64_t bytes_appended_ = 0;
    std::uint64_t rollbacks_ = 0;
    std::uint64_t refs_taken_ = 0;

    // ---- Shared between writer and cleaner ------------------------------------
    std::atomic<std::uint32_t> free_head_;           // top of the ID pool, kNoChunk if empty
    std::atomic<std::uint64_t> chunks_created_{0};
    std::atomic<std::uint64_t> bytes_created_{0};
    std::atomic<std::uint64_t> chunks_freed_{0};
    std::atomic<std::uint64_t> bytes_freed_{0};
    std::atomic<std::uint64_t> refs_dropped_{0};

    std::atomic<DirBlock*> blocks_[kChunkDirMaxBlocks];
};

}  // namespace nova
