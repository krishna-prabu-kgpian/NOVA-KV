// txn_concurrency_test.cpp -- Part C.1 under concurrency (rcu hook build).
//
// Three different properties are kept apart here, because they are different:
//
//   A. POINT-GET LINEARIZABILITY. One ordinary get() returns one complete value
//      that was the key's value at some moment during the call. Two ordinary
//      get() calls are two moments: they may see two different versions.
//   B. ATOMIC MULTI-KEY PUBLICATION. All writes of one Transaction become
//      visible in one step. Observable only by a reader that looks at several
//      keys in ONE version, i.e. through one Snapshot.
//   C. A STABLE SNAPSHOT. Every read through one Snapshot handle sees the same
//      version, however many commits happen in between.
//
// B and C are asserted through explicit Snapshots. Ordinary gets are asserted
// only for A; how often two ordinary gets disagree about a transaction is
// counted and printed, not asserted (it is allowed).
//
// Parts 1 and 2 are deterministic: a thread is parked at an exact step with the
// pause points of src/test_points.h (mutex + condition variable, no sleeps).
// Parts 3 to 5 are stress runs with the background cleaner.
#include "kvstore.h"

#include "inspect.h"
#include "test_points.h"
#include "test_util.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "txn_concurrency_test must be built with NOVA_KV_TEST_HOOKS"
#endif

using nova::TestPoint;

namespace {

// ---- Records that carry their own generation -------------------------------------------
// Every value written by generation g of a group says so, and its payload is a
// function of (g, key): a torn or mixed value is detectable from the bytes.
struct Header {
    std::uint64_t generation;
    long key;
    std::uint32_t length;   // total record length
    std::uint32_t tag;      // which writer thread produced it
};

unsigned char fill_byte(std::uint64_t g, long key) {
    return static_cast<unsigned char>(g * 31 + static_cast<std::uint64_t>(key) * 7 + 5);
}

std::size_t record_length(std::uint64_t g, long key) {
    return sizeof(Header) + (g * 13 + static_cast<std::uint64_t>(key) * 5) % 200;
}

std::vector<unsigned char> make_record(std::uint64_t g, long key, std::uint32_t tag = 0) {
    std::vector<unsigned char> v(record_length(g, key), fill_byte(g, key));
    const Header h{g, key, static_cast<std::uint32_t>(v.size()), tag};
    std::memcpy(v.data(), &h, sizeof(h));
    return v;
}

// 0 if `buf` (as filled by a successful get into a 0x5A-prefilled buffer) is a
// complete record for `key`; its header is returned.
bool record_ok(const std::vector<unsigned char>& buf, long key, Header* out) {
    Header h;
    std::memcpy(&h, buf.data(), sizeof(h));
    if (h.key != key || h.length != record_length(h.generation, key) || h.length > buf.size()) return false;
    const unsigned char want = fill_byte(h.generation, key);
    for (std::size_t i = sizeof(Header); i < h.length; ++i) {
        if (buf[i] != want) return false;
    }
    if (h.length < buf.size() && buf[h.length] != 0x5A) return false;  // nothing copied beyond it
    *out = h;
    return true;
}

bool read_plain(const KVStore& s, long key, std::vector<unsigned char>& buf, Header* out) {
    std::memset(buf.data(), 0x5A, buf.size());
    return s.get(key, buf.data(), buf.size()) && record_ok(buf, key, out);
}

bool read_snap(const KVStore& s, const KVStore::Snapshot& snap, long key, std::vector<unsigned char>& buf,
               Header* out) {
    std::memset(buf.data(), 0x5A, buf.size());
    return s.get(snap, key, buf.data(), buf.size()) && record_ok(buf, key, out);
}

// Writes generation g to keys [first, first + count) as ONE transaction.
bool commit_group(KVStore& s, long first, int count, std::uint64_t g, std::uint32_t tag = 0) {
    Transaction t(s);
    for (long k = first; k < first + count; ++k) {
        const std::vector<unsigned char> v = make_record(g, k, tag);
        t.put(k, v.data(), v.size());
    }
    return t.commit();
}

// Reads the whole group through `snap`. Returns false if any key is missing or
// damaged; *uniform says whether all keys carry one generation (and tag).
bool group_under_snapshot(const KVStore& s, const KVStore::Snapshot& snap, long first, int count,
                          std::vector<unsigned char>& buf, Header* head, bool* uniform) {
    *uniform = true;
    for (long k = first; k < first + count; ++k) {
        Header h;
        if (!read_snap(s, snap, k, buf, &h)) return false;
        if (k == first) *head = h;
        else if (h.generation != head->generation || h.tag != head->tag) *uniform = false;
    }
    return true;
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// ---- Pause-point scheduler --------------------------------------------------------------
// One thread (the "subject") stops once at each point of a chosen set, in the
// order it reaches them; the controlling thread waits for it to be parked, does its
// work, and lets it go on to the next one.
class Stepper {
public:
    void arm(std::vector<TestPoint> points) {
        std::lock_guard<std::mutex> lk(m_);
        points_ = std::move(points);
        parked_ = false;
        resume_ = 0;
        seen_ = 0;
    }
    void bind_this_thread() { tl_subject = this; }
    TestPoint wait_until_parked() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return parked_; });
        return at_;
    }
    void resume() {
        {
            std::lock_guard<std::mutex> lk(m_);
            ++resume_;
            parked_ = false;
        }
        cv_.notify_all();
    }
    void disarm() {
        std::lock_guard<std::mutex> lk(m_);
        points_.clear();
    }
    static void install() { nova::test_hooks::set_point_hook(&Stepper::hook, nullptr); }

