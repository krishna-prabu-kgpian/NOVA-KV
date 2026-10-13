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
// Part C.1 (milestone C1): Transaction on every build; Snapshot, snapshot-scoped
// get() and keys() on the rcu build (docs/transactions.md). The lock-based
// baselines have no versions to pin, so acquire_snapshot() throws there.
//
// Part C.2 (milestone C2): the secondary index (create_index, both index_get
// overloads) on the rcu build, stored in the version and published with the
// data (docs/secondary_index.md). The lock-based baselines have no index:
// create_index() returns false there and index_get() finds nothing.
#include "kvstore.h"

#include "build_info.h"
#include "hash.h"
#include "kvstore_impl.h"
#include "sync_mode.h"

#include "alloc.h"
#include "config.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>

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

// Each successful call publishes exactly one version. The assignment assumes a
// single writer thread; if put, erase and Transaction::commit are nevertheless
// called from several threads, the store serializes them with an internal
// writer mutex (one at a time, each atomic). Readers never take that mutex.
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

// ---- Part C.1: transactions ---------------------------------------------------------

// The staged writes become one BatchOp array and go through the same commit
// path as a single put: private copies of the touched partitions (each cloned
// once), one new Version, one exchange of the root. The only allocation added
// here is that array; if it fails nothing has been built or published.
bool KVStore::Impl::commit_staged(const nova::StagedWrites& staged) noexcept {
    const std::size_t n = staged.count();
    nova::BatchOp* ops = static_cast<nova::BatchOp*>(
        nova::alloc_bytes(n * sizeof(nova::BatchOp), alignof(nova::BatchOp)));
    if (ops == nullptr) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const nova::StagedOp& o = staged.op(i);
        ops[i] = nova::BatchOp{nova::BatchOp::Kind::Put, o.key, staged.bytes(o), o.length};
    }
    const bool ok = store.commit(ops, n);
    nova::free_bytes(ops);
    return ok;
}

// ---- Part C.1: snapshots --------------------------------------------------------------
//
// A Snapshot is one long-lived reclamation pin (VersionedStore::Pin): a reader
// slot of its own that keeps announcing the epoch validated at acquisition, and
// the version loaded then. The handle stores the slot's address in pin_; the
// slot remembers the version. Moving a handle copies those two words and clears
// the source, so the slot is never written by a move and the protection is
// neither dropped nor duplicated. Destroying the handle (or assigning over it)
// clears the announcement and returns the slot, exactly once.

void KVStore::Snapshot::release() noexcept {
    if (pin_ != nullptr) {
        nova::VersionedStore::Pin pin =
            nova::VersionedStore::pin_of(static_cast<nova::ReaderSlot*>(pin_));
        owner_->impl_->store.release_pin(pin);
    }
    owner_ = nullptr;
    pin_ = nullptr;
}

// Throws std::bad_alloc if no reader slot can be allocated; it never returns a
// handle that does not protect its version.
KVStore::Snapshot KVStore::acquire_snapshot() const {
    const nova::VersionedStore::Pin pin = impl_->store.acquire_pin();
    return Snapshot(this, pin.slot);
}

namespace {
// The pin behind a handle, or an inert pin if the handle is moved-from or was
// acquired from another store.
nova::VersionedStore::Pin pin_for(const KVStore* store, const KVStore* owner, void* pin) noexcept {
    if (pin == nullptr || owner != store) return nova::VersionedStore::Pin{};
    return nova::VersionedStore::pin_of(static_cast<nova::ReaderSlot*>(pin));
}
}  // namespace

// Reads the version the snapshot is pinned to, never the current one. False for
// an absent key, for a buffer that is too small (buffer untouched), and for a
// handle that is moved-from or belongs to another store.
bool KVStore::get(const Snapshot& snap, long key,
                  void* value_out, size_t value_out_len) const {
    const nova::VersionedStore::Pin pin = pin_for(this, snap.owner_, snap.pin_);
    if (!pin) return false;
    return impl_->store.get_pinned(pin, key, value_out, value_out_len);
}

// Writes up to max_keys of the keys present in the snapshot's version to
// keys_out and returns how many were written. Each key appears once. The order
// is unspecified (it follows the hash layout) but is the same every time for
// the same snapshot. Returns 0 if keys_out is null, max_keys is 0, or the
// handle is moved-from or belongs to another store.
size_t KVStore::keys(const Snapshot& snap, long* keys_out, size_t max_keys) const {
    const nova::VersionedStore::Pin pin = pin_for(this, snap.owner_, snap.pin_);
    if (!pin) return 0;
    return impl_->store.keys_pinned(pin, keys_out, max_keys);
}

