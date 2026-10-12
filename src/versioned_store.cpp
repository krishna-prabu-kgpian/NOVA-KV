#include "versioned_store.h"

#include "alloc.h"
#include "test_points.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <vector>

namespace nova {

namespace {

std::atomic<std::uint8_t> g_default_reclaim{static_cast<std::uint8_t>(ReclaimMode::Epoch)};
std::atomic<std::uint8_t> g_default_cleaner{static_cast<std::uint8_t>(CleanerMode::Background)};

// How long the background cleaner sleeps when a pass found nothing to do.
constexpr std::chrono::microseconds kCleanerIdleSleep{200};
// StopTheWorld comparator: free the retire list once it is at least this long.
constexpr std::uint64_t kStwBacklogTrigger = 1024;

std::uint64_t segment_bytes(const Segment* s) noexcept {
    return sizeof(Segment) + sizeof(ChunkPin) * std::uint64_t{s->pin_count} +
           sizeof(Entry) * std::uint64_t{s->capacity};
}

// The builders of one commit: one per touched partition. A single-key write
// uses the inline builder; larger batches allocate an array.
class BuilderSet {
public:
    BuilderSet() noexcept : heap_(nullptr), count_(0), constructed_(0) {}
    ~BuilderSet() {
        for (std::uint32_t i = 0; i < constructed_; ++i) at(i).~SegmentBuilder();
        free_bytes(heap_);
    }
    bool init(std::uint32_t count) noexcept {
        count_ = count;
        if (count > 1) {
            heap_ = static_cast<SegmentBuilder*>(
                alloc_bytes(sizeof(SegmentBuilder) * count, alignof(SegmentBuilder)));
            if (heap_ == nullptr) return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            new (&at(i)) SegmentBuilder();
            ++constructed_;
        }
        return true;
    }
    SegmentBuilder& at(std::uint32_t i) noexcept {
        return heap_ != nullptr ? heap_[i] : *reinterpret_cast<SegmentBuilder*>(inline_);
    }

private:
    alignas(SegmentBuilder) unsigned char inline_[sizeof(SegmentBuilder)];
    SegmentBuilder* heap_;
    std::uint32_t count_;
    std::uint32_t constructed_;
};

void raise_max(std::atomic<std::uint64_t>& a, std::uint64_t v) noexcept {
    std::uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

}  // namespace

void set_default_store_options(const StoreOptions& options) noexcept {
    g_default_reclaim.store(static_cast<std::uint8_t>(options.reclaim));
    g_default_cleaner.store(static_cast<std::uint8_t>(options.cleaner));
}

StoreOptions default_store_options() noexcept {
    StoreOptions o;
    o.reclaim = static_cast<ReclaimMode>(g_default_reclaim.load());
    o.cleaner = static_cast<CleanerMode>(g_default_cleaner.load());
    return o;
}

VersionedStore::VersionedStore() : VersionedStore(default_store_options()) {}

VersionedStore::VersionedStore(const StoreOptions& options, std::atomic<int>* reclaim_batch)
    : options_(options),
      current_(nullptr),
      latest_(nullptr),
      queue_tail_(nullptr),
      queue_head_(nullptr),
      reclaim_batch_(reclaim_batch != nullptr ? reclaim_batch : &own_reclaim_batch_) {
    if (!epochs_.ok()) return;
    // Readers never see a null root: an empty version exists from the start.
    void* raw = alloc_bytes(sizeof(Version), alignof(Version));
    void* dummy = alloc_bytes(sizeof(RetireNode), alignof(RetireNode));
    if (raw == nullptr || dummy == nullptr) {
        free_bytes(raw);
        free_bytes(dummy);
        return;
    }
    Version* v = new (raw) Version();
    NOVA_COUNT_OBJECT(LiveObject::Version, 1);
    v->sequence = 0;
    for (Segment*& s : v->segments) s = nullptr;
    latest_ = v;
    w_.index_bytes_added = sizeof(Version);
    // The retirement queue always holds one dummy node; the writer appends
    // after the tail, the cleaner consumes after the head.
    RetireNode* d = new (dummy) RetireNode();
    d->version = nullptr;
    d->retire_epoch = 0;
    d->next.store(nullptr, std::memory_order_relaxed);
    queue_head_ = d;
    queue_tail_ = d;
    current_.store(v, std::memory_order_seq_cst);
    ok_ = true;
    if (options_.cleaner == CleanerMode::Background && !start_cleaner()) ok_ = false;
}

VersionedStore::~VersionedStore() {
    // Shutdown contract (blueprint 2.2, 15.4): every reader and the writer have
    // stopped and every Pin has been released before the store is destroyed.
    stop_cleaner();          // 1. no other thread runs passes from here on
    destroy_everything();    // 2. this thread releases the whole ownership graph
    // 3. epochs_ unregisters from the live-domain table before freeing its
    //    slots, so a thread exiting later cannot touch them; arena_ frees the
    //    writable chunk.
}

void VersionedStore::destroy_everything() noexcept {
    if (latest_ == nullptr) return;  // construction failed early
    std::uint64_t unlimited = ~std::uint64_t{0};
    if (job_.version != nullptr) {
        run_job(unlimited);
        reclaimed_.fetch_add(1, std::memory_order_seq_cst);
    }
    // Every queued version, whatever its epoch: no reader exists any more.
    for (;;) {
        RetireNode* next = queue_head_->next.load(std::memory_order_seq_cst);
        if (next == nullptr) break;
        free_bytes(queue_head_);
        queue_head_ = next;
        job_ = Job{};
        job_.version = next->version;
        unlimited = ~std::uint64_t{0};
        run_job(unlimited);
        reclaimed_.fetch_add(1, std::memory_order_seq_cst);
    }
    free_bytes(queue_head_);
    queue_head_ = nullptr;
    // The current version.
    job_ = Job{};
    job_.version = latest_;
    unlimited = ~std::uint64_t{0};
    run_job(unlimited);
    latest_ = nullptr;
    current_.store(nullptr, std::memory_order_seq_cst);
}

// ---- Read path ---------------------------------------------------------------------

bool VersionedStore::read(const Version* v, long key, std::uint64_t h, void* value_out,
                          std::size_t value_out_len) const noexcept {
    const Entry* e = segment_find(v->segments[partition_of(h)], key, h);
    if (e == nullptr) return false;
    const std::uint32_t len = e->value.length;
    if (len > value_out_len) return false;  // never a truncated copy; buffer untouched
    if (len == 0) return true;              // present, empty value
    if (value_out == nullptr) return false;
    NOVA_TEST_POINT(TestPoint::ReaderBeforeCopyOut);
    // The segment holds a reference on the chunk, so the directory slot is
    // non-null and the bytes are immutable. Copy out while still protected; no
    // internal pointer leaves the store.
    std::memcpy(value_out, arena_.data(e->value), len);
    return true;
}

bool VersionedStore::get(long key, void* value_out, std::size_t value_out_len) const {
    const std::uint64_t h = hash_key(key);
    // StopTheWorld comparator only (an immutable flag, never set by default).
    std::shared_lock<std::shared_mutex> stw(stw_lock_, std::defer_lock);
    if (options_.reclaim == ReclaimMode::StopTheWorld) stw.lock();

    // This thread's slot: a thread-local lookup, except on the thread's first
    // read of this store, when it is registered (may throw std::bad_alloc).
    ReaderSlot* slot = epochs_.thread_slot();
    epochs_.pin(slot);                                                   // [P1]-[P3]
    // Protected from here. The one and only load of the root in this
    // operation; seq_cst, so it is ordered after the writer's exchange that
    // published this version and after this reader's validated announcement.
    const Version* v = current_.load(std::memory_order_seq_cst);        // [P4]
    NOVA_TEST_POINT(TestPoint::ReaderAfterRootLoad);
    const bool found = read(v, key, h, value_out, value_out_len);
    epochs_.unpin(slot);                                                 // [P5]
    return found;
}

VersionedStore::Pin VersionedStore::acquire_pin() const {
    Pin pin;
    pin.slot = epochs_.acquire_slot();  // dedicated slot; may throw before anything is pinned
    epochs_.pin(pin.slot);
    pin.version = current_.load(std::memory_order_seq_cst);  // one load, after validation
    pin.slot->token = pin.version;
    NOVA_TEST_POINT(TestPoint::ReaderAfterRootLoad);
    return pin;
}

VersionedStore::Pin VersionedStore::pin_of(ReaderSlot* slot) noexcept {
    Pin pin;
    pin.slot = slot;
    pin.version = slot != nullptr ? static_cast<const Version*>(slot->token) : nullptr;
    return pin;
}

bool VersionedStore::get_pinned(const Pin& pin, long key, void* value_out,
                                std::size_t value_out_len) const noexcept {
    // No new announcement and no root load: the pin's slot has been announcing
    // one epoch since acquire_pin(), and the version is the one loaded then.
    // (StopTheWorld comparator only: this call is frozen by a sweep like any
    // other read. The pin, not this lock, is what keeps the version alive.)
    std::shared_lock<std::shared_mutex> stw(stw_lock_, std::defer_lock);
    if (options_.reclaim == ReclaimMode::StopTheWorld) stw.lock();
    return read(pin.version, key, hash_key(key), value_out, value_out_len);
}

std::size_t VersionedStore::keys_pinned(const Pin& pin, long* keys_out,
                                        std::size_t max_keys) const noexcept {
    if (keys_out == nullptr || max_keys == 0) return 0;
    std::shared_lock<std::shared_mutex> stw(stw_lock_, std::defer_lock);
    if (options_.reclaim == ReclaimMode::StopTheWorld) stw.lock();
    // Partition by partition, slot by slot. A key occupies exactly one slot of
    // exactly one segment of a version, so no key is reported twice; tombstones
    // and empty slots are skipped.
    std::size_t n = 0;
    for (const Segment* seg : pin.version->segments) {
        if (seg == nullptr) continue;
        for (std::uint32_t i = 0; i < seg->capacity; ++i) {
            if (seg->slots[i].state != SlotState::Occupied) continue;
            if (n == max_keys) return n;
            keys_out[n++] = seg->slots[i].key;
        }
    }
    return n;
}

std::size_t VersionedStore::size_pinned(const Pin& pin) const noexcept {
    std::size_t n = 0;
    for (const Segment* seg : pin.version->segments) {
        if (seg != nullptr) n += seg->live;
    }
    return n;
}

void VersionedStore::release_pin(Pin& pin) const noexcept {
    if (pin.slot == nullptr) return;
    pin.slot->token = nullptr;
    epochs_.unpin(pin.slot);
    epochs_.release_slot(pin.slot);
    pin = Pin{};
}

bool VersionedStore::get_at_unprotected(const Version* v, long key, void* value_out,
                                        std::size_t value_out_len) const noexcept {
    return read(v, key, hash_key(key), value_out, value_out_len);
}

// ---- Write path --------------------------------------------------------------------

bool VersionedStore::put(long key, const void* value, std::size_t value_len) noexcept {
    const BatchOp op{BatchOp::Kind::Put, key, value, value_len};
    return commit(&op, 1);
}

bool VersionedStore::erase(long key) noexcept {
    // An absent key is reported without building or publishing anything. The
    // writer may read the current version freely: it cannot be retired except
    // by the writer itself.
    const std::uint64_t h = hash_key(key);
    std::lock_guard<std::mutex> writer(writer_mutex_);
    if (segment_find(latest_->segments[partition_of(h)], key, h) == nullptr) return false;
    const BatchOp op{BatchOp::Kind::Erase, key, nullptr, 0};
    bool applied = false;
    return commit_locked(&op, 1, &applied) && applied;
}

bool VersionedStore::commit(const BatchOp* ops, std::size_t n, bool* applied) noexcept {
    if (n == 0) return true;
    std::lock_guard<std::mutex> writer(writer_mutex_);
    return commit_locked(ops, n, applied);
}

bool VersionedStore::commit_locked(const BatchOp* ops, std::size_t n, bool* applied) noexcept {
    if (n > 0xFFFFFFFFu) return false;

    // ---- Phase 0: validate every argument before anything is built. ----------
    for (std::size_t i = 0; i < n; ++i) {
        if (ops[i].kind != BatchOp::Kind::Put) continue;
        if (ops[i].length > kMaxValueBytes) return false;
        if (ops[i].length != 0 && ops[i].value == nullptr) return false;
    }

    // The current version. Only the writer retires it, and only at the end of
    // this function, so it and every segment and chunk it references stay
    // alive for the whole commit whatever the cleaner does.
    Version* const base = latest_;

    // ---- Phase 1: group by partition; one builder per touched partition. -----
    std::uint16_t builder_of[kPartitionCount];
    std::uint32_t ops_in[kPartitionCount];
    std::memset(builder_of, 0xFF, sizeof(builder_of));
    std::uint32_t touched = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t p = partition_of(hash_key(ops[i].key));
        if (builder_of[p] == 0xFFFF) {
            builder_of[p] = static_cast<std::uint16_t>(touched);
            ops_in[touched] = 0;
            ++touched;
        }
        ++ops_in[builder_of[p]];
    }