private:
    static void hook(TestPoint point, void*) {
        Stepper* st = tl_subject;
        if (st == nullptr) return;
        std::unique_lock<std::mutex> lk(st->m_);
        // Each armed point parks the subject once (a pin loop that has to
        // repeat passes the same point again and must not stop there twice).
        bool wanted = false;
        for (std::size_t i = 0; i < st->points_.size(); ++i) {
            if (st->points_[i] == point) {
                st->points_.erase(st->points_.begin() + static_cast<std::ptrdiff_t>(i));
                wanted = true;
                break;
            }
        }
        if (!wanted) return;
        const int ticket = ++st->seen_;
        st->at_ = point;
        st->parked_ = true;
        st->cv_.notify_all();
        st->cv_.wait(lk, [st, ticket] { return st->resume_ >= ticket; });
    }
    static thread_local Stepper* tl_subject;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<TestPoint> points_;
    TestPoint at_ = TestPoint::ReaderBeforeAnnounce;
    bool parked_ = false;
    int resume_ = 0;
    int seen_ = 0;
};
thread_local Stepper* Stepper::tl_subject = nullptr;

// ---- 1. A commit of N keys is seen as none, then all ---------------------------------------
// The writer is parked (a) with the new version completely built but not
// published, and (b) right after the root swap, before the old version has even
// been queued for reclamation.
void test_none_then_all(int n) {
    NovaKvInspector::set_default_rcu_options(0, 1);  // manual passes: the test is the cleaner
    KVStore s;
    NOVA_CHECK(commit_group(s, 0, n, 1));
    std::vector<unsigned char> buf(4096);
    Stepper stepper;
    stepper.arm({TestPoint::WriterBeforePublish, TestPoint::WriterAfterPublish});
    bool committed = false;
    std::thread writer([&]() {
        stepper.bind_this_thread();
        committed = commit_group(s, 0, n, 2);
    });

    // (a) Built, not published: nobody can see any of it.
    NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterBeforePublish);
    KVStore::Snapshot before = s.acquire_snapshot();
    Header h{};
    bool uniform = false;
    NOVA_CHECK(group_under_snapshot(s, before, 0, n, buf, &h, &uniform) && uniform && h.generation == 1);
    for (long k = 0; k < n; ++k) NOVA_CHECK(read_plain(s, k, buf, &h) && h.generation == 1);
    const std::uint64_t published_before = NovaKvInspector::stats(s).versions_published;
    stepper.resume();

    // (b) Published: everybody who looks now sees all of it.
    NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterAfterPublish);
    KVStore::Snapshot after = s.acquire_snapshot();
    NOVA_CHECK(group_under_snapshot(s, after, 0, n, buf, &h, &uniform) && uniform && h.generation == 2);
    for (long k = 0; k < n; ++k) NOVA_CHECK(read_plain(s, k, buf, &h) && h.generation == 2);
    // The earlier snapshot is still entirely the old state, and stays so while
    // the cleaner tries (the old version is not even queued yet).
    for (int i = 0; i < 200; ++i) NovaKvInspector::settle(s);
    NOVA_CHECK(group_under_snapshot(s, before, 0, n, buf, &h, &uniform) && uniform && h.generation == 1);
    stepper.resume();
    writer.join();
    stepper.disarm();
    NOVA_CHECK(committed);
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == published_before + 1);  // one swap for n keys

    // Many more commits and passes: each snapshot keeps its own state.
    for (std::uint64_t g = 3; g < 300; ++g) {
        NOVA_CHECK(commit_group(s, 0, n, g));
        if (g % 50 == 0) NovaKvInspector::settle(s);
    }
    NOVA_CHECK(group_under_snapshot(s, before, 0, n, buf, &h, &uniform) && uniform && h.generation == 1);
    NOVA_CHECK(group_under_snapshot(s, after, 0, n, buf, &h, &uniform) && uniform && h.generation == 2);
    {
        KVStore::Snapshot a = std::move(before), b = std::move(after);
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
    std::printf("  %3d-key commit: none visible before the swap, all after it; older snapshot unchanged\n", n);
    NovaKvInspector::set_default_rcu_options(0, 0);
}

