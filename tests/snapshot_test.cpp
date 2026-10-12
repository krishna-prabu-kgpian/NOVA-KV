// snapshot_test.cpp -- Part C.1: KVStore::Snapshot, acquire_snapshot(),
// snapshot-scoped get() and keys() (Appendix A.1).
//
// Public API only, plus the internal inspector for statistics. Built for every
// variant: on the lock-based baselines the only thing to test is that a
// snapshot is refused loudly (they have no versions to pin).
//
// What a Snapshot must do on the rcu build:
//   - name ONE published version, the one current at acquisition;
//   - keep that version, its segments and its value bytes alive for as long as
//     the handle lives, in every reclamation mode, whatever the writer and the
//     cleaner do (in particular: no chunk ID it can reach is reused);
//   - be move-only: a move transfers the protection, never drops or copies it;
//   - release the protection exactly once;
//   - be refused by any store other than the one it came from.
#include "kvstore.h"

#include "hash.h"
#include "inspect.h"
#include "test_util.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 59 + i * 7 + 1);
    return v;
}

bool put(KVStore& s, long key, const Bytes& v) { return s.put(key, v.data(), v.size()); }

bool holds(const KVStore& s, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    return s.get(key, buf.data(), expect.size()) &&
           (expect.empty() || std::memcmp(buf.data(), expect.data(), expect.size()) == 0) &&
           buf[expect.size()] == 0xCD;
}

bool snap_holds(const KVStore& s, const KVStore::Snapshot& snap, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    if (!s.get(snap, key, buf.data(), expect.size())) return false;
    if (!expect.empty() && std::memcmp(buf.data(), expect.data(), expect.size()) != 0) return false;
    if (buf[expect.size()] != 0xCD) return false;
    if (!expect.empty() && s.get(snap, key, buf.data(), expect.size() - 1)) return false;  // exact length
    return true;
}

// Absent under the snapshot: not found even with a buffer larger than any value
// these tests store.
bool snap_missing(const KVStore& s, const KVStore::Snapshot& snap, long key) {
    static std::vector<unsigned char> big(300000);
    return !s.get(snap, key, nullptr, 0) && !s.get(snap, key, big.data(), big.size());
}

std::set<long> snap_keys(const KVStore& s, const KVStore::Snapshot& snap, std::size_t capacity,
                         bool* duplicates = nullptr) {
    std::vector<long> buf(capacity + 4, LONG_MIN + 12345);
    const std::size_t n = s.keys(snap, buf.data(), capacity);
    NOVA_CHECK(n <= capacity);
    for (std::size_t i = n; i < buf.size(); ++i) NOVA_CHECK(buf[i] == LONG_MIN + 12345);  // nothing beyond
    std::set<long> out(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    if (duplicates != nullptr) *duplicates = out.size() != n;
    NOVA_CHECK(out.size() == n);  // no key twice
    return out;
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

NovaKvStats stats(const KVStore& s) { return NovaKvInspector::stats(s); }

// ---- Lock-based baselines: refused, loudly ---------------------------------------------
void test_lock_variants_refuse_snapshots() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(8, 1)));
    bool threw = false;
    try {
        KVStore::Snapshot snap = s.acquire_snapshot();
        (void)snap;
    } catch (const std::logic_error&) {
        threw = true;
    }
    NOVA_CHECK(threw);
    NOVA_CHECK(holds(s, 1, pattern(8, 1)));  // the store itself is unaffected
}

