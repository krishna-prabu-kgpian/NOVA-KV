// kvstore_impl.h -- private state of KVStore (shared by kvstore.cpp and the
// test/statistics inspector). Not part of the public API.
#pragma once

#include "kvstore.h"

#include "sync_mode.h"

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RCU

#include "versioned_store.h"

// Part B: immutable published versions behind one atomic root pointer.
struct KVStore::Impl {
    // The engine reads the reclaim batch from KVStore's own private member.
    explicit Impl(std::atomic<int>* reclaim_batch)
        : store(nova::default_store_options(), reclaim_batch) {}
    nova::VersionedStore store;
};

#else

#include "arena.h"
#include "config.h"
#include "hash_segment.h"
#include "sync_policy.h"

// Part A: one mutable index and arena, guarded by the variant's lock.
struct KVStore::Impl {
    nova::StoreLock lock;                             // guards everything below
    nova::HashSegment segments[nova::kPartitionCount];  // primary index
    nova::Arena arena;                                // value bytes
};

#endif