// ---- 2. A snapshot being ACQUIRED while transactions commit and the cleaner runs ------------
// The acquiring thread is parked inside acquire_snapshot() at each step of the
// pin protocol. Whatever it ends up with must be one complete generation, and
// if it had already loaded the root it must be exactly the state of that moment.
void test_acquisition_parked(TestPoint point, const char* label) {
    NovaKvInspector::set_default_rcu_options(0, 1);
    KVStore s;
    const int n = 16;
    NOVA_CHECK(commit_group(s, 0, n, 1));
    NovaKvInspector::settle(s);
    Stepper stepper;
    stepper.arm({point});
    std::uint64_t seen_generation = 0;
    bool complete = false, uniform = false;
    std::thread reader([&]() {
        stepper.bind_this_thread();
        std::vector<unsigned char> buf(4096);
        KVStore::Snapshot snap = s.acquire_snapshot();       // parks inside
        Header h{};
        complete = group_under_snapshot(s, snap, 0, n, buf, &h, &uniform);
        seen_generation = h.generation;
        // Stable: a second pass over the same handle gives the same answer.
        Header again{};
        bool u2 = false;
        complete = complete && group_under_snapshot(s, snap, 0, n, buf, &again, &u2) && u2 &&
                   again.generation == h.generation;
    });
    NOVA_CHECK(stepper.wait_until_parked() == point);
    const std::uint64_t last = 800;
    for (std::uint64_t g = 2; g <= last; ++g) {
        NOVA_CHECK(commit_group(s, 0, n, g));
        if (g % 20 == 0) NovaKvInspector::settle(s);         // the cleaner works throughout
    }
    NovaKvInspector::settle(s);
    const std::uint64_t reclaimed_while_parked = NovaKvInspector::stats(s).versions_reclaimed;
    stepper.resume();
    reader.join();
    stepper.disarm();
    NOVA_CHECK(complete && uniform);
    if (point == TestPoint::ReaderAfterRootLoad) {
        NOVA_CHECK(seen_generation == 1);                    // it had the old root: that is what it reads
        NOVA_CHECK(reclaimed_while_parked <= 1);             // and nothing retired since could be destroyed
    } else {
        NOVA_CHECK(seen_generation == last);                 // not yet loaded: it gets the newest state
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
    std::printf("  acquire_snapshot parked %-22s 799 transactions meanwhile; snapshot reads generation %llu, complete\n",
                label, static_cast<unsigned long long>(seen_generation));
    NovaKvInspector::set_default_rcu_options(0, 0);
}

// ---- 3. Stress: one writer, snapshot readers, point readers, thread churn -------------------
void stress_atomicity(int group_size) {
    KVStore s;                                    // background cleaner
    const int groups = 6;
    // Fewer generations for larger transactions: the same order of work each.
    const std::uint64_t generations =
        group_size == 1 ? 4000 : (group_size == 4 ? 2000 : (group_size == 16 ? 800 : 300));
    for (int g = 0; g < groups; ++g) NOVA_CHECK(commit_group(s, static_cast<long>(g) * 1000, group_size, 1));

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> partial{0}, damaged{0}, unstable{0}, backwards{0}, torn{0};
    std::atomic<std::uint64_t> snapshots{0}, point_reads{0}, plain_pairs_differ{0};
    std::atomic<std::uint64_t> newest{1};         // generation the writer has finished committing everywhere

    std::vector<std::thread> threads;
    // Snapshot readers: properties B and C.
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<unsigned char> buf(4096);
            std::uint64_t last_seen[6] = {0, 0, 0, 0, 0, 0};
            std::uint64_t n = 0;
            while (!done.load() || n < 200) {
                const int g = static_cast<int>((n + static_cast<std::uint64_t>(t)) % groups);
                const long first = static_cast<long>(g) * 1000;
                const std::uint64_t floor = newest.load();   // committed before this snapshot exists
                KVStore::Snapshot snap = s.acquire_snapshot();
                Header h{}, again{};
                bool uniform = false, uniform2 = false;
                if (!group_under_snapshot(s, snap, first, group_size, buf, &h, &uniform)) damaged.fetch_add(1);
                else if (!uniform) partial.fetch_add(1);     // B: some keys of a transaction, not all
                if (!group_under_snapshot(s, snap, first, group_size, buf, &again, &uniform2) ||
                    again.generation != h.generation) {
                    unstable.fetch_add(1);                   // C: the same handle changed its mind
                }
                if (h.generation < floor) backwards.fetch_add(1);      // older than a completed commit
                if (h.generation < last_seen[g]) backwards.fetch_add(1);
                last_seen[g] = h.generation;
                long keys[640];
                if (s.keys(snap, keys, 640) != static_cast<std::size_t>(groups * group_size)) partial.fetch_add(1);
                ++n;
            }
            snapshots.fetch_add(n);
        });
    }
    // Point readers: property A only.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<unsigned char> buf(4096);
            std::uint64_t n = 0, differ = 0;
            std::uint64_t last_first = 0;
            while (!done.load() || n < 2000) {
                const long first = static_cast<long>((n + static_cast<std::uint64_t>(t)) % groups) * 1000;
                Header a{}, b{};
                if (!read_plain(s, first, buf, &a)) torn.fetch_add(1);
                if (group_size > 1) {
                    if (!read_plain(s, first + group_size - 1, buf, &b)) torn.fetch_add(1);
                    else if (b.generation < a.generation) backwards.fetch_add(1);  // later read, older state
                    if (b.generation != a.generation) ++differ;  // two gets, two versions: allowed
                }
                if (first == 0) {
                    if (a.generation < last_first) backwards.fetch_add(1);
                    last_first = a.generation;
                }
                ++n;
            }
            point_reads.fetch_add(n);
            plain_pairs_differ.fetch_add(differ);
        });
    }
    // Churn: short-lived threads that register, take a snapshot, and leave.
    threads.emplace_back([&]() {
        while (!done.load()) {
            std::thread brief([&]() {
                std::vector<unsigned char> buf(4096);
                KVStore::Snapshot snap = s.acquire_snapshot();
                Header h{};
                bool uniform = false;
                if (!group_under_snapshot(s, snap, 0, group_size, buf, &h, &uniform)) damaged.fetch_add(1);
                else if (!uniform) partial.fetch_add(1);
                if (!read_plain(s, 0, buf, &h)) torn.fetch_add(1);
            });
            brief.join();
        }
    });

    bool writes_ok = true;
    for (std::uint64_t gen = 2; gen <= generations; ++gen) {
        for (int g = 0; g < groups; ++g) {
            writes_ok = writes_ok && commit_group(s, static_cast<long>(g) * 1000, group_size, gen);
        }
        newest.store(gen);
    }
    done.store(true);
    for (auto& t : threads) t.join();

    NOVA_CHECK(writes_ok);
    NOVA_CHECK(partial.load() == 0);     // B
    NOVA_CHECK(unstable.load() == 0);    // C
    NOVA_CHECK(torn.load() == 0);        // A
    NOVA_CHECK(damaged.load() == 0);
    NOVA_CHECK(backwards.load() == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.reader_slots_owned == 0);
    NOVA_CHECK(st.versions_published == static_cast<std::uint64_t>(groups) * generations);  // one per transaction
    check_valid(s);
    std::printf("  %3d-key transactions: %llu commits, %llu snapshots (0 partial, 0 unstable), %llu point reads (0 torn); "
                "pairs of ordinary gets that saw two versions: %llu (allowed)\n",
                group_size, static_cast<unsigned long long>(st.versions_published),
                static_cast<unsigned long long>(snapshots.load()), static_cast<unsigned long long>(point_reads.load()),
                static_cast<unsigned long long>(plain_pairs_differ.load()));
}

