// inspect.h -- internal (non-assignment) window into a store for tests and
// benchmark statistics. Callers must ensure no other thread is using the store.
#pragma once

#include "kvstore.h"

#include <cstddef>
#include <cstdint>

struct NovaKvStats {
    std::uint64_t live_keys = 0;
    std::uint64_t tombstones = 0;
    std::uint64_t slot_capacity = 0;       // slots over all allocated segments
    std::uint64_t segments_allocated = 0;  // partitions that own a slot array
    std::uint64_t segment_growths = 0;
    std::uint64_t segment_rebuilds = 0;
    std::uint64_t chunks_live = 0;
    std::uint64_t chunk_ids_issued = 0;
    std::uint64_t chunks_freed = 0;
    std::uint64_t chunk_ids_reused = 0;
    std::uint64_t chunk_bytes_reserved = 0;
    std::uint64_t value_bytes_live = 0;

    // Part B (rcu variant) only; all zero for the lock-based variants.
    std::uint64_t versions_published = 0;     // successful root publications
    std::uint64_t versions_retained = 0;      // replaced versions still held
    std::uint64_t versions_released = 0;
    std::uint64_t commits_failed = 0;         // commits that published nothing
    std::uint64_t segments_built = 0;         // replacement segments published (clones)
    std::uint64_t segments_released = 0;
    std::uint64_t segments_shared_with_previous = 0;  // of the current version, vs. the newest retained
    std::uint64_t index_bytes_copied = 0;     // slot bytes copied or rebuilt, cumulative
    std::uint64_t directory_bytes_copied = 0; // cumulative
    std::uint64_t owner_refs_taken = 0;       // segment references acquired, cumulative
    std::uint64_t chunk_pins_taken = 0;       // cumulative
    std::uint64_t value_bytes_appended = 0;   // cumulative
    std::uint64_t index_bytes_held = 0;       // versions + segments, current and retained
};

// One operation of an internal multi-key write (rcu variant; the public
// Transaction class is Part C).
struct NovaBatchOp {
    bool is_put;
    long key;
    const void* value;
    std::size_t length;
};

struct NovaKvInspector {
    static NovaKvStats stats(const KVStore& store);

    // Address of the store's lock object (meaningless for the `single`
    // variant). Lets a profiler's cache-line report be matched to the lock.
    // For the rcu variant: the address and size of the atomic root pointer.
    static const void* lock_address(const KVStore& store);
    static std::size_t lock_size();

    // ---- Part B (rcu variant). On other variants these return false / 0 / null.
    static bool is_versioned();
    // Applies all operations as one logical write with one publication.
    static bool commit_batch(KVStore& store, const NovaBatchOp* ops, std::size_t n);
    // Opaque handle on the version that is current now. In B1 it stays valid
    // until the store is destroyed, unless quiescent reclamation releases it.
    static const void* current_version(const KVStore& store);
    static std::uint64_t version_sequence(const void* version);
    // Identity of the segment a version uses for a partition (null if empty).
    static const void* version_segment(const void* version, unsigned partition);
    // get() through a specific version instead of the current one.
    static bool get_at(const KVStore& store, const void* version, long key, void* value_out,
                       std::size_t value_out_len);
    // CALLER PROMISES no other thread is reading the store (versioned_store.h).
    static void set_quiescent_reclaim(KVStore& store, bool enabled);
    static void set_default_quiescent_reclaim(bool enabled);
    static void release_retained_quiescent(KVStore& store);
    // Bytes held by versions, segments and chunks, current and retained. Cheap;
    // call from the writer thread or when quiescent. 0 on other variants.
    static std::uint64_t memory_held_bytes(const KVStore& store);

    // Walks the whole store and checks its structural invariants: slot counts,
    // load bound, each key in its own partition and reachable exactly once
    // through its probe chain, every ValueRef in bounds of a live chunk, and
    // each chunk's reference count equal to the entries pointing at it.
    // Returns nullptr if everything holds, else a description of the first
    // violation.
    static const char* validate(const KVStore& store);
};
