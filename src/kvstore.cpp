// kvstore.cpp -- NOVA-KV store implementation.
//
// MILESTONE A0 STATE: API-complete placeholder. There is no hash table yet, so
// the store is permanently empty: every mutation reports failure and every
// lookup reports "not found". This exists so the build, the Appendix A
// signatures and the unmodified supplied workload generator can be verified
// before any storage code is written. Milestone A1 replaces the Part A bodies.
#include "kvstore.h"

#include "build_info.h"
#include "sync_mode.h"

#include <new>

const char* nova_kv_sync_mode_name() noexcept { return NOVA_KV_SYNC_MODE_NAME; }

struct KVStore::Impl {};

KVStore::KVStore() : impl_(new Impl()) {}

KVStore::~KVStore() { delete impl_; }

// ---- Part A (not implemented until A1) -------------------------------------

bool KVStore::put(long key, const void* value, size_t value_len) {
    (void)key;
    (void)value;
    (void)value_len;
    return false;  // nothing is stored, so success must not be reported
}

bool KVStore::get(long key, void* value_out, size_t value_out_len) const {
    (void)key;
    (void)value_out;
    (void)value_out_len;
    return false;
}

bool KVStore::erase(long key) {
    (void)key;
    return false;
}

// ---- Part B (not implemented until B2) -------------------------------------

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