    BuilderSet builders;
    Version* next = nullptr;
    RetireNode* node = nullptr;
    bool arena_open = false;

    // Everything from here to "nothing can fail" works on private memory, plus
    // arena appends that are covered by writable / commit references. On any
    // failure: roll the arena back (which releases those references), let the
    // builders free what they allocated, and report failure. No segment owner
    // count or segment chunk reference has been touched and `current_` has not
    // been written.
    auto fail = [&]() {
        if (arena_open) arena_.rollback_commit();
        free_bytes(next);
        free_bytes(node);
        ++w_.commits_failed;
        return false;
    };

    if (!builders.init(touched)) return fail();
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (builder_of[p] == 0xFFFF) continue;
        if (!builders.at(builder_of[p]).begin(base->segments[p], ops_in[builder_of[p]])) {
            return fail();
        }
    }

    // ---- Phase 2: apply the operations in order to the private copies. ------
    arena_.begin_commit();
    arena_open = true;
    bool any_change = false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t h = hash_key(ops[i].key);
        SegmentBuilder& b = builders.at(builder_of[partition_of(h)]);
        bool took_effect;
        if (ops[i].kind == BatchOp::Kind::Put) {
            if (!b.prepare_put(ops[i].key, h)) return fail();
            ValueRef ref;
            if (!arena_.append(ops[i].value, static_cast<std::uint32_t>(ops[i].length), &ref)) {
                b.abort_put();
                return fail();
            }
            b.commit_put(ops[i].key, h, ref);
            took_effect = true;
        } else {
            took_effect = b.erase(ops[i].key, h);
        }
        any_change = any_change || took_effect;
        if (applied != nullptr) applied[i] = took_effect;
    }

    if (!any_change) {
        // Only erases of absent keys: the state is unchanged, publish nothing.
        arena_.rollback_commit();
        ++w_.commits_without_change;
        return true;
    }
    // The commit now holds unpublished ValueRefs. Every chunk they name is
    // covered by the arena's writable or commit reference until end_commit().
    NOVA_TEST_POINT(TestPoint::WriterAfterAppends);

    // ---- Phase 3: the remaining allocations. ---------------------------------
    for (std::uint32_t i = 0; i < touched; ++i) {
        if (builders.at(i).changed() && !builders.at(i).prepare_finish()) return fail();
    }
    next = static_cast<Version*>(alloc_bytes(sizeof(Version), alignof(Version)));
    if (next == nullptr) return fail();
    // The retirement record is reserved BEFORE publication, so handing the old
    // version over afterwards cannot fail.
    node = static_cast<RetireNode*>(alloc_bytes(sizeof(RetireNode), alignof(RetireNode)));
    if (node == nullptr) return fail();

    // ================= Nothing below this line can fail. =======================

    // ---- Phase 4: assemble the new version and take ownership. --------------
    new (next) Version();
    NOVA_COUNT_OBJECT(LiveObject::Version, 1);
    next->sequence = base->sequence + 1;
    std::memcpy(next->segments, base->segments, sizeof(next->segments));
    w_.directory_bytes_copied += sizeof(next->segments);

    bool replaced[kPartitionCount] = {};
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (builder_of[p] == 0xFFFF) continue;
        SegmentBuilder& b = builders.at(builder_of[p]);
        if (!b.changed()) continue;  // e.g. an erase of an absent key: keep sharing
        w_.index_bytes_copied += b.bytes_built();
        w_.segment_growths += b.growths();
        w_.segment_rebuilds += b.rebuilds();
        Segment* seg = b.finish();
        if (seg != nullptr) {
            // The new segment takes its own reference on every chunk it lists,
            // BEFORE it can be seen. Each of those chunks is alive: it is
            // either listed by the base segment (which `base` owns) or covered
            // by this commit's writable / commit reference.
            for (std::uint32_t k = 0; k < seg->pin_count; ++k) arena_.add_ref(seg->pins[k].chunk_id);
            seg->owners.store(1, std::memory_order_seq_cst);
            w_.index_bytes_added += segment_bytes(seg);
            ++w_.segments_built;
        } else {
            ++w_.segments_dropped;
        }
        next->segments[p] = seg;
        replaced[p] = true;
    }
    // Segments carried over unchanged gain one more owning version. `base`
    // owns each of them, so this is never an increment from zero.
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (replaced[p] || next->segments[p] == nullptr) continue;
        next->segments[p]->owners.fetch_add(1, std::memory_order_seq_cst);
        ++w_.owner_refs_taken;
    }
    w_.index_bytes_added += sizeof(Version);
    // The new segments hold their references now: the commit drops its own.
    arena_.end_commit();

    // ---- Phase 5: publish. The linearization point of the whole write. ------
    NOVA_TEST_POINT(TestPoint::WriterBeforePublish);
    Version* const old = current_.exchange(next, std::memory_order_seq_cst);     // [W1]
    latest_ = next;
    ++w_.versions_published;
    NOVA_TEST_POINT(TestPoint::WriterAfterPublish);

    // ---- Phase 6: retire the replaced version. -------------------------------
    // It is owned by this local variable until the store to the queue below;
    // nothing in between can fail or return, so it cannot be lost. The tag is
    // read AFTER the exchange: every reader that can hold `old` loaded the
    // root before [W1], hence validated an epoch <= this tag.
    node->version = old;
    node->retire_epoch = epochs_.epoch();                                        // [W2]
    node->next.store(nullptr, std::memory_order_relaxed);
    NOVA_TEST_POINT(TestPoint::WriterAfterRetireTag);
    // One store makes the record visible to the cleaner, complete.
    queue_tail_->next.store(node, std::memory_order_seq_cst);                    // [W3]
    queue_tail_ = node;
    const std::uint64_t retired = retired_.load(std::memory_order_relaxed) + 1;
    retired_.store(retired, std::memory_order_seq_cst);
    const std::uint64_t backlog = retired - reclaimed_.load(std::memory_order_seq_cst);
    if (backlog > w_.backlog_max) w_.backlog_max = backlog;

    // Deterministic single-threaded configurations: the writer also plays the
    // cleaner. These are ordinary passes; they never wait for a reader.
    if (options_.cleaner == CleanerMode::Inline) drain();
    return true;
}