// ---- Part C.2: secondary index ----------------------------------------------------------
//
// One index per store, over one byte range of each value (secondary_index.h).
// It lives inside the version: a write publishes its primary changes and the
// matching index changes in the same single root swap, so no reader ever sees
// one without the other, and a Snapshot sees the index of its own version.

// Builds the index over the values already stored and publishes it, complete,
// in one publication. False (store unchanged) if field_len is 0, the range
// cannot fit in a value, memory runs out, or an index over a different range
// exists. The same range again: true, nothing published. Values too short to
// contain the range are simply not indexed. A writer operation.
bool KVStore::create_index(size_t field_offset, size_t field_len) {
    return impl_->store.create_index(field_offset, field_len);
}

// Primary keys of the values whose indexed bytes equal the field_len bytes at
// field_value (byte for byte; zero bytes are ordinary bytes), from the CURRENT
// version, loaded once. Ascending key order; at most max_keys are written, and
// if there are more the smallest max_keys are returned. Returns the number
// written; 0 if there is no index, field_len is not the index's field length,
// a pointer is null, or max_keys <= 0. No lock; like get(), it can throw
// std::bad_alloc on a thread's first read.
int KVStore::index_get(const void* field_value, size_t field_len,
                       long* keys_out, int max_keys) const {
    return impl_->store.index_get(field_value, field_len, keys_out, max_keys);
}

// The same lookup in the version the snapshot is pinned to, using THAT
// version's index. A moved-from handle, or one from another store, yields 0.
int KVStore::index_get(const Snapshot& snap, const void* field_value, size_t field_len,
                       long* keys_out, int max_keys) const {
    const nova::VersionedStore::Pin pin = pin_for(this, snap.owner_, snap.pin_);
    if (!pin) return 0;
    return impl_->store.index_get_pinned(pin, field_value, field_len, keys_out, max_keys);
}

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


// ---- Part C.1 on the lock-based baselines ---------------------------------------------

// A Transaction is supported: every staged write is applied under ONE exclusive
// hold of the store lock, so a concurrent reader sees none or all of them, and
// if any step fails the writes already applied are undone before the lock is
// released. Values that are replaced stay allocated until the whole batch has
// succeeded, which is what makes the undo possible.
bool KVStore::Impl::commit_staged(const nova::StagedWrites& staged) noexcept {
    struct Undo {
        long key;
        std::uint64_t h;
        bool replaced;
        ValueRef old_ref;
    };
    const std::size_t n = staged.count();
    Undo* undo = static_cast<Undo*>(nova::alloc_bytes(n * sizeof(Undo), alignof(Undo)));
    if (undo == nullptr) return false;

    WriteGuard guard(lock);
    std::size_t done = 0;
    for (; done < n; ++done) {
        const nova::StagedOp& o = staged.op(done);
        const std::uint64_t h = nova::hash_key(o.key);
        HashSegment& seg = segments[nova::partition_of(h)];
        HashSegment::UpsertPlan plan;
        if (!seg.prepare_upsert(o.key, h, plan)) break;
        ValueRef ref;
        if (!arena.append(staged.bytes(o), o.length, &ref)) {
            seg.abort_upsert(plan);
            break;
        }
        Undo& u = undo[done];
        u.key = o.key;
        u.h = h;
        u.replaced = seg.commit_upsert(o.key, h, ref, plan, &u.old_ref);
    }
    if (done == n) {
        // Nothing can fail any more: the replaced values may go.
        for (std::size_t i = 0; i < n; ++i) {
            if (undo[i].replaced) arena.release(undo[i].old_ref);
        }
        nova::free_bytes(undo);
        return true;
    }
    // Undo in reverse order. Restoring a value overwrites an existing key, and
    // removing a key needs no memory, so neither step can fail.
    while (done-- > 0) {
        const Undo& u = undo[done];
        HashSegment& seg = segments[nova::partition_of(u.h)];
        ValueRef staged_ref;
        if (u.replaced) {
            HashSegment::UpsertPlan plan;
            seg.prepare_upsert(u.key, u.h, plan);  // the key exists: no allocation
            seg.commit_upsert(u.key, u.h, u.old_ref, plan, &staged_ref);
        } else {
            seg.erase(u.key, u.h, &staged_ref);
        }
        arena.release(staged_ref);
    }
    nova::free_bytes(undo);
    return false;
}

