// sync_mode.h -- build-time selection of the store's synchronization policy.
//
// The same kvstore.cpp is compiled once per policy (see CMakeLists.txt). The
// public kvstore.h is identical for every policy.
//
// The three Part A policies share one mutable index and differ only in the
// lock. The Part B policy (rcu) replaces the lock with immutable published
// versions (src/versioned_store.h); it uses the same hash, slot format, probing
// rules and chunked value storage, built copy-on-write instead of in place.
#pragma once

#define NOVA_KV_SYNC_SINGLE 1       // no internal synchronization (single-threaded driver)
#define NOVA_KV_SYNC_GLOBAL_LOCK 2  // one std::mutex around every operation
#define NOVA_KV_SYNC_RWLOCK 3       // std::shared_mutex: shared get, unique put/erase
#define NOVA_KV_SYNC_RCU 4          // Part B: immutable versions, atomic root, lock-free reads

#ifndef NOVA_KV_SYNC_MODE
#error "NOVA_KV_SYNC_MODE must be defined by the build (see CMakeLists.txt)"
#endif

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_SINGLE
#define NOVA_KV_SYNC_MODE_NAME "single"
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_GLOBAL_LOCK
#define NOVA_KV_SYNC_MODE_NAME "global_lock"
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RWLOCK
#define NOVA_KV_SYNC_MODE_NAME "rwlock"
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RCU
#define NOVA_KV_SYNC_MODE_NAME "rcu"
#else
#error "NOVA_KV_SYNC_MODE has an unknown value"
#endif