// ---- Reclamation ---------------------------------------------------------------------

void VersionedStore::set_reclaim_batch(int n) noexcept {
    reclaim_batch_->store(n <= 0 ? 1 : n, std::memory_order_seq_cst);
}

void VersionedStore::set_reclaim_work_per_version(std::uint32_t units) noexcept {
    work_per_version_.store(units == 0 ? 1 : units, std::memory_order_seq_cst);
}

// Releases the version in job_, resuming where a previous call stopped. Spends
// at most `budget` work units (one per directory entry visited, one per chunk
// reference dropped). Returns true when the version is completely destroyed;
// false if the budget ran out first, leaving job_ ready to be resumed.
bool VersionedStore::run_job(std::uint64_t& budget) noexcept {
    Version* v = job_.version;
    for (;;) {
        if (job_.segment == nullptr) {
            if (job_.partition == kPartitionCount) break;
            if (budget == 0) return false;
            --budget;
            Segment* seg = v->segments[job_.partition];
            // This version gives up its reference. Only the decrement that
            // reaches zero makes this thread the segment's destroyer; every
            // other version that points at the segment still owns it.
            if (seg == nullptr || seg->owners.fetch_sub(1, std::memory_order_seq_cst) != 1) {
                ++job_.partition;
                continue;
            }
            job_.segment = seg;
            job_.pin = 0;
        }
        // We are the last owner of job_.segment: drop its chunk references.
        Segment* seg = job_.segment;
        while (job_.pin < seg->pin_count) {
            if (budget == 0) return false;
            --budget;
            arena_.release(seg->pins[job_.pin].chunk_id);
            ++job_.pin;
        }
        index_bytes_released_.fetch_add(segment_bytes(seg), std::memory_order_relaxed);
        segments_released_.fetch_add(1, std::memory_order_relaxed);
        destroy_segment(seg);
        job_.segment = nullptr;
        ++job_.partition;
        NOVA_TEST_POINT(TestPoint::CleanerMidVersion);
    }
    index_bytes_released_.fetch_add(sizeof(Version), std::memory_order_relaxed);
    NOVA_COUNT_OBJECT(LiveObject::Version, -1);
    v->~Version();
    free_bytes(v);
    job_ = Job{};
    return true;
}

