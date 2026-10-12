// versioned_store.h -- the Part B storage engine: immutable published versions,
// copy-on-write index, one atomic root pointer, epoch-based reclamation.
//
// Shape (blueprint section 3):
//
//   current ──atomic──> Version { sequence, segments[256] }      immutable
//                           └──> Segment { slots[], chunk refs }  immutable, shared
//                                    └──> ValueRef {chunk_id, offset, length}
//   chunk directory (global, atomic slots) ──> chunk bytes        append-only
//
// THREE ROLES
//
//   Readers (any number of threads): announce an epoch in their own slot,
//     validate it, load `current` exactly once, read only immutable objects,
//     copy the value out, clear the slot. No lock, no allocation, no reference
//     count, no waiting for anyone.
//
//   The writer (exactly one thread, as the assignment assumes): builds the
//     replacement segments privately and publishes them with one atomic swap
//     of `current`. The replaced version is tagged with the epoch read AFTER
//     the swap and appended to the retirement queue using a record allocated
//     BEFORE the swap. The writer never waits for a grace period.
//
//   The cleaner (one thread at a time): in passes. One pass = one gated attempt
//     to advance the epoch, then destruction of retired versions whose grace
//     period has passed: at most `reclaim batch` versions per pass, in FIFO
//     order, within a work budget, resumable in the middle of a version. It
//     never waits for a reader and never writes `current`.
//
// A version may be destroyed only when the global epoch is at least two gated
// advances past its retirement tag. Never because of its age or its position
// in the queue. The argument that this is sufficient, line by line, is
// docs/reclamation.md.
//
// Ownership (unchanged from B1, see docs/versioning.md): a Version owns one
// reference to each Segment in its directory (`owners`); a Segment owns one
// reference to each distinct chunk it lists; the arena adds writable and commit
// references so that one count decides a chunk's life (pinned_arena.h).
#pragma once

#include "cow_segment.h"
#include "epoch.h"
#include "pinned_arena.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace nova {

// One complete, immutable state of the store.
struct Version {
    std::uint64_t sequence;                 // commits since creation; diagnostic only
    Segment* segments[kPartitionCount];     // the directory; null = empty partition
};

struct BatchOp {
    enum class Kind : std::uint8_t { Put, Erase };
    Kind kind;
    long key;
    const void* value;   // Put only
    std::size_t length;  // Put only
};

// How retired versions are treated.
enum class ReclaimMode : std::uint8_t {
    Epoch,         // the real thing: epoch-based reclamation (default)
    Disabled,      // comparator: versions are retired but never destroyed
    StopTheWorld,  // comparator ONLY: readers take a shared lock; the cleaner
                   // excludes all of them and frees the whole retire list at once
};

// Who runs reclamation passes.
enum class CleanerMode : std::uint8_t {
    Background,  // a dedicated thread owned by the store (default)
    Manual,      // nobody, until reclaim_pass() / drain() is called (tests)
    Inline,      // the writer, after each commit (deterministic single-thread tests)
};

struct StoreOptions {
    ReclaimMode reclaim = ReclaimMode::Epoch;
    CleanerMode cleaner = CleanerMode::Background;
};

// Options used by stores constructed without explicit options (KVStore's
// default constructor). Tests and benchmark harnesses set this before
// constructing a store; it is not reachable through the assignment's API.
void set_default_store_options(const StoreOptions& options) noexcept;
StoreOptions default_store_options() noexcept;

struct VersionedStats {
    // Publication
    std::uint64_t versions_published = 0;     // successful swaps of `current`
    std::uint64_t commits_failed = 0;         // commits that published nothing (allocation, limits)
    std::uint64_t commits_without_change = 0; // e.g. erase of absent keys only
    // Reclamation
    std::uint64_t versions_retired = 0;       // handed to the retirement queue
    std::uint64_t versions_reclaimed = 0;     // completely destroyed
    std::uint64_t versions_retained = 0;      // retired, not yet completely destroyed (the backlog)
    std::uint64_t backlog_max = 0;            // largest backlog seen by the writer
    std::uint64_t reclaim_passes = 0;
    std::uint64_t reclaim_passes_with_work = 0;
    std::uint64_t reclaim_max_versions_in_pass = 0;   // never above the batch size in force
    std::uint64_t reclaim_max_units_in_pass = 0;
    std::uint64_t reclaim_interrupted_versions = 0;   // destructions split across passes
    std::uint64_t stw_pauses = 0;
    std::uint64_t cleaner_cpu_ns = 0;         // CPU time of the background cleaner thread(s), recorded when stopped
    EpochStats epoch;
    // Copy-on-write cost
    std::uint64_t segments_built = 0;         // replacement segments published
    std::uint64_t segments_dropped = 0;       // partitions that became empty
    std::uint64_t segments_released = 0;      // segments destroyed
    std::uint64_t segment_growths = 0;
    std::uint64_t segment_rebuilds = 0;
    std::uint64_t index_bytes_copied = 0;     // slot bytes copied/rebuilt for published segments
    std::uint64_t directory_bytes_copied = 0;
    std::uint64_t owner_refs_taken = 0;       // segment references acquired by new versions
    // Memory held by index structures (versions + segments), current and retired
    std::uint64_t index_bytes_held = 0;
    PinnedArenaStats arena;
};

