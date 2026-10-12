// versioned_store.h -- the Part B storage engine: immutable published versions,
// copy-on-write index, one atomic root pointer.
//
// MILESTONE B1 STATE. Publication and ownership are complete; RECLAMATION IS
// NOT: there is no epoch protocol and no cleaner yet. Every replaced version is
// kept on a writer-owned retained list until the store is destroyed, so any
// reader, however slow, finds every object it can reach intact. Epoch-based
// reclamation (blueprint section 5, formal-safety supplement) is milestone B2.
//
// Shape (blueprint section 3):
//
//   current ──atomic──> Version { sequence, segments[256] }      immutable
//                           └──> Segment { slots[], chunk pins }  immutable, shared
//                                    └──> ValueRef {chunk_id, offset, length}
//   chunk directory (global, atomic slots) ──> chunk bytes        append-only
//
// Reads (any number of threads): one atomic load of `current`, then only
// immutable objects; no lock, no allocation, no shared write.
//
// Writes (exactly ONE writer thread, as the assignment assumes): build the
// replacement segments privately, allocate the new Version, and only when
// nothing can fail any more take ownership and swap `current` once. A logical
// write of any number of keys is one publication: each affected segment is
// cloned once, unaffected segments are shared with the previous version.
//
// Invariants this file is responsible for (blueprint section 7):
//   INV-01 a published Version, its Segments and the value bytes they reference
//          are never modified.
//   INV-02 only the writer stores to `current`.
//   INV-03 one successful put / erase / commit = exactly one store to `current`.
//   INV-05 every published Segment pins every chunk it references.
//   INV-10 a failed commit publishes nothing and leaves no trace.
//   INV-15 no internal pointer is returned; get() copies.
#pragma once

#include "cow_segment.h"
#include "pinned_arena.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nova {

// One complete, immutable state of the store.
struct Version {
    std::uint64_t sequence;                 // commits since creation; diagnostic only
    Segment* segments[kPartitionCount];     // the directory; null = empty partition
};

struct BatchOp {
    enum class Kind : std::uint8_t { Put, Erase };
    Kind kind;
    long key;
    const void* value;   // Put only
    std::size_t length;  // Put only
};

struct VersionedStats {
    // Publication
    std::uint64_t versions_published = 0;   // successful stores to `current`
    std::uint64_t versions_retained = 0;    // replaced versions currently kept
    std::uint64_t versions_released = 0;    // versions whose ownership was released
    std::uint64_t commits_failed = 0;       // commits that published nothing (allocation, limits)
    std::uint64_t commits_without_change = 0;  // e.g. erase of absent keys only
    // Copy-on-write cost
    std::uint64_t segments_built = 0;       // replacement segments published
    std::uint64_t segments_dropped = 0;     // partitions that became empty
    std::uint64_t segments_released = 0;    // segments destroyed
    std::uint64_t segment_growths = 0;
    std::uint64_t segment_rebuilds = 0;
    std::uint64_t index_bytes_copied = 0;   // slot bytes copied/rebuilt for published segments
    std::uint64_t directory_bytes_copied = 0;
    std::uint64_t owner_refs_taken = 0;     // segment references acquired by new versions
    // Memory held by index structures (versions + segments), current and retained
    std::uint64_t index_bytes_held = 0;
    PinnedArenaStats arena;
};

class VersionedStore {
public:
    VersionedStore() noexcept;
    ~VersionedStore();
    VersionedStore(const VersionedStore&) = delete;
    VersionedStore& operator=(const VersionedStore&) = delete;

    // False if the initial empty version could not be allocated.
    bool ok() const noexcept { return latest_ != nullptr; }

    // ---- Readers (any thread) -----------------------------------------------
    bool get(long key, void* value_out, std::size_t value_out_len) const noexcept;

    // ---- The single writer -----------------------------------------------------
    bool put(long key, const void* value, std::size_t value_len) noexcept;
    bool erase(long key) noexcept;
    // Applies `ops` in order as ONE logical write and publishes at most once.
    // Returns false, having changed nothing, if an argument is invalid or an
    // allocation fails. `applied` (optional, n entries) reports per operation
    // whether it took effect: always true for a put, true for an erase only if
    // the key was present at that point of the batch.
    bool commit(const BatchOp* ops, std::size_t n, bool* applied = nullptr) noexcept;

    // ---- Temporary lifetime policy (B1) -----------------------------------------
    // Default: every replaced version is retained until destruction. That is
    // the only mode that is safe with concurrent readers in this milestone.
    //
    // Quiescent reclamation: the CALLER PROMISES that no other thread is inside
    // or will enter get() while it is in effect (single-threaded tests and
    // benchmark phases). Replaced versions are then released at once, which
    // exercises the full ownership graph (segment references, chunk pins, chunk
    // ID reuse) without any reader that could be harmed. It is not a
    // reclamation protocol and must never be enabled with concurrent readers.
    void set_quiescent_reclaim(bool enabled) noexcept { quiescent_reclaim_ = enabled; }
    bool quiescent_reclaim() const noexcept { return quiescent_reclaim_; }
    // One-off release of everything retained so far, under the same promise.
    void release_retained_quiescent() noexcept;

    // ---- Introspection (tests, statistics; see each comment for thread rules) --
    // Safe from any thread in B1 because nothing is freed while retaining.
    const Version* current_version() const noexcept {
        return current_.load(std::memory_order_seq_cst);
    }
    // Reads through a specific version instead of `current`.
    bool get_at(const Version* v, long key, void* value_out,
                std::size_t value_out_len) const noexcept;
    const VersionedStats& stats() const noexcept;        // writer thread or quiescent
    std::uint64_t memory_held_bytes() const noexcept {   // writer thread or quiescent
        return stats_.index_bytes_held + arena_.stats().bytes_reserved;
    }
    const PinnedArena& arena() const noexcept { return arena_; }
    const void* root_address() const noexcept { return &current_; }
    // Calls fn(version, context) for the current and every retained version. Quiescent.
    void for_each_version(void (*fn)(const Version*, void*), void* context) const;
    // Recomputes every owner count and chunk pin from the versions that exist
    // and compares with the stored counts. nullptr if consistent. Quiescent.
    const char* validate() const;

private:
    struct RetiredNode {
        Version* version;
        RetiredNode* next;
    };

    bool read(const Version* v, long key, std::uint64_t h, void* value_out,
              std::size_t value_out_len) const noexcept;
    void release_version(Version* v) noexcept;

    // The one word readers and the writer share. Alone on its cache line so
    // that writer-side state next to it is never dragged between cores by
    // readers, and vice versa.
    alignas(kCacheLine) std::atomic<Version*> current_;

    // Writer-only state.
    alignas(kCacheLine) Version* latest_;   // == current_, without an atomic load
    RetiredNode* retired_;                  // replaced versions, newest first
    bool quiescent_reclaim_;
    mutable VersionedStats stats_;
    PinnedArena arena_;
};

// Process-wide default for newly constructed stores (tests of single-threaded
// code paths only; see set_quiescent_reclaim).
void set_default_quiescent_reclaim(bool enabled) noexcept;

}  // namespace nova