bool VersionedStore::epoch_pass() noexcept {
    bool progress = false;
    const bool pending =
        job_.version != nullptr || queue_head_->next.load(std::memory_order_seq_cst) != nullptr;
    // (1) One gated attempt to advance the epoch, if anything is waiting for it.
    if (pending && epochs_.try_advance()) progress = true;

    // (2) Destroy retired versions whose grace period has passed: at most
    // `batch` of them completely, oldest first, within the work budget.
    const std::uint64_t batch = static_cast<std::uint64_t>(reclaim_batch_->load(std::memory_order_seq_cst));
    const std::uint64_t allowance = batch * work_per_version_.load(std::memory_order_seq_cst);
    std::uint64_t budget = allowance;
    std::uint64_t freed = 0;
    while (freed < batch) {
        if (job_.version == nullptr) {
            RetireNode* oldest = queue_head_->next.load(std::memory_order_seq_cst);   // [C5]
            if (oldest == nullptr) break;
            // ELIGIBILITY: two gated advances beyond the retirement tag.
            // Nothing else (not age, not queue length) makes a version
            // destroyable. The queue is in tag order, so if the oldest entry
            // is not eligible none behind it is.
            if (epochs_.epoch() < oldest->retire_epoch + 2) break;                   // [C6]
            // Take it. `oldest` becomes the new dummy; its payload has been
            // moved into job_ and is never read from the node again.
            job_.version = oldest->version;
            RetireNode* consumed = queue_head_;
            queue_head_ = oldest;
            free_bytes(consumed);
        }
        const std::uint64_t before = budget;
        const bool done = run_job(budget);
        if (before != budget) progress = true;
        if (!done) {
            interrupted_versions_.fetch_add(1, std::memory_order_relaxed);
            break;  // budget exhausted: resume in a later pass
        }
        // Only now, with every reference released and the memory freed, does
        // the version count as reclaimed.
        ++freed;
        reclaimed_.fetch_add(1, std::memory_order_seq_cst);
        progress = true;
    }
    raise_max(max_versions_in_pass_, freed);
    raise_max(max_units_in_pass_, allowance - budget);
    return progress;
}