class VersionedStore {
public:
    VersionedStore();                                  // default_store_options()
    // `reclaim_batch`, if given, is where the reclaim-batch setting is stored
    // (KVStore's private member); it must already hold a positive value and
    // must outlive the store. Without it the store keeps the setting itself.
    explicit VersionedStore(const StoreOptions& options,
                            std::atomic<int>* reclaim_batch = nullptr);
    ~VersionedStore();
    VersionedStore(const VersionedStore&) = delete;
    VersionedStore& operator=(const VersionedStore&) = delete;

    // False if construction could not allocate what the store needs.
    bool ok() const noexcept { return ok_; }

    // ---- Readers (any thread) -----------------------------------------------
    // Throws std::bad_alloc only if, on a thread's FIRST read of this store, a
    // reader slot cannot be allocated; it never reports a present key as absent
    // for lack of resources. Every later read by that thread cannot throw.
    bool get(long key, void* value_out, std::size_t value_out_len) const;

    // A long-lived protected view of one version: the internal foundation of
    // the Part C Snapshot. It owns a dedicated reader slot, pinned from
    // acquire_pin() to release_pin(), whichever thread holds it. Moving a Pin
    // is copying this struct and clearing the source; the slot's announcement
    // is not touched by a move. While a Pin exists, the version it names and
    // everything reachable from it cannot be destroyed. One thread at a time
    // may use a given Pin.
    struct Pin {
        ReaderSlot* slot = nullptr;
        const Version* version = nullptr;
        explicit operator bool() const noexcept { return slot != nullptr; }
    };
    Pin acquire_pin() const;                             // throws std::bad_alloc
    bool get_pinned(const Pin& pin, long key, void* value_out,
                    std::size_t value_out_len) const noexcept;
    void release_pin(Pin& pin) const noexcept;           // pin becomes inert
    // The pin that owns `slot` (the slot remembers its version). Null gives an
    // inert pin. This is how a public Snapshot, which stores one pointer, is
    // turned back into a Pin.
    static Pin pin_of(ReaderSlot* slot) noexcept;
    // Up to `max_keys` keys present in the pinned version, each exactly once,
    // in an unspecified but repeatable order (partition, then slot). Returns
    // the number written; 0 if `keys_out` is null or `max_keys` is 0.
    std::size_t keys_pinned(const Pin& pin, long* keys_out, std::size_t max_keys) const noexcept;
    // Number of keys in the pinned version.
    std::size_t size_pinned(const Pin& pin) const noexcept;

    // ---- Writes ------------------------------------------------------------------
    // One writer at a time: put, erase and commit take an internal writer mutex
    // for their whole duration, so calls from several threads are serialized
    // (they never run concurrently, and each is one atomic publication). No
    // reader ever takes this mutex.
    bool put(long key, const void* value, std::size_t value_len) noexcept;
    bool erase(long key) noexcept;
    // Applies `ops` in order as ONE logical write and publishes at most once.
    // Returns false, having changed nothing, if an argument is invalid or an
    // allocation fails. `applied` (optional, n entries) reports per operation
    // whether it took effect.
    bool commit(const BatchOp* ops, std::size_t n, bool* applied = nullptr) noexcept;

    // ---- Reclamation ------------------------------------------------------------
    // How many retired versions one pass may completely destroy. n <= 0 is
    // treated as 1. Any thread.
    void set_reclaim_batch(int n) noexcept;
    int reclaim_batch() const noexcept { return reclaim_batch_->load(std::memory_order_seq_cst); }
    // Work units (directory entries visited + chunk references dropped) one
    // pass may spend per version of its batch. A version costing more is
    // finished in a later pass. Any thread.
    void set_reclaim_work_per_version(std::uint32_t units) noexcept;

    // One reclamation pass on the calling thread. Returns true if it made
    // progress (advanced the epoch or released something). If another pass is
    // running it returns false at once; it never waits. With a Background
    // cleaner, call this only after stop_cleaner().
    bool reclaim_pass() noexcept;
    // Runs passes until nothing is retired any more or passes stop making
    // progress (a pinned participant holds the epoch back). Returns true if
    // the backlog is empty. Same calling rule as reclaim_pass().
    bool drain() noexcept;
    // Stops and joins the background cleaner (no-op in other modes); restarts it.
    void stop_cleaner() noexcept;
    bool start_cleaner() noexcept;

