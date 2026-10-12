// kvstore_impl.h -- private state of KVStore (shared by kvstore.cpp and the
// test/statistics inspector). Not part of the public API.
#pragma once

#include "kvstore.h"

#include "arena.h"
#include "config.h"
#include "hash_segment.h"
#include "sync_policy.h"

struct KVStore::Impl {
    nova::StoreLock lock;                             // guards everything below
    nova::HashSegment segments[nova::kPartitionCount];  // primary index
    nova::Arena arena;                                // value bytes
};