// ---- 4. Several threads writing: serialized by the store -------------------------------------
// put(), erase() and Transaction::commit() from four threads at once. The store
// promises that they run one at a time, each as one atomic publication. So:
//   - a group that all four threads overwrite must always be, under any
//     snapshot, entirely one thread's write of one generation;
//   - each thread's private keys must end exactly as that thread left them;
//   - the number of publications equals the number of successful writes.
void stress_serialized_writers() {
    KVStore s;
    const int kWriters = 4, kGroup = 8, kRounds = 1500;
    NOVA_CHECK(commit_group(s, 0, kGroup, 1, 99));
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> mixed{0}, damaged{0}, publications{0};
    std::thread observer([&]() {
        std::vector<unsigned char> buf(4096);
        while (!done.load()) {
            KVStore::Snapshot snap = s.acquire_snapshot();
            Header h{};
            bool uniform = false;
            if (!group_under_snapshot(s, snap, 0, kGroup, buf, &h, &uniform)) damaged.fetch_add(1);
            else if (!uniform) mixed.fetch_add(1);
        }
    });
    std::vector<std::thread> writers;
    std::vector<std::vector<std::uint64_t>> final_generation(kWriters, std::vector<std::uint64_t>(20, 0));
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w]() {
            const long base = 100000 + static_cast<long>(w) * 1000;   // this thread's private keys
            std::uint64_t ok = 0;
            for (int r = 1; r <= kRounds; ++r) {
                const std::uint64_t g = static_cast<std::uint64_t>(r);
                // The shared group, as one transaction.
                if (commit_group(s, 0, kGroup, g, static_cast<std::uint32_t>(w))) ++ok;
                // Private keys: a put, sometimes an erase, sometimes a transaction.
                const long k = base + r % 20;
                const std::vector<unsigned char> v = make_record(g, k, static_cast<std::uint32_t>(w));
                if (s.put(k, v.data(), v.size())) ++ok;
                final_generation[static_cast<std::size_t>(w)][static_cast<std::size_t>(r % 20)] = g;
                if (r % 7 == 0) {
                    const long victim = base + (r + 3) % 20;
                    if (s.erase(victim)) {
                        ++ok;
                        final_generation[static_cast<std::size_t>(w)][static_cast<std::size_t>((r + 3) % 20)] = 0;
                    }
                }
                if (r % 5 == 0) {
                    Transaction t(s);
                    for (int i = 0; i < 4; ++i) {
                        const long tk = base + (r + i) % 20;
                        const std::vector<unsigned char> tv = make_record(g + 1000000, tk, static_cast<std::uint32_t>(w));
                        t.put(tk, tv.data(), tv.size());
                        final_generation[static_cast<std::size_t>(w)][static_cast<std::size_t>((r + i) % 20)] = g + 1000000;
                    }
                    if (t.commit()) ++ok;
                }
            }
            publications.fetch_add(ok);
        });
    }
    for (auto& t : writers) t.join();
    done.store(true);
    observer.join();

    NOVA_CHECK(mixed.load() == 0 && damaged.load() == 0);
    std::vector<unsigned char> buf(4096);
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < 20; ++i) {
            const long k = 100000 + static_cast<long>(w) * 1000 + i;
            const std::uint64_t want = final_generation[static_cast<std::size_t>(w)][static_cast<std::size_t>(i)];
            Header h;
            if (want == 0) {
                NOVA_CHECK(!s.get(k, buf.data(), buf.size()));
            } else {
                NOVA_CHECK(read_plain(s, k, buf, &h) && h.generation == want && h.tag == static_cast<std::uint32_t>(w));
            }
        }
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.versions_published == publications.load() + 1);   // + the initial group
    NOVA_CHECK(st.commits_failed == 0);
    check_valid(s);
    std::printf("  4 writer threads: %llu publications, each atomic; shared 8-key group never mixed under a snapshot\n",
                static_cast<unsigned long long>(st.versions_published));
}