// Comparator for Part D.2 only: the naive stop-the-world sweep the assignment
// asks to be compared against. Every get() (and every snapshot-scoped call)
// holds stw_lock_ shared for its whole duration; taking it exclusively freezes
// all of them, and the sweep then walks the entire retire list in one go, with
// no batch limit and no work budget.
//
// What it frees is still decided by the epoch rule [C6], never by "the lock is
// held": a long-lived pin (a Snapshot) holds no lock between calls, only its
// announced epoch. With every short reader frozen out, only pins can be
// announcing, so two gated advances make every entry that no pin protects
// eligible; an entry a pin may still need stays, exactly as in an epoch pass.
bool VersionedStore::stw_pass() noexcept {
    if (backlog() < kStwBacklogTrigger) return false;
    std::unique_lock<std::shared_mutex> freeze(stw_lock_);
    stw_pauses_.fetch_add(1, std::memory_order_relaxed);
    bool progress = false;
    if (epochs_.try_advance()) progress = true;
    if (epochs_.try_advance()) progress = true;
    std::uint64_t unlimited = ~std::uint64_t{0};
    for (;;) {
        if (job_.version == nullptr) {
            RetireNode* oldest = queue_head_->next.load(std::memory_order_seq_cst);
            if (oldest == nullptr) break;
            if (epochs_.epoch() < oldest->retire_epoch + 2) break;   // [C6], as in epoch_pass
            job_.version = oldest->version;
            RetireNode* consumed = queue_head_;
            queue_head_ = oldest;
            free_bytes(consumed);
        }
        run_job(unlimited);
        reclaimed_.fetch_add(1, std::memory_order_seq_cst);
        progress = true;
    }
    return progress;
}

