// sync_mode.h -- build-time selection of the store's synchronization policy.
//
// The same kvstore.cpp is compiled once per policy (see CMakeLists.txt); only
// the synchronization differs between the resulting libraries, never the data
// structure. The public kvstore.h is identical for every policy.
#pragma once

#define NOVA_KV_SYNC_SINGLE 1       // no internal synchronization (single-threaded driver)
#define NOVA_KV_SYNC_GLOBAL_LOCK 2  // one std::mutex around every operation
#define NOVA_KV_SYNC_RWLOCK 3       // std::shared_mutex: shared get, unique put/erase

#ifndef NOVA_KV_SYNC_MODE
#error "NOVA_KV_SYNC_MODE must be defined by the build (see CMakeLists.txt)"
#endif

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_SINGLE
#define NOVA_KV_SYNC_MODE_NAME "single"
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_GLOBAL_LOCK
#define NOVA_KV_SYNC_MODE_NAME "global_lock"
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RWLOCK
#define NOVA_KV_SYNC_MODE_NAME "rwlock"
#else
#error "NOVA_KV_SYNC_MODE has an unknown value"
#endif