// ---- A snapshot is the state at acquisition, and stays that ----------------------------
void test_snapshot_is_the_state_at_acquisition() {
    KVStore s;
    for (long k = 0; k < 50; ++k) NOVA_CHECK(put(s, k, pattern(40, static_cast<std::uint64_t>(k))));
    KVStore::Snapshot snap = s.acquire_snapshot();

    for (long k = 0; k < 50; k += 2) NOVA_CHECK(put(s, k, pattern(60, 1000 + static_cast<std::uint64_t>(k))));  // updates
    for (long k = 1; k < 50; k += 4) NOVA_CHECK(s.erase(k));                                                    // deletions
    for (long k = 50; k < 80; ++k) NOVA_CHECK(put(s, k, pattern(10, static_cast<std::uint64_t>(k))));           // inserts
    {
        Transaction t(s);
        for (long k = 80; k < 120; ++k) t.put(k, "x", 1);
        const Bytes v = pattern(70000, 5);
        t.put(3, v.data(), v.size());
        NOVA_CHECK(t.commit());
    }
    // Through the snapshot: exactly the 50 original values.
    for (long k = 0; k < 50; ++k) NOVA_CHECK(snap_holds(s, snap, k, pattern(40, static_cast<std::uint64_t>(k))));
    for (long k = 50; k < 120; ++k) NOVA_CHECK(snap_missing(s, snap, k));
    NOVA_CHECK(snap_keys(s, snap, 1000).size() == 50);
    // Without it: the current state.
    NOVA_CHECK(holds(s, 0, pattern(60, 1000)) && holds(s, 3, pattern(70000, 5)));
    unsigned char b[4];
    NOVA_CHECK(!s.get(1, b, 0) && s.get(100, b, 1));
    // A second snapshot is an independent view of the newer state.
    KVStore::Snapshot later = s.acquire_snapshot();
    NOVA_CHECK(snap_holds(s, later, 0, pattern(60, 1000)) && snap_missing(s, later, 1));
    NOVA_CHECK(snap_holds(s, snap, 1, pattern(40, 1)));
    NOVA_CHECK(stats(s).reader_slots_owned == 3);  // two snapshots + this thread's own reader slot
}

// Buffer contracts are those of the ordinary get() (decision D-012).
void test_snapshot_get_buffer_contracts() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(32, 1)));
    NOVA_CHECK(s.put(2, nullptr, 0));
    KVStore::Snapshot snap = s.acquire_snapshot();
    unsigned char buf[64];
    std::memset(buf, 0xAB, sizeof(buf));
    NOVA_CHECK(!s.get(snap, 1, buf, 31));           // too small: false ...
    NOVA_CHECK(!s.get(snap, 1, nullptr, 32));       // null with a length: false
    for (unsigned char c : buf) NOVA_CHECK(c == 0xAB);  // ... and the buffer is untouched
    NOVA_CHECK(s.get(snap, 1, buf, sizeof(buf)));   // larger than needed is fine
    NOVA_CHECK(std::memcmp(buf, pattern(32, 1).data(), 32) == 0 && buf[32] == 0xAB);
    NOVA_CHECK(s.get(snap, 2, nullptr, 0));         // present, empty
    NOVA_CHECK(!s.get(snap, 3, nullptr, 0));        // absent
}

// ---- Move-only ownership; released exactly once ------------------------------------------
void test_move_semantics() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(16, 1)));
    NOVA_CHECK(stats(s).reader_slots_owned == 0);
    {
        KVStore::Snapshot a = s.acquire_snapshot();
        NOVA_CHECK(stats(s).reader_slots_owned == 1);
        NOVA_CHECK(put(s, 1, pattern(16, 2)));

        KVStore::Snapshot b = std::move(a);                 // move construction
        NOVA_CHECK(stats(s).reader_slots_owned == 1);       // transferred, not copied
        NOVA_CHECK(snap_holds(s, b, 1, pattern(16, 1)));
        NOVA_CHECK(snap_missing(s, a, 1));                  // moved-from: inert, safely
        long out[4] = {7, 7, 7, 7};
        NOVA_CHECK(s.keys(a, out, 4) == 0 && out[0] == 7);

        KVStore::Snapshot c = s.acquire_snapshot();         // a view of the newer state
        NOVA_CHECK(stats(s).reader_slots_owned == 2);
        NOVA_CHECK(snap_holds(s, c, 1, pattern(16, 2)));
        c = std::move(b);                                   // move assignment over a live handle:
        NOVA_CHECK(stats(s).reader_slots_owned == 1);       //   c's own pin is released, b's is taken over
        NOVA_CHECK(snap_holds(s, c, 1, pattern(16, 1)));
        NOVA_CHECK(snap_missing(s, b, 1));

        KVStore::Snapshot& alias = c;
        c = std::move(alias);                               // self-assignment keeps the pin
        NOVA_CHECK(stats(s).reader_slots_owned == 1);
        NOVA_CHECK(snap_holds(s, c, 1, pattern(16, 1)));

        a = std::move(c);                                   // into a moved-from handle
        NOVA_CHECK(stats(s).reader_slots_owned == 1);
        NOVA_CHECK(snap_holds(s, a, 1, pattern(16, 1)));
        b = std::move(c);                                   // inert into inert
        NOVA_CHECK(stats(s).reader_slots_owned == 1);
    }                                                       // a releases; b and c are inert
    NOVA_CHECK(stats(s).reader_slots_owned == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));                 // nothing left pinned: everything reclaimable
    check_valid(s);
}

