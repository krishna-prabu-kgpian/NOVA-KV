// kvstore.cpp -- NOVA-KV store implementation.
//
// MILESTONE A1 STATE: Part A baseline. put / get / erase are implemented on a
// segmented linear-probing hash index (hash_segment.h) whose entries reference
// value bytes in a chunked arena (arena.h). Segments are mutated in place under
// the build's synchronization policy (sync_policy.h); that policy is the only
// difference between the single / global_lock / rwlock libraries.
//
// Parts B and C are declared in kvstore.h for source compatibility with the
// supplied workload generator but are NOT implemented: their bodies below are
// inert and never report success.
#include "kvstore.h"

#include "build_info.h"
#include "hash.h"
#include "kvstore_impl.h"
#include "sync_mode.h"

#include <cstdint>
#include <cstring>

using nova::HashSegment;
using nova::ReadGuard;
using nova::ValueRef;
using nova::WriteGuard;

const char* nova_kv_sync_mode_name() noexcept { return NOVA_KV_SYNC_MODE_NAME; }

KVStore::KVStore() : impl_(new Impl()) {}

KVStore::~KVStore() { delete impl_; }

// ---- Part A -------------------------------------------------------------------

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

// ---- Part B (not implemented until B2) ----------------------------------------

void KVStore::set_reclaim_batch(int n) { (void)n; }

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