// ---- 5. Snapshots released while the cleaner is busy -----------------------------------------
// Snapshots of very different ages are destroyed from several threads while the
// writer keeps the cleaner fully occupied with large values.
void stress_release_during_cleaning() {
    KVStore s;
    s.set_reclaim_batch(4);
    const int n = 4;
    auto big = [](std::uint64_t g, long k) {
        std::vector<unsigned char> v(20000, static_cast<unsigned char>(g * 3 + static_cast<std::uint64_t>(k)));
        std::memcpy(v.data(), &g, sizeof(g));
        return v;
    };
    auto commit_big = [&](std::uint64_t g) {
        Transaction t(s);
        for (long k = 0; k < n; ++k) {
            const std::vector<unsigned char> v = big(g, k);
            t.put(k, v.data(), v.size());
        }
        return t.commit();
    };
    NOVA_CHECK(commit_big(1));
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> wrong{0}, taken{0};
    std::vector<std::thread> holders;
    for (int t = 0; t < 4; ++t) {
        holders.emplace_back([&, t]() {
            std::vector<unsigned char> buf(20000);
            std::vector<KVStore::Snapshot> kept;
            std::uint64_t n_taken = 0;
            while (!done.load()) {
                kept.push_back(s.acquire_snapshot());
                ++n_taken;
                // Check the newest and the oldest handle this thread holds.
                for (const KVStore::Snapshot* snap : {&kept.front(), &kept.back()}) {
                    std::uint64_t g0 = 0;
                    for (long k = 0; k < n; ++k) {
                        if (!s.get(*snap, k, buf.data(), buf.size())) {
                            wrong.fetch_add(1);
                            continue;
                        }
                        std::uint64_t g;
                        std::memcpy(&g, buf.data(), sizeof(g));
                        if (k == 0) g0 = g;
                        if (g != g0 || buf != big(g, k)) wrong.fetch_add(1);
                    }
                }
                // Release in different orders on different threads.
                if (kept.size() > static_cast<std::size_t>(3 + t)) {
                    if (t % 2 == 0) kept.erase(kept.begin());
                    else kept.pop_back();
                }
                if (n_taken % 64 == 0) kept.clear();
            }
            taken.fetch_add(n_taken);
        });
    }
    bool ok = true;
    for (std::uint64_t g = 2; g <= 3000; ++g) ok = ok && commit_big(g);
    done.store(true);
    for (auto& t : holders) t.join();
    NOVA_CHECK(ok);
    NOVA_CHECK(wrong.load() == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.reader_slots_owned == 0);
    NOVA_CHECK(st.reclaim_max_versions_in_pass <= 4);
    NOVA_CHECK(st.chunks_freed > 100);
    check_valid(s);
    std::printf("  release during cleaning: %llu snapshots taken and released by 4 threads over 2999 commits; "
                "%llu chunks freed, %llu IDs reused, 0 wrong reads\n",
                static_cast<unsigned long long>(taken.load()), static_cast<unsigned long long>(st.chunks_freed),
                static_cast<unsigned long long>(st.chunk_ids_reused));
}

}  // namespace

int main() {
    Stepper::install();
    for (int n : {1, 4, 16, 100}) test_none_then_all(n);
    test_acquisition_parked(TestPoint::ReaderBeforeAnnounce, "before announcing:");
    test_acquisition_parked(TestPoint::ReaderAfterAnnounce, "after announcing:");
    test_acquisition_parked(TestPoint::ReaderAfterValidate, "after validating:");
    test_acquisition_parked(TestPoint::ReaderAfterRootLoad, "after loading the root:");
    for (int n : {1, 4, 16, 100}) stress_atomicity(n);
    stress_serialized_writers();
    stress_release_during_cleaning();

    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    return nova_test::finish("txn_concurrency_test");
}