// ---- A handle is valid only for the store it came from ------------------------------------
void test_wrong_store_is_rejected() {
    KVStore a, b;
    NOVA_CHECK(put(a, 1, pattern(16, 1)));
    NOVA_CHECK(put(b, 1, pattern(16, 2)));
    KVStore::Snapshot sa = a.acquire_snapshot();
    unsigned char buf[32];
    std::memset(buf, 0xAB, sizeof(buf));
    NOVA_CHECK(!b.get(sa, 1, buf, sizeof(buf)));  // the key exists in b, but the handle is a's
    for (unsigned char c : buf) NOVA_CHECK(c == 0xAB);
    long out[4] = {7, 7, 7, 7};
    NOVA_CHECK(b.keys(sa, out, 4) == 0 && out[0] == 7);
    NOVA_CHECK(b.index_get(sa, "x", 1, out, 4) == 0);
    NOVA_CHECK(snap_holds(a, sa, 1, pattern(16, 1)));  // still perfectly good for its own store
    NOVA_CHECK(stats(a).reader_slots_owned == 1 && stats(b).reader_slots_owned == 0);
}

// ---- keys() --------------------------------------------------------------------------------
void test_keys_basic() {
    KVStore s;
    {
        KVStore::Snapshot empty = s.acquire_snapshot();
        long out[4] = {7, 7, 7, 7};
        NOVA_CHECK(s.keys(empty, out, 4) == 0 && out[0] == 7);
    }
    std::set<long> expect;
    for (long k = -500; k < 500; ++k) {
        NOVA_CHECK(put(s, k * 7919, pattern(8, static_cast<std::uint64_t>(k + 500))));
        expect.insert(k * 7919);
    }
    for (long k : {LONG_MIN, LONG_MAX, 0L, -1L}) {
        NOVA_CHECK(put(s, k, pattern(4, 1)));
        expect.insert(k);
    }
    KVStore::Snapshot snap = s.acquire_snapshot();
    NOVA_CHECK(snap_keys(s, snap, 5000) == expect);               // exactly the keys, each once
    NOVA_CHECK(snap_keys(s, snap, expect.size()) == expect);      // capacity exactly enough

    // A smaller buffer gets that many distinct, valid keys and nothing is
    // written past it.
    for (std::size_t cap : {std::size_t{1}, std::size_t{10}, expect.size() - 1}) {
        const std::set<long> some = snap_keys(s, snap, cap);
        NOVA_CHECK(some.size() == cap);
        NOVA_CHECK(std::includes(expect.begin(), expect.end(), some.begin(), some.end()));
    }
    // Zero capacity and a null pointer.
    long sentinel[2] = {7, 7};
    NOVA_CHECK(s.keys(snap, sentinel, 0) == 0 && sentinel[0] == 7);
    NOVA_CHECK(s.keys(snap, nullptr, 100) == 0);
    NOVA_CHECK(s.keys(snap, nullptr, 0) == 0);
    // The order is unspecified but repeatable for one snapshot.
    std::vector<long> first(expect.size()), second(expect.size());
    NOVA_CHECK(s.keys(snap, first.data(), first.size()) == expect.size());
    NOVA_CHECK(put(s, 123456789, pattern(4, 2)));
    NOVA_CHECK(s.keys(snap, second.data(), second.size()) == expect.size());
    NOVA_CHECK(first == second);
    // Every enumerated key can be fetched through the same snapshot.
    for (long k : first) NOVA_CHECK(s.get(snap, k, nullptr, 0) == false && !snap_missing(s, snap, k));
}

