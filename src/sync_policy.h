// sync_policy.h -- the only code that differs between the Part A variants.
//
//   single       no lock; the caller guarantees one thread at a time
//   global_lock  one std::mutex held for every put / get / erase
//   rwlock       std::shared_mutex: get shared, put / erase exclusive
#pragma once

#include "sync_mode.h"

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_GLOBAL_LOCK
#include <mutex>
#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RWLOCK
#include <mutex>
#include <shared_mutex>
#endif

namespace nova {

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_SINGLE

struct StoreLock {};
struct ReadGuard {
    explicit ReadGuard(StoreLock&) noexcept {}
};
struct WriteGuard {
    explicit WriteGuard(StoreLock&) noexcept {}
};

#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_GLOBAL_LOCK

using StoreLock = std::mutex;
using ReadGuard = std::lock_guard<std::mutex>;
using WriteGuard = std::lock_guard<std::mutex>;

#elif NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RWLOCK

using StoreLock = std::shared_mutex;
using ReadGuard = std::shared_lock<std::shared_mutex>;
using WriteGuard = std::unique_lock<std::shared_mutex>;

#endif

}  // namespace nova
