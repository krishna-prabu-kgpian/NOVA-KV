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
    std::uint64_t versions_retired = 0;       // handed to the retirement queue
    std::uint64_t versions_reclaimed = 0;     // completely destroyed
    std::uint64_t versions_retained = 0;      // backlog: retired, not yet destroyed
    std::uint64_t backlog_max = 0;            // largest backlog the writer saw
    std::uint64_t commits_failed = 0;         // commits that published nothing
    std::uint64_t segments_built = 0;         // replacement segments published (clones)
    std::uint64_t segments_released = 0;
    std::uint64_t segments_shared = 0;        // of the current version, also owned by a retired one
    std::uint64_t index_bytes_copied = 0;     // slot bytes copied or rebuilt, cumulative
    std::uint64_t directory_bytes_copied = 0; // cumulative
    std::uint64_t owner_refs_taken = 0;       // segment references acquired, cumulative
    std::uint64_t chunk_refs_taken = 0;       // by segments, cumulative
    std::uint64_t chunk_refs_dropped = 0;
    std::uint64_t value_bytes_appended = 0;   // cumulative
    std::uint64_t index_bytes_held = 0;       // versions + segments, current and retired
    // Epoch-based reclamation
    std::uint64_t epoch = 0;
    std::uint64_t epoch_advance_attempts = 0;
    std::uint64_t epoch_advances = 0;
    std::uint64_t epoch_advances_blocked = 0; // attempts stopped by a participant in an older epoch
    std::uint64_t reader_slots = 0;           // slots that exist
    std::uint64_t reader_slots_owned = 0;
    std::uint64_t reader_registrations = 0;
    std::uint64_t reader_validation_retries = 0;
    std::uint64_t reclaim_passes = 0;
    std::uint64_t reclaim_passes_with_work = 0;
    std::uint64_t reclaim_max_versions_in_pass = 0;
    std::uint64_t reclaim_max_units_in_pass = 0;
    std::uint64_t reclaim_interrupted_versions = 0;
    std::uint64_t stw_pauses = 0;
    std::uint64_t cleaner_cpu_ns = 0;         // background cleaner CPU time, up to its last stop
    int reclaim_batch = 0;
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
    // Quiet store (writer not running) or the writer's own thread.
    static NovaKvStats stats(const KVStore& store);

    // Quiet store required (no writer running). For the rcu variant the
    // background cleaner is stopped for the duration and restarted afterwards.
    //
    // Walks the whole store and checks its structural invariants: slot counts,
    // load bound, each key in its own partition and reachable exactly once
    // through its probe chain, every ValueRef in bounds of a live chunk, and
    // every ownership count equal to what the existing objects imply.
    // Returns nullptr if everything holds, else a description of the first
    // violation.
    static const char* validate(KVStore& store);

    // Address of the store's lock object (meaningless for the `single`
    // variant). Lets a profiler's cache-line report be matched to the lock.
    // For the rcu variant: the address and size of the atomic root pointer.
    static const void* lock_address(const KVStore& store);
    static std::size_t lock_size();

    // ---- Part B (rcu variant). On the other variants these do nothing and
    // return false / 0.
    static bool is_versioned();
    // Applies all operations as one logical write with one publication.
    static bool commit_batch(KVStore& store, const NovaBatchOp* ops, std::size_t n);
    // Process-wide options for stores constructed from now on.
    //   reclaim: 0 epoch-based (default), 1 disabled (comparator), 2 stop-the-world (comparator)
    //   cleaner: 0 background thread (default), 1 manual, 2 inline in the writer
    static void set_default_rcu_options(int reclaim, int cleaner);
    // Stops the background cleaner (if any), runs reclamation passes on the
    // calling thread until nothing is retired or no pass makes progress, then
    // restarts the cleaner. Returns true if the backlog is empty. The caller
    // must not be racing with the store's writer.
    static bool settle(KVStore& store);
    // Bytes held by versions, segments and chunks, current and retired. Cheap;
    // call from the writer thread or on a quiet store. 0 on other variants.
    static std::uint64_t memory_held_bytes(const KVStore& store);
    // Retired versions not yet destroyed. Any thread, any time.
    static std::uint64_t backlog(const KVStore& store);
    // Work units per version a pass may spend (see docs/reclamation.md).
    static void set_reclaim_work_per_version(KVStore& store, unsigned units);
};