bool VersionedStore::reclaim_pass() noexcept {
    // One pass at a time; a second caller leaves at once instead of waiting.
    if (pass_running_.exchange(true, std::memory_order_seq_cst)) return false;
    bool progress = false;
    switch (options_.reclaim) {
        case ReclaimMode::Epoch: progress = epoch_pass(); break;
        case ReclaimMode::StopTheWorld: progress = stw_pass(); break;
        case ReclaimMode::Disabled: break;  // retired versions are kept until destruction
    }
    passes_.fetch_add(1, std::memory_order_relaxed);
    if (progress) passes_with_work_.fetch_add(1, std::memory_order_relaxed);
    pass_running_.store(false, std::memory_order_seq_cst);
    return progress;
}

bool VersionedStore::drain() noexcept {
    // Passes are repeated only while they achieve something. If a pinned
    // participant holds the epoch back, this returns with the backlog intact.
    for (int i = 0; i < 1 << 20 && backlog() != 0; ++i) {
        if (!reclaim_pass()) break;
    }
    return backlog() == 0;
}

void VersionedStore::cleaner_main() noexcept {
    while (!cleaner_stop_.load(std::memory_order_seq_cst)) {
        // Back to back while there is work; a short sleep when there is none.
        // The cleaner never blocks on a reader or on the writer.
        if (!reclaim_pass()) std::this_thread::sleep_for(kCleanerIdleSleep);
    }
    // CPU time this thread consumed, for the statistics (read after stop_cleaner()).
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        cleaner_cpu_ns_.fetch_add(static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
                                      static_cast<std::uint64_t>(ts.tv_nsec),
                                  std::memory_order_relaxed);
    }
}