// Enumeration against a model through collisions, resizing, tombstone rebuilds,
// updates and deletions; several snapshots are kept across the changes and each
// must keep reporting exactly the key set of its own moment.
void test_keys_through_structural_changes() {
    KVStore s;
    // Keys that all collide into two partitions: long probe chains, growth,
    // tombstones and rebuilds happen in those two segments.
    std::vector<long> pool;
    for (long k = 0; pool.size() < 1200; ++k) {
        const std::uint32_t p = nova::partition_of(nova::hash_key(k));
        if (p == 5 || p == 200) pool.push_back(k);
    }
    std::set<long> model;
    struct Held {
        KVStore::Snapshot snap;
        std::set<long> keys;
    };
    std::vector<Held> held;
    auto checkpoint = [&]() { held.push_back(Held{s.acquire_snapshot(), model}); };

    checkpoint();                                                    // empty
    for (std::size_t i = 0; i < 300; ++i) {                          // inserts: several growths
        NOVA_CHECK(put(s, pool[i], pattern(12, i)));
        model.insert(pool[i]);
        if (i % 60 == 59) checkpoint();
    }
    for (std::size_t i = 0; i < 300; i += 2) {                       // updates: no key appears twice
        NOVA_CHECK(put(s, pool[i], pattern(20, 5000 + i)));
    }
    checkpoint();
    for (std::size_t i = 0; i < 300; ++i) {                          // deletions: tombstones
        if (i % 5 != 0) {
            NOVA_CHECK(s.erase(pool[i]));
            model.erase(pool[i]);
        }
        if (i % 75 == 74) checkpoint();
    }
    for (std::size_t i = 300; i < 1200; ++i) {                       // churn: rebuilds reuse and drop tombstones
        NOVA_CHECK(put(s, pool[i], pattern(12, i)));
        model.insert(pool[i]);
        if (i % 3 == 0) {
            NOVA_CHECK(s.erase(pool[i - 100]) == (model.erase(pool[i - 100]) == 1));
        }
        if (i % 150 == 149) checkpoint();
    }
    {                                                                // one transaction: insert + overwrite
        Transaction t(s);
        for (long k = 100000; k < 100300; ++k) {
            t.put(k, "ab", 2);
            model.insert(k);
        }
        for (long k : model) {
            if (k < 100000 && k % 4 == 0) t.put(k, "c", 1);
        }
        NOVA_CHECK(t.commit());
    }
    checkpoint();
    const NovaKvStats st = stats(s);
    NOVA_CHECK(st.segment_growths >= 4 && st.segment_rebuilds >= 2);  // resizing and tombstone rebuilds really happened

    for (Held& h : held) {
        NOVA_CHECK(snap_keys(s, h.snap, 5000) == h.keys);
        for (long k : h.keys) NOVA_CHECK(!snap_missing(s, h.snap, k));
    }
    NOVA_CHECK(stats(s).reader_slots_owned == held.size());
    std::printf("  keys(): %zu snapshots across %llu growths and %llu tombstone rebuilds each report their own key set\n",
                held.size(), static_cast<unsigned long long>(st.segment_growths),
                static_cast<unsigned long long>(st.segment_rebuilds));
    held.clear();
    NOVA_CHECK(stats(s).reader_slots_owned == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// ---- Lifetime: thousands of commits, the cleaner running, chunk IDs -------------------------
// reclaim: 0 epoch-based, 1 disabled, 2 stop-the-world. cleaner: 0 background, 1 manual.
void test_snapshot_survives_reclamation(int reclaim, int cleaner, const char* label) {
    NovaKvInspector::set_default_rcu_options(reclaim, cleaner);
    {
        KVStore s;
        const int nkeys = 24;
        for (long k = 0; k < nkeys; ++k) NOVA_CHECK(put(s, k, pattern(16000, static_cast<std::uint64_t>(k))));
        NovaKvInspector::settle(s);                         // nothing older than the snapshot is pending
        KVStore::Snapshot snap = s.acquire_snapshot();
        const NovaKvStats before = stats(s);

        // 3000 publications over the same keys with large values: without the
        // snapshot every old chunk would be freed and its ID reused many times.
        for (std::uint64_t i = 0; i < 3000; ++i) {
            if (i % 3 == 2) {
                Transaction t(s);
                for (long k = 0; k < 4; ++k) {
                    const Bytes v = pattern(16000, 100000 + i + static_cast<std::uint64_t>(k));
                    t.put((static_cast<long>(i) + k) % nkeys, v.data(), v.size());
                }
                NOVA_CHECK(t.commit());
            } else {
                NOVA_CHECK(put(s, static_cast<long>(i % nkeys), pattern(16000, 100000 + i)));
            }
            if (i % 500 == 499) {
                NovaKvInspector::settle(s);                 // the cleaner tries as hard as it can
                for (long k = 0; k < nkeys; ++k) {
                    NOVA_CHECK(snap_holds(s, snap, k, pattern(16000, static_cast<std::uint64_t>(k))));
                }
            }
        }
        NOVA_CHECK(!NovaKvInspector::settle(s) || reclaim == 1);  // the backlog cannot be emptied while pinned
        const NovaKvStats held = stats(s);
        NOVA_CHECK(held.versions_reclaimed == before.versions_reclaimed);  // nothing retired since is destroyed
        NOVA_CHECK(held.chunks_freed == before.chunks_freed);              // no chunk the snapshot may reach is freed
        NOVA_CHECK(held.chunk_ids_reused == before.chunk_ids_reused);      // and no chunk ID is reused
        NOVA_CHECK(NovaKvInspector::backlog(s) >= 3000);
        for (long k = 0; k < nkeys; ++k) NOVA_CHECK(snap_holds(s, snap, k, pattern(16000, static_cast<std::uint64_t>(k))));
        NOVA_CHECK(snap_keys(s, snap, 100).size() == static_cast<std::size_t>(nkeys));

        {
            KVStore::Snapshot gone = std::move(snap);       // destroyed here: the one and only release
        }
        NOVA_CHECK(stats(s).reader_slots_owned == 0);
        const bool drained = NovaKvInspector::settle(s);
        const NovaKvStats after = stats(s);
        if (reclaim == 1) {
            NOVA_CHECK(after.versions_reclaimed == before.versions_reclaimed);  // disabled: kept until destruction
        } else {
            NOVA_CHECK(drained && NovaKvInspector::backlog(s) == 0);
            NOVA_CHECK(after.chunks_freed > before.chunks_freed + 500);
            for (std::uint64_t i = 0; i < 100; ++i) NOVA_CHECK(put(s, static_cast<long>(i % nkeys), pattern(16000, 900000 + i)));
            NOVA_CHECK(stats(s).chunk_ids_reused > before.chunk_ids_reused);    // only now are IDs reused
        }
        check_valid(s);
        std::printf("  %-28s 3000 publications under one snapshot: %llu versions held back, 0 chunks freed, 0 IDs reused; "
                    "after release %llu chunks freed\n",
                    label, static_cast<unsigned long long>(held.versions_retained),
                    static_cast<unsigned long long>(after.chunks_freed - before.chunks_freed));
    }
    NovaKvInspector::set_default_rcu_options(0, 2);
}

// ---- Threads --------------------------------------------------------------------------------
// Acquired on one thread, moved to a second (the first has exited by then),
// used and destroyed there.
void test_snapshot_moved_between_threads() {
    KVStore s;
    for (long k = 0; k < 30; ++k) NOVA_CHECK(put(s, k, pattern(2000, static_cast<std::uint64_t>(k))));
    std::vector<KVStore::Snapshot> box;
    std::thread acquirer([&]() {
        NOVA_CHECK(holds(s, 0, pattern(2000, 0)));          // this thread also has an ordinary reader slot
        box.push_back(s.acquire_snapshot());
    });
    acquirer.join();                                        // its thread slot is returned; the snapshot's is not
    NOVA_CHECK(stats(s).reader_slots_owned == 1);
    for (std::uint64_t i = 0; i < 2000; ++i) NOVA_CHECK(put(s, static_cast<long>(i % 30), pattern(2000, 5000 + i)));
    std::thread user([&s, snap = std::move(box[0])]() mutable {
        for (long k = 0; k < 30; ++k) NOVA_CHECK(snap_holds(s, snap, k, pattern(2000, static_cast<std::uint64_t>(k))));
        KVStore::Snapshot mine = std::move(snap);
        NOVA_CHECK(snap_keys(s, mine, 100).size() == 30);
    });                                                     // destroyed on that thread
    user.join();
    box.clear();
    NOVA_CHECK(stats(s).reader_slots_owned == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// Short-lived reader threads come and go while a snapshot is held: their slots
// are returned and reused, the snapshot's slot never is.
void test_snapshot_held_while_reader_slots_are_reused() {
    KVStore s;
    for (long k = 0; k < 10; ++k) NOVA_CHECK(put(s, k, pattern(500, static_cast<std::uint64_t>(k))));
    KVStore::Snapshot snap = s.acquire_snapshot();
    const std::uint64_t registrations0 = stats(s).reader_registrations;
    for (int round = 0; round < 200; ++round) {
        NOVA_CHECK(put(s, round % 10, pattern(500, 1000 + static_cast<std::uint64_t>(round))));
        std::thread t([&]() {
            unsigned char b[600];
            for (int i = 0; i < 10; ++i) NOVA_CHECK(s.get(i, b, sizeof(b)));
            KVStore::Snapshot brief = s.acquire_snapshot();  // and short-lived snapshots as well
            NOVA_CHECK(s.get(brief, 0, b, sizeof(b)));
        });
        t.join();
        NOVA_CHECK(stats(s).reader_slots_owned == 1);        // only the long-lived snapshot
    }
    const NovaKvStats st = stats(s);
    NOVA_CHECK(st.reader_registrations == registrations0 + 400);  // 200 thread slots + 200 brief snapshots
    NOVA_CHECK(st.reader_slots == 32);                            // all served from one block: slots were reused
    for (long k = 0; k < 10; ++k) NOVA_CHECK(snap_holds(s, snap, k, pattern(500, static_cast<std::uint64_t>(k))));
}

void test_repeated_creation_and_destruction() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(64, 1)));
    for (int i = 0; i < 50000; ++i) {
        KVStore::Snapshot snap = s.acquire_snapshot();
        if (i % 1000 == 0) {
            NOVA_CHECK(snap_holds(s, snap, 1, pattern(64, static_cast<std::uint64_t>(i == 0 ? 1 : i - 1000 + 2))));
            NOVA_CHECK(put(s, 1, pattern(64, static_cast<std::uint64_t>(i + 2))));
        }
    }
    {
        std::vector<KVStore::Snapshot> many;                // more than one slot block at once
        for (int i = 0; i < 100; ++i) many.push_back(s.acquire_snapshot());
        NOVA_CHECK(stats(s).reader_slots_owned == 100);
    }
    const NovaKvStats st = stats(s);
    NOVA_CHECK(st.reader_slots_owned == 0);
    NOVA_CHECK(st.reader_slots <= 160);                     // no growth with the number of acquisitions
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// Stores are created, used with snapshots, and destroyed after every snapshot
// and thread is gone (the shutdown contract), with a backlog still pending.
void test_shutdown_after_snapshots() {
    NovaKvInspector::set_default_rcu_options(0, 0);  // background cleaner
    for (int round = 0; round < 50; ++round) {
        KVStore s;
        for (long k = 0; k < 20; ++k) NOVA_CHECK(put(s, k, pattern(3000, static_cast<std::uint64_t>(k))));
        {
            KVStore::Snapshot snap = s.acquire_snapshot();
            std::thread reader([&]() {
                for (long k = 0; k < 20; ++k) NOVA_CHECK(snap_holds(s, snap, k, pattern(3000, static_cast<std::uint64_t>(k))));
            });
            for (std::uint64_t i = 0; i < 300; ++i) NOVA_CHECK(put(s, static_cast<long>(i % 20), pattern(3000, 77 + i)));
            reader.join();
        }
    }                                                       // destroyed with whatever the cleaner had not reached
    NovaKvInspector::set_default_rcu_options(0, 2);
}

}  // namespace

int main() {
    if (!NovaKvInspector::is_versioned()) {
        test_lock_variants_refuse_snapshots();
        return nova_test::finish("snapshot_test");
    }
    // Single-threaded parts: the writer runs the reclamation passes itself
    // after each write (the same epoch-based protocol), so counters are exact.
    NovaKvInspector::set_default_rcu_options(0, 2);

    test_snapshot_is_the_state_at_acquisition();
    test_snapshot_get_buffer_contracts();
    test_move_semantics();
    test_wrong_store_is_rejected();
    test_keys_basic();
    test_keys_through_structural_changes();
    test_snapshot_survives_reclamation(0, 1, "epoch, manual passes:");
    test_snapshot_survives_reclamation(0, 0, "epoch, background cleaner:");
    test_snapshot_survives_reclamation(2, 1, "stop-the-world comparator:");
    test_snapshot_survives_reclamation(1, 1, "reclamation disabled:");
    test_snapshot_moved_between_threads();
    test_snapshot_held_while_reader_slots_are_reused();
    test_repeated_creation_and_destruction();
    test_shutdown_after_snapshots();
    return nova_test::finish("snapshot_test");
}
