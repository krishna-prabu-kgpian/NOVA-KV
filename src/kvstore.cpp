// kvstore.cpp -- NOVA-KV store implementation.
//
// One KVStore class, compiled once per synchronization policy (sync_mode.h):
//
//   single / global_lock / rwlock   Part A baseline: a segmented linear-probing
//       hash index (hash_segment.h) over a chunked value arena (arena.h),
//       mutated in place under the policy's lock (sync_policy.h).
//   rcu                             Part B: the same hash, slot format, probing
//       rules and chunked values, but every write builds new state privately
//       and publishes an immutable version with one atomic pointer swap; reads
//       take no lock (versioned_store.h).
//
// Part B (rcu) is complete as of milestone B2: replaced versions are reclaimed
// by epoch-based reclamation on a background cleaner thread, in passes bounded
// by set_reclaim_batch() (docs/reclamation.md).
//
// Part C (Transaction, Snapshot, secondary index) is declared in kvstore.h for
// source compatibility with the supplied workload generator but is NOT
// implemented: those bodies are inert and never report success.
#include "kvstore.h"

#include "build_info.h"
#include "hash.h"
#include "kvstore_impl.h"
#include "sync_mode.h"

#include <cstdint>
#include <cstring>
#include <new>

const char* nova_kv_sync_mode_name() noexcept { return NOVA_KV_SYNC_MODE_NAME; }

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RCU

// ---- Part B: RCU ------------------------------------------------------------------

KVStore::KVStore() : reclaim_batch_(nova::kDefaultReclaimBatch), impl_(new Impl(&reclaim_batch_)) {
    if (!impl_->store.ok()) {  // initial version, queue or cleaner thread could not be created
        delete impl_;
        throw std::bad_alloc();
    }
}

KVStore::~KVStore() { delete impl_; }

// Single writer (the assignment's assumption): put and erase must not be called
// from two threads at once. Each successful call publishes exactly one version.
bool KVStore::put(long key, const void* value, size_t value_len) {
    return impl_->store.put(key, value, value_len);
}

// Any number of threads, concurrently with the writer: announce an epoch, one
// atomic load of the current version, no lock, no waiting, bytes copied out
// before returning. Resource exhaustion is never reported as "not found": the
// only allocation a read can need is its thread's reader slot, on that thread's
// first read of this store, and if that fails std::bad_alloc is thrown.
bool KVStore::get(long key, void* value_out, size_t value_out_len) const {
    return impl_->store.get(key, value_out, value_out_len);
}

// Returns false if the key is absent. It also returns false, with the store
// unchanged, if memory for the replacement version cannot be allocated.
bool KVStore::erase(long key) { return impl_->store.erase(key); }

// How many retired versions one reclamation pass may completely free. A pass
// is one attempt to advance the reclamation epoch followed by the destruction
// of versions whose grace period has passed. n <= 0 is treated as 1. Takes
// effect from the next pass; never blocks a reader or the writer. The value is
// kept in the private member reclaim_batch_ (kvstore.h), which is the variable
// every reclamation pass reads.
void KVStore::set_reclaim_batch(int n) { reclaim_batch_.store(n <= 0 ? 1 : n); }

#else

// ---- Part A: lock-based baseline ---------------------------------------------------

using nova::HashSegment;
using nova::ReadGuard;
using nova::ValueRef;
using nova::WriteGuard;

KVStore::KVStore() : reclaim_batch_(nova::kDefaultReclaimBatch), impl_(new Impl()) {}

KVStore::~KVStore() { delete impl_; }

bool KVStore::put(long key, const void* value, size_t value_len) {
    if (value_len > nova::kMaxValueBytes) return false;
    if (value_len != 0 && value == nullptr) return false;
    const std::uint32_t len = static_cast<std::uint32_t>(value_len);
    const std::uint64_t h = nova::hash_key(key);
    Impl& impl = *impl_;
    HashSegment& seg = impl.segments[nova::partition_of(h)];

    WriteGuard guard(impl.lock);

    // Every step that can fail runs before the first change to the index:
    // 1. find the slot, building a private replacement array if the segment
    //    must grow or shed tombstones;
    // 2. copy the value into the arena (the bytes are unreachable until an
    //    index entry points at them).
    HashSegment::UpsertPlan plan;
    if (!seg.prepare_upsert(key, h, plan)) return false;
    ValueRef ref;
    if (!impl.arena.append(value, len, &ref)) {
        seg.abort_upsert(plan);
        return false;
    }

    // From here on nothing can fail.
    ValueRef old_ref;
    if (seg.commit_upsert(key, h, ref, plan, &old_ref)) impl.arena.release(old_ref);
    return true;
}

