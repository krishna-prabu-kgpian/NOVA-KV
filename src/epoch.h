// epoch.h -- epoch-based reclamation (EBR): the reader side and the epoch gate.
//
// Classical three-epoch scheme with GATED advancement, exactly the protocol of
// NOVA_KV_Formal_Safety_and_Agent_Audit.md section 3. Every atomic operation
// of the protocol is memory_order_seq_cst.
//
//   reader pin:    loop { e = epoch.load; slot.store(e); if (epoch.load == e) break; }
//                  ... then, and only then, load the root and use it ...
//   reader unpin:  slot.store(INACTIVE)
//   collector:     e = epoch.load; for every slot: if (active && slot != e) give up;
//                  epoch.compare_exchange(e, e + 1)
//   retirement:    tag = epoch.load  AFTER the root has been replaced
//   eligibility:   epoch.load >= tag + 2
//
// The collector never waits for a reader and a reader never waits for anyone:
// a reader that has not left simply makes the collector's advance fail, to be
// tried again in a later pass. Age is never used: only the two gated advances.
//
// SLOTS. A participant (a reading thread, or a long-lived pin such as a future
// Snapshot) announces itself in a ReaderSlot. Slots live in blocks that are
// appended to a list and never moved or freed while the domain lives, so the
// collector can always enumerate every slot that was ever handed out. There is
// no fixed limit on their number. Handing a slot out (registration) is the only
// operation that takes a mutex or allocates; it happens once per (thread,
// store), not per read.
//
// The correctness argument, tied to the lines of this file and of
// versioned_store.cpp, is docs/reclamation.md.
#pragma once

#include "config.h"
#include "test_points.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace nova {

constexpr std::uint64_t kEpochInactive = ~std::uint64_t{0};

// One participant's announcement. Alone on its cache line: readers on
// different cores never write to the same line.
struct alignas(kCacheLine) ReaderSlot {
    std::atomic<std::uint64_t> state{kEpochInactive};  // announced epoch, or kEpochInactive
    std::atomic<std::uint32_t> owned{0};               // 1 while handed out
    std::atomic<std::uint64_t> retries{0};             // validations that had to repeat (owner writes)
};

struct EpochStats {
    std::uint64_t epoch = 0;
    std::uint64_t advance_attempts = 0;
    std::uint64_t advance_successes = 0;
    std::uint64_t advance_blocked = 0;        // attempts stopped by a participant in an older epoch
    std::uint64_t slots_total = 0;            // slots that exist
    std::uint64_t slots_owned = 0;            // slots currently handed out
    std::uint64_t slots_active = 0;           // slots announcing an epoch right now
    std::uint64_t registrations = 0;          // slots handed out, cumulative
    std::uint64_t validation_retries = 0;     // reader pin loops that repeated, cumulative
    std::uint64_t oldest_active_epoch = kEpochInactive;
};

// The slot this thread used for its most recent read, and for which domain.
// Constant-initialized and defined inline so that reading it is a plain
// thread-local load with no initialization guard on the read path.
struct EpochTlsLast {
    std::uint64_t domain_id;  // 0 = none; domain IDs start at 1 and are never reused
    ReaderSlot* slot;
};
inline thread_local EpochTlsLast tl_epoch_last = {0, nullptr};

class EpochDomain {
public:
    EpochDomain();
    ~EpochDomain();
    EpochDomain(const EpochDomain&) = delete;
    EpochDomain& operator=(const EpochDomain&) = delete;

    // False if the first slot block could not be allocated.
    bool ok() const noexcept { return first_ != nullptr; }

    // ---- Participants -------------------------------------------------------
    // The calling thread's slot for this domain, registering the thread on
    // first use. Steady state: one thread-local comparison, no lock, no
    // allocation. Throws std::bad_alloc if a slot cannot be provided; it never
    // returns a slot the collector cannot see.
    ReaderSlot* thread_slot() {
        if (tl_epoch_last.domain_id == id_) return tl_epoch_last.slot;
        return thread_slot_slow();
    }
    // A slot not tied to any thread (long-lived pins). Throws std::bad_alloc.
    ReaderSlot* acquire_slot();
    // Returns a slot; it must be inactive.
    void release_slot(ReaderSlot* slot) noexcept;

    // Announces the participant and validates the announcement. On return the
    // slot holds an epoch that was the global epoch at a moment after the slot
    // was written; only now may the caller load the root. One participant, one
    // critical section at a time per slot.
    void pin(ReaderSlot* slot) noexcept {
        for (;;) {
            const std::uint64_t e = epoch_.load(std::memory_order_seq_cst);          // [P1]
            NOVA_TEST_POINT(TestPoint::ReaderBeforeAnnounce);
            slot->state.store(e, std::memory_order_seq_cst);                         // [P2] announce
            NOVA_TEST_POINT(TestPoint::ReaderAfterAnnounce);
            if (epoch_.load(std::memory_order_seq_cst) == e) break;                  // [P3] validate
            // The epoch moved between [P1] and [P3]: announce again. Nothing
            // has been dereferenced yet.
            slot->retries.store(slot->retries.load(std::memory_order_relaxed) + 1,
                                std::memory_order_relaxed);
        }
        NOVA_TEST_POINT(TestPoint::ReaderAfterValidate);
    }
    void unpin(ReaderSlot* slot) noexcept {
        slot->state.store(kEpochInactive, std::memory_order_seq_cst);                // [P5]
    }

    // ---- Collector (one thread at a time) -------------------------------------
    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_seq_cst); }
    // One gated attempt to move the epoch forward by one. Never waits.
    bool try_advance() noexcept;

    EpochStats stats() const noexcept;

private:
    struct SlotBlock;
    ReaderSlot* thread_slot_slow();
    friend struct EpochThreadCache;

    const std::uint64_t id_;                  // unique for the life of the process
    std::atomic<std::uint64_t> epoch_;
    SlotBlock* first_;                        // head of the block list; never changes
    std::mutex registration_mutex_;           // registration only, never on the read path
    std::atomic<std::uint64_t> registrations_{0};
    std::atomic<std::uint64_t> advance_attempts_{0};
    std::atomic<std::uint64_t> advance_successes_{0};
    std::atomic<std::uint64_t> advance_blocked_{0};
};

}  // namespace nova