// A Snapshot is NOT supported. These stores have one mutable state and no
// versions: the only way to hold a view still is to hold the lock, which would
// block the writer for the snapshot's lifetime (and deadlock a thread that then
// calls the store with the mutex variant). The assignment defines Snapshot on
// top of the Part B reclamation scheme. Rather than hand back a handle whose
// reads silently report "absent", acquisition fails loudly.
void KVStore::Snapshot::release() noexcept {
    owner_ = nullptr;
    pin_ = nullptr;
}

KVStore::Snapshot KVStore::acquire_snapshot() const {
    throw std::logic_error("KVStore::acquire_snapshot: snapshots need the rcu build of NOVA-KV; "
                           "the lock-based baseline stores have no versions to pin");
}

bool KVStore::get(const Snapshot& snap, long key,
                  void* value_out, size_t value_out_len) const {
    (void)snap;  // no valid handle can exist in this build
    (void)key;
    (void)value_out;
    (void)value_out_len;
    return false;
}

size_t KVStore::keys(const Snapshot& snap, long* keys_out, size_t max_keys) const {
    (void)snap;
    (void)keys_out;
    (void)max_keys;
    return 0;
}

// A secondary index is NOT supported on the lock-based baselines either. The
// Part C index is defined as part of a published version, updated in the same
// pointer swap as the data; these stores have no versions. create_index()
// reports failure and the lookups find nothing: "this store has no index".
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

int KVStore::index_get(const Snapshot& snap, const void* field_value, size_t field_len,
                       long* keys_out, int max_keys) const {
    (void)snap;
    (void)field_value;
    (void)field_len;
    (void)keys_out;
    (void)max_keys;
    return 0;
}

#endif  // NOVA_KV_SYNC_MODE

// ---- Snapshot handle: the parts common to every build ----------------------------------

KVStore::Snapshot::Snapshot(const KVStore* owner, void* pin) noexcept
    : owner_(owner), pin_(pin) {}

// A move transfers the pin: the source becomes inert and its destructor does
// nothing. The pin itself (the reader slot) is not touched.
KVStore::Snapshot::Snapshot(Snapshot&& other) noexcept
    : owner_(other.owner_), pin_(other.pin_) {
    other.owner_ = nullptr;
    other.pin_ = nullptr;
}

KVStore::Snapshot& KVStore::Snapshot::operator=(Snapshot&& other) noexcept {
    if (this != &other) {
        release();  // the pin this handle held until now, if any
        owner_ = other.owner_;
        pin_ = other.pin_;
        other.owner_ = nullptr;
        other.pin_ = nullptr;
    }
    return *this;
}

KVStore::Snapshot::~Snapshot() { release(); }

// ---- Transaction (Part C.1) ----------------------------------------------------------
//
// put() stages; commit() publishes everything staged as one logical write.
//
//   - put() copies the bytes immediately. It returns void, so anything that
//     goes wrong while staging (no memory, null value with a length, a value
//     above the size limit) is remembered and makes commit() return false.
//   - Several puts to one key: the last one wins.
//   - commit() can be called once. It returns true if every staged write was
//     published (in one publication), or if nothing was staged (nothing is
//     published). It returns false, with the store unchanged, if staging or the
//     commit failed. After commit() the Transaction is finished: further puts
//     are ignored and further commits return false and publish nothing.
//   - Destroying a Transaction that was not committed discards its writes.

struct Transaction::Impl {
    nova::StagedWrites staged;
    bool failed = false;    // a put() could not be staged
    bool finished = false;  // commit() has been called
};

Transaction::Transaction(KVStore& store) : store_(store), impl_(nullptr) {
    // If even this small object cannot be allocated the Transaction is created
    // in the failed state: puts are ignored and commit() returns false.
    void* raw = nova::alloc_bytes(sizeof(Impl), alignof(Impl));
    if (raw != nullptr) impl_ = new (raw) Impl();
}

Transaction::~Transaction() {
    if (impl_ == nullptr) return;
    impl_->~Impl();  // frees whatever is still staged
    nova::free_bytes(impl_);
}

void Transaction::put(long key, const void* value, size_t value_len) {
    if (impl_ == nullptr || impl_->finished || impl_->failed) return;
    const bool valid = value_len <= nova::kMaxValueBytes && (value_len == 0 || value != nullptr);
    if (!valid || !impl_->staged.put(key, value, static_cast<std::uint32_t>(value_len))) {
        impl_->failed = true;
        impl_->staged.clear();  // nothing of this transaction will ever be published
    }
}

bool Transaction::commit() {
    if (impl_ == nullptr || impl_->finished) return false;
    impl_->finished = true;
    bool ok = false;
    if (!impl_->failed) {
        ok = impl_->staged.count() == 0 || store_.impl_->commit_staged(impl_->staged);
    }
    impl_->staged.clear();
    return ok;
}