bool VersionedStore::start_cleaner() noexcept {
    if (options_.cleaner != CleanerMode::Background || cleaner_thread_.joinable()) return true;
    cleaner_stop_.store(false, std::memory_order_seq_cst);
    try {
        cleaner_thread_ = std::thread([this]() { cleaner_main(); });
    } catch (...) {
        return false;
    }
    return true;
}

void VersionedStore::stop_cleaner() noexcept {
    if (!cleaner_thread_.joinable()) return;
    cleaner_stop_.store(true, std::memory_order_seq_cst);
    cleaner_thread_.join();
}

// ---- Introspection -----------------------------------------------------------------

std::uint64_t VersionedStore::memory_held_bytes() const noexcept {
    return w_.index_bytes_added - index_bytes_released_.load(std::memory_order_relaxed) +
           arena_.bytes_reserved();
}

VersionedStats VersionedStore::stats() const noexcept {
    VersionedStats s;
    s.versions_published = w_.versions_published;
    s.commits_failed = w_.commits_failed;
    s.commits_without_change = w_.commits_without_change;
    s.versions_retired = retired_.load(std::memory_order_seq_cst);
    s.versions_reclaimed = reclaimed_.load(std::memory_order_seq_cst);
    s.versions_retained = s.versions_retired - s.versions_reclaimed;
    s.backlog_max = w_.backlog_max;
    s.reclaim_passes = passes_.load(std::memory_order_relaxed);
    s.reclaim_passes_with_work = passes_with_work_.load(std::memory_order_relaxed);
    s.reclaim_max_versions_in_pass = max_versions_in_pass_.load(std::memory_order_relaxed);
    s.reclaim_max_units_in_pass = max_units_in_pass_.load(std::memory_order_relaxed);
    s.reclaim_interrupted_versions = interrupted_versions_.load(std::memory_order_relaxed);
    s.stw_pauses = stw_pauses_.load(std::memory_order_relaxed);
    s.cleaner_cpu_ns = cleaner_cpu_ns_.load(std::memory_order_relaxed);
    s.epoch = epochs_.stats();
    s.segments_built = w_.segments_built;
    s.segments_dropped = w_.segments_dropped;
    s.segments_released = segments_released_.load(std::memory_order_relaxed);
    s.segment_growths = w_.segment_growths;
    s.segment_rebuilds = w_.segment_rebuilds;
    s.index_bytes_copied = w_.index_bytes_copied;
    s.directory_bytes_copied = w_.directory_bytes_copied;
    s.owner_refs_taken = w_.owner_refs_taken;
    s.index_bytes_held = w_.index_bytes_added - index_bytes_released_.load(std::memory_order_relaxed);
    s.arena = arena_.stats();
    // Commits that changed nothing also unwind the arena, without being failures.
    s.arena.rollbacks -= w_.commits_without_change;
    return s;
}

void VersionedStore::for_each_version(void (*fn)(const Version*, void*), void* context) const {
    fn(latest_, context);
    for (const RetireNode* node = queue_head_->next.load(std::memory_order_seq_cst);
         node != nullptr; node = node->next.load(std::memory_order_seq_cst)) {
        fn(node->version, context);
    }
}

