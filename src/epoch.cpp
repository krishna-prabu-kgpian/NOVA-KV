#include "epoch.h"

#include <new>
#include <utility>
#include <vector>

namespace nova {

namespace {
constexpr std::uint32_t kSlotsPerBlock = 32;
}

// A block of slots. Fully initialized before it is linked; once linked it is
// never unlinked, moved or freed until the domain is destroyed.
struct EpochDomain::SlotBlock {
    ReaderSlot slots[kSlotsPerBlock];
    std::atomic<SlotBlock*> next{nullptr};
};

// ---- Which domains are alive ------------------------------------------------------
//
// A thread caches the slot it was given per domain. When the thread exits it
// must give those slots back, but a domain may have been destroyed in the
// meantime. Domains therefore carry an ID that is never reused, and this
// registry says which IDs are alive. Thread exit and domain destruction both
// take its mutex, so a slot is either returned to a live domain or not touched
// at all. None of this is on the read path. The registry is never destroyed
// (threads may exit during process shutdown).
namespace {

struct LiveDomains {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, EpochDomain*>> live;
    std::atomic<std::uint64_t> next_id{1};  // 0 is "no domain"
};

LiveDomains& live_domains() {
    static LiveDomains* instance = new LiveDomains();
    return *instance;
}

}  // namespace

// Every slot this thread holds, for all domains. Destroyed at thread exit.
struct EpochThreadCache {
    struct Entry {
        std::uint64_t domain_id;
        ReaderSlot* slot;
    };
    std::vector<Entry> entries;

    ~EpochThreadCache() {
        LiveDomains& reg = live_domains();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (const Entry& e : entries) {
            for (const auto& d : reg.live) {
                if (d.first == e.domain_id) {
                    d.second->release_slot(e.slot);  // the domain still exists
                    break;
                }
            }
            // Otherwise the domain is gone and took its slots with it.
        }
    }
};

namespace {
thread_local EpochThreadCache tl_cache;
}

EpochDomain::EpochDomain()
    : id_(live_domains().next_id.fetch_add(1)), epoch_(0), first_(nullptr) {
    first_ = new (std::nothrow) SlotBlock();
    LiveDomains& reg = live_domains();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.live.emplace_back(id_, this);
}

EpochDomain::~EpochDomain() {
    {
        // After this block no exiting thread can reach this domain's slots.
        LiveDomains& reg = live_domains();
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (std::size_t i = 0; i < reg.live.size(); ++i) {
            if (reg.live[i].first == id_) {
                reg.live[i] = reg.live.back();
                reg.live.pop_back();
                break;
            }
        }
    }
    for (SlotBlock* b = first_; b != nullptr;) {
        SlotBlock* next = b->next.load(std::memory_order_relaxed);
        delete b;
        b = next;
    }
}

ReaderSlot* EpochDomain::acquire_slot() {
    std::lock_guard<std::mutex> lock(registration_mutex_);
    SlotBlock* last = nullptr;
    for (SlotBlock* b = first_; b != nullptr; b = b->next.load(std::memory_order_seq_cst)) {
        for (ReaderSlot& s : b->slots) {
            // Only registration (under this mutex) sets `owned`; a release
            // clears it. A free slot is inactive, so the collector ignores it.
            if (s.owned.load(std::memory_order_seq_cst) == 0) {
                s.owned.store(1, std::memory_order_seq_cst);
                registrations_.fetch_add(1, std::memory_order_relaxed);
                return &s;
            }
        }
        last = b;
    }
    // Every slot is taken: add a block. It is complete (all slots inactive)
    // before the store below makes it reachable, and the slot handed out is in
    // a block that is already linked when the caller first announces in it.
    SlotBlock* block = new (std::nothrow) SlotBlock();
    if (block == nullptr || last == nullptr) throw std::bad_alloc();
    block->slots[0].owned.store(1, std::memory_order_relaxed);
    last->next.store(block, std::memory_order_seq_cst);                               // [R1] link
    registrations_.fetch_add(1, std::memory_order_relaxed);
    return &block->slots[0];
}

void EpochDomain::release_slot(ReaderSlot* slot) noexcept {
    slot->state.store(kEpochInactive, std::memory_order_seq_cst);
    slot->owned.store(0, std::memory_order_seq_cst);
}

ReaderSlot* EpochDomain::thread_slot_slow() {
    for (const EpochThreadCache::Entry& e : tl_cache.entries) {
        if (e.domain_id == id_) {
            tl_epoch_last = EpochTlsLast{id_, e.slot};
            return e.slot;
        }
    }
    tl_cache.entries.reserve(tl_cache.entries.size() + 1);  // may throw: nothing acquired yet
    ReaderSlot* slot = acquire_slot();                       // may throw: nothing recorded yet
    tl_cache.entries.push_back(EpochThreadCache::Entry{id_, slot});  // cannot throw now
    tl_epoch_last = EpochTlsLast{id_, slot};
    return slot;
}

bool EpochDomain::try_advance() noexcept {
    advance_attempts_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t e = epoch_.load(std::memory_order_seq_cst);                         // [C1]
    NOVA_TEST_POINT(TestPoint::CleanerBeforeScan);
    for (SlotBlock* b = first_; b != nullptr; b = b->next.load(std::memory_order_seq_cst)) {  // [C2]
        for (ReaderSlot& s : b->slots) {
            const std::uint64_t announced = s.state.load(std::memory_order_seq_cst);  // [C3]
            NOVA_TEST_POINT(TestPoint::CleanerMidScan);
            if (announced != kEpochInactive && announced != e) {
                // A participant is still in an older epoch: no advance. Not a
                // wait; the next pass tries again.
                advance_blocked_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }
    NOVA_TEST_POINT(TestPoint::CleanerBeforeAdvance);
    // Advance by exactly one, and only from the epoch that was scanned.
    if (!epoch_.compare_exchange_strong(e, e + 1, std::memory_order_seq_cst)) return false;  // [C4]
    advance_successes_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

EpochStats EpochDomain::stats() const noexcept {
    EpochStats s;
    s.epoch = epoch_.load(std::memory_order_seq_cst);
    s.advance_attempts = advance_attempts_.load(std::memory_order_relaxed);
    s.advance_successes = advance_successes_.load(std::memory_order_relaxed);
    s.advance_blocked = advance_blocked_.load(std::memory_order_relaxed);
    s.registrations = registrations_.load(std::memory_order_relaxed);
    for (SlotBlock* b = first_; b != nullptr; b = b->next.load(std::memory_order_seq_cst)) {
        for (const ReaderSlot& slot : b->slots) {
            ++s.slots_total;
            if (slot.owned.load(std::memory_order_seq_cst) != 0) ++s.slots_owned;
            const std::uint64_t st = slot.state.load(std::memory_order_seq_cst);
            if (st != kEpochInactive) {
                ++s.slots_active;
                if (st < s.oldest_active_epoch) s.oldest_active_epoch = st;
            }
            s.validation_retries += slot.retries.load(std::memory_order_relaxed);
        }
    }
    return s;
}

}  // namespace nova