bool KVStore::get(long key, void* value_out, size_t value_out_len) const {
    const std::uint64_t h = nova::hash_key(key);
    Impl& impl = *impl_;
    const HashSegment& seg = impl.segments[nova::partition_of(h)];

    ReadGuard guard(impl.lock);

    const nova::Entry* e = seg.find(key, h);
    if (e == nullptr) return false;
    const std::uint32_t len = e->value.length;
    if (len > value_out_len) return false;  // never a truncated copy; buffer untouched
    if (len == 0) return true;              // present, empty value
    if (value_out == nullptr) return false;
    // Copy out while the lock is held: no internal pointer leaves the store.
    std::memcpy(value_out, impl.arena.data(e->value), len);
    return true;
}

bool KVStore::erase(long key) {
    const std::uint64_t h = nova::hash_key(key);
    Impl& impl = *impl_;
    HashSegment& seg = impl.segments[nova::partition_of(h)];

    WriteGuard guard(impl.lock);

    ValueRef old_ref;
    if (!seg.erase(key, h, &old_ref)) return false;
    impl.arena.release(old_ref);
    return true;
}

// The lock-based baselines retire nothing, so there is nothing to reclaim: the
// setting is recorded and has no effect.
void KVStore::set_reclaim_batch(int n) { reclaim_batch_.store(n <= 0 ? 1 : n); }

#endif  // NOVA_KV_SYNC_MODE

// ---- Part C (not implemented until C1/C2) ----------------------------------

bool KVStore::create_index(size_t field_offset, size_t field_len) {
    (void)field_offset;
    (void)field_len;
    return false;
}

int KVStore::index_get(const void* field_value, size_t field_len,
                       long* keys_out, int max_keys) const {
    (void)field_value;
    (void)field_len;
    (void)keys_out;
    (void)max_keys;
    return 0;
}

KVStore::Snapshot::Snapshot(const KVStore* owner, void* pin) noexcept
    : owner_(owner), pin_(pin) {}

KVStore::Snapshot::Snapshot(Snapshot&& other) noexcept
    : owner_(other.owner_), pin_(other.pin_) {
    other.owner_ = nullptr;
    other.pin_ = nullptr;
}

KVStore::Snapshot& KVStore::Snapshot::operator=(Snapshot&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        pin_ = other.pin_;
        other.owner_ = nullptr;
        other.pin_ = nullptr;
    }
    return *this;
}

KVStore::Snapshot::~Snapshot() { release(); }

void KVStore::Snapshot::release() noexcept {
    // No reclamation scheme exists yet, so there is no pin to drop.
    owner_ = nullptr;
    pin_ = nullptr;
}

KVStore::Snapshot KVStore::acquire_snapshot() const {
    // Inert handle: with no published versions there is nothing to pin.
    return Snapshot(this, nullptr);
}

bool KVStore::get(const Snapshot& snap, long key,
                  void* value_out, size_t value_out_len) const {
    (void)snap;
    (void)key;
    (void)value_out;
    (void)value_out_len;
    return false;
}

int KVStore::index_get(const Snapshot& snap, const void* field_value, size_t field_len,
                       long* keys_out, int max_keys) const {
    (void)snap;
    (void)field_value;
    (void)field_len;
    (void)keys_out;
    (void)max_keys;
    return 0;
}

size_t KVStore::keys(const Snapshot& snap, long* keys_out, size_t max_keys) const {
    (void)snap;
    (void)keys_out;
    (void)max_keys;
    return 0;
}

struct Transaction::Impl {};

Transaction::Transaction(KVStore& store) : store_(store), impl_(nullptr) {}

Transaction::~Transaction() { delete impl_; }

void Transaction::put(long key, const void* value, size_t value_len) {
    (void)key;
    (void)value;
    (void)value_len;
}

bool Transaction::commit() {
    (void)store_;
    return false;  // nothing is published, so success must not be reported
}
