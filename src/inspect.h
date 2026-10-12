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
};

struct NovaKvInspector {
    static NovaKvStats stats(const KVStore& store);

    // Address of the store's lock object (meaningless for the `single`
    // variant). Lets a profiler's cache-line report be matched to the lock.
    static const void* lock_address(const KVStore& store);
    static std::size_t lock_size();

    // Walks the whole store and checks its structural invariants: slot counts,
    // load bound, each key in its own partition and reachable exactly once
    // through its probe chain, every ValueRef in bounds of a live chunk, and
    // each chunk's reference count equal to the entries pointing at it.
    // Returns nullptr if everything holds, else a description of the first
    // violation.
    static const char* validate(const KVStore& store);
};
