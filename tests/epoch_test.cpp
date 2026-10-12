// epoch_test.cpp -- unit tests of the epoch domain (src/epoch.h): reader slots
// that never move and have no fixed limit, the gated advance, slot return at
// thread exit, and thread exit after the domain is gone.
#include "epoch.h"

#include "test_util.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using nova::EpochDomain;
using nova::ReaderSlot;

namespace {

void test_gated_advance() {
    EpochDomain d;
    NOVA_CHECK(d.ok() && d.epoch() == 0);
    // Nobody active: every attempt advances by exactly one.
    NOVA_CHECK(d.try_advance() && d.epoch() == 1);
    NOVA_CHECK(d.try_advance() && d.epoch() == 2);

    ReaderSlot* a = d.acquire_slot();
    ReaderSlot* b = d.acquire_slot();
    NOVA_CHECK(a != b);
    d.pin(a);  // a announces epoch 2
    NOVA_CHECK(a->state.load() == 2);
    // A participant in the CURRENT epoch does not stop the advance ...
    NOVA_CHECK(d.try_advance() && d.epoch() == 3);
    // ... but it now sits in an older epoch and stops every further one,
    // however often the collector tries and however long it has been there.
    for (int i = 0; i < 1000; ++i) NOVA_CHECK(!d.try_advance());
    NOVA_CHECK(d.epoch() == 3);
    d.pin(b);  // b arrives later, in epoch 3
    NOVA_CHECK(b->state.load() == 3);
    NOVA_CHECK(!d.try_advance() && d.epoch() == 3);  // a still blocks
    d.unpin(a);
    NOVA_CHECK(d.try_advance() && d.epoch() == 4);   // b was in the scanned epoch
    NOVA_CHECK(!d.try_advance());                    // now b blocks
    d.unpin(b);
    NOVA_CHECK(d.try_advance() && d.epoch() == 5);

    const nova::EpochStats st = d.stats();
    NOVA_CHECK(st.advance_successes == 5);
    NOVA_CHECK(st.advance_blocked == 1002);
    NOVA_CHECK(st.advance_attempts == 1007);
    NOVA_CHECK(st.slots_owned == 2 && st.slots_active == 0);
    d.release_slot(a);
    d.release_slot(b);
    NOVA_CHECK(d.stats().slots_owned == 0);
}

// No fixed limit: slots keep coming, existing ones never move, and the
// collector sees every one of them, including those in blocks added later.
void test_slots_expand_and_stay_put() {
    EpochDomain d;
    std::vector<ReaderSlot*> slots;
    for (int i = 0; i < 1000; ++i) slots.push_back(d.acquire_slot());
    for (int i = 0; i < 1000; ++i) {
        for (int j = i + 1; j < 1000; j += 97) NOVA_CHECK(slots[static_cast<std::size_t>(i)] != slots[static_cast<std::size_t>(j)]);
    }
    const nova::EpochStats st = d.stats();
    NOVA_CHECK(st.slots_owned == 1000 && st.slots_total >= 1000 && st.registrations == 1000);
    // Pin the very last slot handed out (in the newest block): it must gate.
    d.pin(slots.back());
    NOVA_CHECK(d.try_advance());
    NOVA_CHECK(!d.try_advance());
    NOVA_CHECK(d.stats().oldest_active_epoch == 0);
    d.unpin(slots.back());
    NOVA_CHECK(d.try_advance());
    // Released slots are reused before new blocks are added.
    const std::uint64_t total_before = d.stats().slots_total;
    for (int i = 0; i < 500; ++i) d.release_slot(slots[static_cast<std::size_t>(i)]);
    for (int i = 0; i < 500; ++i) slots[static_cast<std::size_t>(i)] = d.acquire_slot();
    NOVA_CHECK(d.stats().slots_total == total_before && d.stats().slots_owned == 1000);
    for (ReaderSlot* s : slots) d.release_slot(s);
}

// A thread gets one slot per domain, keeps it across reads, and gives it back
// when the thread ends; the next thread reuses it.
void test_thread_slots_and_reuse_after_exit() {
    EpochDomain d1, d2;
    ReaderSlot* seen1 = nullptr;
    ReaderSlot* seen2 = nullptr;
    std::thread t1([&]() {
        ReaderSlot* a = d1.thread_slot();
        ReaderSlot* b = d2.thread_slot();
        NOVA_CHECK(a != b);
        // Stable across calls, also when alternating between domains.
        for (int i = 0; i < 100; ++i) {
            NOVA_CHECK(d1.thread_slot() == a);
            NOVA_CHECK(d2.thread_slot() == b);
        }
        d1.pin(a);
        d1.unpin(a);
        seen1 = a;
        seen2 = b;
    });
    t1.join();
    NOVA_CHECK(d1.stats().slots_owned == 0 && d2.stats().slots_owned == 0);  // given back at exit
    NOVA_CHECK(d1.stats().registrations == 1);
    std::thread t2([&]() {
        NOVA_CHECK(d1.thread_slot() == seen1);  // the same slot, reused
        NOVA_CHECK(d2.thread_slot() == seen2);
    });
    t2.join();
    NOVA_CHECK(d1.stats().registrations == 2 && d1.stats().slots_total == d2.stats().slots_total);

    // Many short-lived threads: the slot population does not grow with them.
    for (int round = 0; round < 200; ++round) {
        std::thread t([&]() {
            ReaderSlot* s = d1.thread_slot();
            d1.pin(s);
            d1.unpin(s);
        });
        t.join();
    }
    NOVA_CHECK(d1.stats().slots_owned == 0);
    NOVA_CHECK(d1.stats().slots_total == d2.stats().slots_total);
    NOVA_CHECK(d1.stats().registrations == 202);
}

// A thread that used a domain outlives it. Its exit must not touch the domain.
// (ASan reports a use-after-free here if the thread-exit path does.)
void test_thread_outlives_domain() {
    std::mutex m;
    std::condition_variable cv;
    bool registered = false, domain_gone = false;
    auto d = std::make_unique<EpochDomain>();
    std::thread t([&]() {
        ReaderSlot* s = d->thread_slot();
        d->pin(s);
        d->unpin(s);
        {
            std::lock_guard<std::mutex> lk(m);
            registered = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return domain_gone; });
        // The thread now ends, with a cached slot of a destroyed domain.
    });
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return registered; });
    }
    d.reset();  // domain destroyed while the thread still caches one of its slots
    {
        std::lock_guard<std::mutex> lk(m);
        domain_gone = true;
    }
    cv.notify_all();
    t.join();

    // A new domain at (possibly) the same address is a different domain: a
    // thread that cached a slot of the old one must register afresh.
    std::thread t2([&]() {
        auto a = std::make_unique<EpochDomain>();
        ReaderSlot* sa = a->thread_slot();
        a.reset();
        auto b = std::make_unique<EpochDomain>();
        ReaderSlot* sb = b->thread_slot();
        NOVA_CHECK(b->stats().registrations == 1);  // not served from the stale cache
        b->pin(sb);
        NOVA_CHECK(!b->try_advance() || b->epoch() == 1);
        b->unpin(sb);
        (void)sa;
    });
    t2.join();
}