const char* VersionedStore::validate() {
    if (latest_ == nullptr) return "store has no current version";
    if (current_.load(std::memory_order_seq_cst) != latest_) return "current_ differs from the writer's latest version";
    if (pass_running_.load(std::memory_order_seq_cst)) return "validate() called while a reclamation pass is running";
    // A version caught half-released was already eligible; finish it so that
    // every version that exists is whole.
    if (job_.version != nullptr) {
        std::uint64_t unlimited = ~std::uint64_t{0};
        run_job(unlimited);
        reclaimed_.fetch_add(1, std::memory_order_seq_cst);
    }

    // Every (version, partition) reference that exists.
    std::vector<const Segment*> refs;
    std::uint64_t versions = 0;
    struct Ctx {
        std::vector<const Segment*>* refs;
        std::uint64_t* versions;
    } ctx{&refs, &versions};
    for_each_version(
        [](const Version* v, void* c) {
            Ctx* x = static_cast<Ctx*>(c);
            ++*x->versions;
            for (const Segment* s : v->segments) {
                if (s != nullptr) x->refs->push_back(s);
            }
        },
        &ctx);
    if (versions != backlog() + 1) return "queued version count differs from the backlog";
    std::sort(refs.begin(), refs.end());

    const PinnedArenaStats arena_stats = arena_.stats();
    std::vector<std::uint32_t> chunk_refs(arena_stats.chunk_ids_issued, 0);
    std::uint64_t held = versions * sizeof(Version);
    for (std::size_t i = 0; i < refs.size();) {
        const Segment* s = refs[i];
        std::size_t j = i;
        while (j < refs.size() && refs[j] == s) ++j;
        // O1: owners == number of versions whose directory points at the segment.
        if (s->owners.load(std::memory_order_seq_cst) != j - i) return "segment owner count mismatch";
        i = j;
        held += segment_bytes(s);

        if (s->capacity == 0 || (s->capacity & (s->capacity - 1)) != 0) return "segment capacity invalid";
        if (s->live == 0) return "published segment has no live key";
        std::uint32_t live = 0, tombs = 0;
        std::vector<ChunkPin> counted;
        for (std::uint32_t k = 0; k < s->capacity; ++k) {
            const Entry& e = s->slots[k];
            if (e.state == SlotState::Tombstone) {
                ++tombs;
                continue;
            }
            if (e.state == SlotState::Empty) continue;
            if (e.state != SlotState::Occupied) return "slot has an invalid state";
            ++live;
            const std::uint64_t h = hash_key(e.key);
            if (segment_find(s, e.key, h) != &e) return "key unreachable or duplicated in its segment";
            if (e.value.length == 0) {
                if (e.value.chunk_id != kNoChunk || e.value.offset != 0) return "empty value does not use the sentinel reference";
                continue;
            }
            const PinnedChunk* c = arena_.chunk(e.value.chunk_id);
            if (c == nullptr) return "ValueRef points at a missing chunk";
            if (c->id != e.value.chunk_id) return "chunk id does not match its directory slot";
            if (std::uint64_t{e.value.offset} + e.value.length > c->used) return "ValueRef exceeds the chunk's written bytes";
            bool found = false;
            for (ChunkPin& cp : counted) {
                if (cp.chunk_id == e.value.chunk_id) {
                    ++cp.refs;
                    found = true;
                    break;
                }
            }
            if (!found) counted.push_back(ChunkPin{e.value.chunk_id, 1});
        }
        if (live != s->live) return "segment live count mismatch";
        if (tombs != s->tombstones) return "segment tombstone count mismatch";
        if ((std::uint64_t{live} + tombs) * kMaxLoadDen > std::uint64_t{s->capacity} * kMaxLoadNum) return "segment exceeds the load bound";
        // O2: the list is exactly the set of distinct chunks referenced.
        if (counted.size() != s->pin_count) return "segment chunk list does not match its entries";
        for (std::uint32_t k = 0; k < s->pin_count; ++k) {
            if (k != 0 && s->pins[k - 1].chunk_id >= s->pins[k].chunk_id) return "segment chunk list not sorted";
            bool found = false;
            for (const ChunkPin& cp : counted) {
                if (cp.chunk_id == s->pins[k].chunk_id) {
                    found = cp.refs == s->pins[k].refs;
                    break;
                }
            }
            if (!found) return "segment chunk entry does not match its entries";
            ++chunk_refs[s->pins[k].chunk_id];
        }
    }
    if (held != w_.index_bytes_added - index_bytes_released_.load(std::memory_order_relaxed)) return "index bytes held mismatch";

    // O3/O4: refs == segments listing the chunk, plus one for the writable
    // chunk (no commit is open here). A chunk with no reference does not exist.
    std::uint64_t live_chunks = 0;
    for (std::uint32_t id = 0; id < chunk_refs.size(); ++id) {
        const PinnedChunk* c = arena_.chunk(id);
        if (c == nullptr) {
            if (chunk_refs[id] != 0) return "a segment references a destroyed chunk";
            continue;
        }
        ++live_chunks;
        const std::uint32_t expected = chunk_refs[id] + (c == arena_.current() ? 1u : 0u);
        if (c->refs.load(std::memory_order_seq_cst) != expected) return "chunk reference count mismatch";
        if (expected == 0) return "chunk exists with no reference";
        if (c->used > c->capacity) return "chunk used beyond its capacity";
    }
    if (live_chunks != arena_stats.chunks_live) return "live chunk count mismatch";
    return nullptr;
}

}  // namespace nova