    // ---- Introspection ----------------------------------------------------------
    // Safe from any thread at any time.
    std::uint64_t backlog() const noexcept {
        return retired_.load(std::memory_order_seq_cst) - reclaimed_.load(std::memory_order_seq_cst);
    }
    std::uint64_t epoch() const noexcept { return epochs_.epoch(); }
    const StoreOptions& options() const noexcept { return options_; }
    const void* root_address() const noexcept { return &current_; }
    // Bytes held by versions, segments and chunks. Writer thread, or quiet store.
    std::uint64_t memory_held_bytes() const noexcept;
    // Writer thread, or quiet store (no writer; cleaner stopped or idle).
    VersionedStats stats() const noexcept;
    const PinnedArena& arena() const noexcept { return arena_; }

    // The following require a QUIET store: no writer, no pass in progress, and
    // the background cleaner stopped. They do not need readers to be absent.
    //
    // Pointer to the current version WITHOUT protection. It stays valid only
    // until the version is retired and reclaimed; tests use it with a Manual
    // cleaner, where nothing is reclaimed until they say so.
    const Version* current_version_unprotected() const noexcept {
        return current_.load(std::memory_order_seq_cst);
    }
    bool get_at_unprotected(const Version* v, long key, void* value_out,
                            std::size_t value_out_len) const noexcept;
    // Calls fn for the current version and every version still in the queue.
    void for_each_version(void (*fn)(const Version*, void*), void* context) const;
    // Recomputes every owner count and chunk reference from the versions that
    // exist and compares with the stored counts. nullptr if consistent.
    // Finishes a half-released version first (it was already eligible).
    const char* validate();

private:
    struct RetireNode {
        Version* version;
        std::uint64_t retire_epoch;
        std::atomic<RetireNode*> next;
    };
    // A version in the middle of being released, and where to resume.
    struct Job {
        Version* version = nullptr;
        std::uint32_t partition = 0;
        Segment* segment = nullptr;   // segment whose last owner we were; chunk refs in progress
        std::uint32_t pin = 0;
    };

    bool read(const Version* v, long key, std::uint64_t h, void* value_out,
              std::size_t value_out_len) const noexcept;
    bool commit_locked(const BatchOp* ops, std::size_t n, bool* applied) noexcept;  // writer_mutex_ held
    bool run_job(std::uint64_t& budget) noexcept;     // cleaner role
    bool epoch_pass() noexcept;                       // cleaner role
    bool stw_pass() noexcept;                         // cleaner role
    void cleaner_main() noexcept;
    void destroy_everything() noexcept;               // destructor only

    const StoreOptions options_;
    bool ok_ = false;

    // The one word readers and the writer share, alone on its cache line.
    alignas(kCacheLine) std::atomic<Version*> current_;

    // ---- Writer-only state (guarded by writer_mutex_) ------------------------------
    alignas(kCacheLine) std::mutex writer_mutex_;   // serializes put / erase / commit
    Version* latest_;                       // == current_, without an atomic load
    RetireNode* queue_tail_;                // last node of the retirement queue
    struct WriterCounters {
        std::uint64_t versions_published = 0, commits_failed = 0, commits_without_change = 0;
        std::uint64_t segments_built = 0, segments_dropped = 0, segment_growths = 0, segment_rebuilds = 0;
        std::uint64_t index_bytes_copied = 0, directory_bytes_copied = 0, owner_refs_taken = 0;
        std::uint64_t index_bytes_added = 0, backlog_max = 0;
    } w_;

    // ---- Written by the writer, read by anyone -----------------------------------
    alignas(kCacheLine) std::atomic<std::uint64_t> retired_{0};

    // ---- Cleaner-role state -------------------------------------------------------
    alignas(kCacheLine) RetireNode* queue_head_;   // dummy node; its successor is the oldest entry
    Job job_;
    std::atomic<bool> pass_running_{false};
    std::atomic<std::uint64_t> reclaimed_{0};
    std::atomic<std::uint64_t> segments_released_{0};
    std::atomic<std::uint64_t> index_bytes_released_{0};
    std::atomic<std::uint64_t> passes_{0}, passes_with_work_{0};
    std::atomic<std::uint64_t> max_versions_in_pass_{0}, max_units_in_pass_{0};
    std::atomic<std::uint64_t> interrupted_versions_{0};
    std::atomic<std::uint64_t> stw_pauses_{0};
    std::atomic<std::uint64_t> cleaner_cpu_ns_{0};

    // ---- Configuration and the background thread ---------------------------------
    std::atomic<int> own_reclaim_batch_{kDefaultReclaimBatch};  // used when none was supplied
    std::atomic<int>* const reclaim_batch_;                     // the setting every pass reads
    std::atomic<std::uint32_t> work_per_version_{1024};
    std::atomic<bool> cleaner_stop_{false};
    std::thread cleaner_thread_;
    mutable std::shared_mutex stw_lock_;   // StopTheWorld comparator only

    mutable EpochDomain epochs_;
    PinnedArena arena_;
};

}  // namespace nova