// Registrations racing with scans. Whatever the timing, a participant that has
// validated epoch e and is still pinned keeps the epoch at most at e + 1.
void test_registration_races_with_scans() {
    EpochDomain d;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> violations{0}, pins{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&]() {
            while (!stop.load()) {
                // A brand new slot each time: registration, and often a new
                // block, concurrent with the collector's traversal.
                ReaderSlot* s = d.acquire_slot();
                d.pin(s);
                const std::uint64_t e = s->state.load();
                for (int k = 0; k < 20; ++k) {
                    if (d.epoch() > e + 1) violations.fetch_add(1);
                }
                d.unpin(s);
                pins.fetch_add(1);
                // Keep most of them: the slot list keeps growing.
                if (pins.load() % 4 == 0) d.release_slot(s);
            }
        });
    }
    std::uint64_t advances = 0;
    while (pins.load() < 12000) {
        if (d.try_advance()) ++advances;
    }
    stop.store(true);
    for (auto& t : threads) t.join();
    NOVA_CHECK(violations.load() == 0);
    NOVA_CHECK(advances > 100);
    NOVA_CHECK(d.stats().slots_total > 1000);  // the list really grew during the scans (about 9000 slots)
}

}  // namespace

int main() {
    NOVA_CHECK(alignof(ReaderSlot) == nova::kCacheLine && sizeof(ReaderSlot) == nova::kCacheLine);
    test_gated_advance();
    test_slots_expand_and_stay_put();
    test_thread_slots_and_reuse_after_exit();
    test_thread_outlives_domain();
    test_registration_races_with_scans();
    return nova_test::finish("epoch_test");
}
