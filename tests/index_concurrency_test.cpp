// index_concurrency_test.cpp -- Part C.2: primary data and secondary index are
// published together, and read together (rcu hook build).
//
// The property under test, at every point where anything can be observed:
//
//     in ONE version, the index describes exactly that version's rows.
//
// A reader that uses one version (one index_get call, or any number of calls
// through one Snapshot) must therefore never see a new row with an old index
// entry, an old row with a new index entry, a deleted row still in the index,
// part of a transaction, or an index that is still being built.
//
// Parts 1-3 are deterministic: a thread is parked at a named step (mutex and
// condition variable; no sleeps). Parts 4-5 are stress runs with the background
// cleaner.
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
#error "index_concurrency_test must be built with NOVA_KV_TEST_HOOKS"
#endif

using nova::TestPoint;

namespace {

// ---- Rows: {generation, key, field}. The indexed field is a function of (group,
// generation), so all keys a group holds at one generation share one value.
struct Row {
    std::uint64_t generation;
    long key;
    unsigned char field[8];
    std::uint32_t check;
    std::uint32_t pad;
};
const std::size_t kFieldOff = offsetof(Row, field), kFieldLen = 8;

void field_of(std::uint64_t group, std::uint64_t generation, unsigned char* out) {
    const std::uint64_t x = group * 0x9E3779B97F4A7C15ULL + generation * 0xC2B2AE3D27D4EB4FULL + 1;
    std::memcpy(out, &x, 8);
    out[3] = 0;  // an embedded zero byte
}

std::uint32_t row_check(const Row& r) {
    std::uint64_t x = r.generation * 31 + static_cast<std::uint64_t>(r.key) * 17;
    for (unsigned char c : r.field) x = x * 131 + c;
    return static_cast<std::uint32_t>(x ^ (x >> 32));
}

Row make_row(std::uint64_t group, std::uint64_t generation, long key) {
    Row r;
    std::memset(&r, 0, sizeof(r));
    r.generation = generation;
    r.key = key;
    field_of(group, generation, r.field);
    r.check = row_check(r);
    return r;
}

// Group g owns keys g * 1000 + [0, size).
bool commit_group(KVStore& s, std::uint64_t group, int size, std::uint64_t generation) {
    Transaction t(s);
    for (int i = 0; i < size; ++i) {
        const long key = static_cast<long>(group) * 1000 + i;
        const Row r = make_row(group, generation, key);
        t.put(key, &r, sizeof(r));
    }
    return t.commit();
}

// What one version says about a group, through the index. `snap` null = the
// current version in ONE index_get call.
struct View {
    int matches = 0;        // keys the index returned for (group, generation)
    bool exact = false;     // exactly the group's keys, ascending
};

View probe(const KVStore& s, const KVStore::Snapshot* snap, std::uint64_t group, int size, std::uint64_t generation) {
    unsigned char f[8];
    field_of(group, generation, f);
    long keys[256];
    View v;
    v.matches = snap != nullptr ? s.index_get(*snap, f, kFieldLen, keys, 256) : s.index_get(f, kFieldLen, keys, 256);
    v.exact = v.matches == size;
    for (int i = 0; i < v.matches && v.exact; ++i) v.exact = keys[i] == static_cast<long>(group) * 1000 + i;
    return v;
}

// Under one snapshot: the generation of the group's first row, and whether the
// rows AND the index of that same version agree completely:
//   - every row of the group is intact and of that generation;
//   - the index returns exactly the group's keys for that generation's field;
//   - and nothing for the neighbouring generations.
bool snapshot_consistent(const KVStore& s, const KVStore::Snapshot& snap, std::uint64_t group, int size,
                         std::uint64_t* generation) {
    Row first;
    if (!s.get(snap, static_cast<long>(group) * 1000, &first, sizeof(first))) return false;
    *generation = first.generation;
    for (int i = 0; i < size; ++i) {
        Row r;
        const long key = static_cast<long>(group) * 1000 + i;
        if (!s.get(snap, key, &r, sizeof(r))) return false;
        if (r.key != key || r.generation != first.generation || r.check != row_check(r)) return false;
    }
    if (!probe(s, &snap, group, size, first.generation).exact) return false;
    if (probe(s, &snap, group, size, first.generation + 1).matches != 0) return false;
    if (first.generation > 0 && probe(s, &snap, group, size, first.generation - 1).matches != 0) return false;
    return true;
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// ---- Pause-point scheduler (one subject thread; each armed point parks it once) ---------------
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

// ---- 1. An indexed multi-key transaction: index and rows change in the same swap ---------------
void test_indexed_transaction_publication(int size) {
    NovaKvInspector::set_default_rcu_options(0, 1);  // manual passes
    {
        KVStore s;
        NOVA_CHECK(s.create_index(kFieldOff, kFieldLen));
        NOVA_CHECK(commit_group(s, 1, size, 1));
        Stepper stepper;
        stepper.arm({TestPoint::WriterBeforePublish, TestPoint::WriterAfterPublish});
        bool committed = false;
        std::thread writer([&]() {
            stepper.bind_this_thread();
            committed = commit_group(s, 1, size, 2);     // every key's indexed field changes
        });

        // (a) New rows and new index segments are built; nothing is published.
        NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterBeforePublish);
        KVStore::Snapshot before = s.acquire_snapshot();
        std::uint64_t g = 0;
        NOVA_CHECK(snapshot_consistent(s, before, 1, size, &g) && g == 1);
        NOVA_CHECK(probe(s, nullptr, 1, size, 1).exact);           // the current index: old entries, all of them
        NOVA_CHECK(probe(s, nullptr, 1, size, 2).matches == 0);    // and none of the new ones
        const NovaKvStats st_before = NovaKvInspector::stats(s);
        stepper.resume();

        // (b) Published: the new rows and the new index entries, together.
        NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterAfterPublish);
        KVStore::Snapshot after = s.acquire_snapshot();
        NOVA_CHECK(snapshot_consistent(s, after, 1, size, &g) && g == 2);
        NOVA_CHECK(probe(s, nullptr, 1, size, 2).exact);
        NOVA_CHECK(probe(s, nullptr, 1, size, 1).matches == 0);    // no old entry lingers in the current index
        // The earlier snapshot still has the old rows WITH the old index.
        for (int i = 0; i < 100; ++i) NovaKvInspector::settle(s);
        NOVA_CHECK(snapshot_consistent(s, before, 1, size, &g) && g == 1);
        stepper.resume();
        writer.join();
        stepper.disarm();
        NOVA_CHECK(committed);
        NOVA_CHECK(NovaKvInspector::stats(s).versions_published == st_before.versions_published + 1);  // ONE swap

        for (std::uint64_t gen = 3; gen < 200; ++gen) {
            NOVA_CHECK(commit_group(s, 1, size, gen));
            if (gen % 40 == 0) NovaKvInspector::settle(s);
        }
        NOVA_CHECK(snapshot_consistent(s, before, 1, size, &g) && g == 1);
        NOVA_CHECK(snapshot_consistent(s, after, 1, size, &g) && g == 2);
        check_valid(s);                                            // index == rows in every retained version
        { KVStore::Snapshot a = std::move(before), b = std::move(after); }
        NOVA_CHECK(NovaKvInspector::settle(s));
        check_valid(s);
        std::printf("  %3d-key indexed transaction: before the swap old rows + old index; after it new rows + new index; one publication\n", size);
    }
    NovaKvInspector::set_default_rcu_options(0, 0);
}

// ---- 2. create_index: invisible until complete -----------------------------------------------------
void test_index_creation_is_atomic() {
    NovaKvInspector::set_default_rcu_options(0, 1);
    {
        KVStore s;
        const int groups = 40, size = 8;
        for (int grp = 0; grp < groups; ++grp) NOVA_CHECK(commit_group(s, static_cast<std::uint64_t>(grp), size, 5));
        NovaKvInspector::settle(s);
        KVStore::Snapshot before_creation = s.acquire_snapshot();
        Stepper stepper;
        stepper.arm({TestPoint::WriterDuringIndexBuild, TestPoint::WriterBeforePublish, TestPoint::WriterAfterPublish});
        bool created = false;
        std::thread writer([&]() {
            stepper.bind_this_thread();
            created = s.create_index(kFieldOff, kFieldLen);
        });
        auto nothing_visible = [&]() {
            bool none = !NovaKvInspector::stats(s).indexed;
            for (int grp = 0; grp < groups; ++grp) none = none && probe(s, nullptr, static_cast<std::uint64_t>(grp), size, 5).matches == 0;
            KVStore::Snapshot now = s.acquire_snapshot();
            for (int grp = 0; grp < groups; ++grp) none = none && probe(s, &now, static_cast<std::uint64_t>(grp), size, 5).matches == 0;
            Row r;
            none = none && s.get(7000, &r, sizeof(r)) && r.generation == 5;    // the data is readable throughout
            return none;
        };
        // (a) The writer is in the middle of building index segments.
        NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterDuringIndexBuild);
        NOVA_CHECK(nothing_visible());
        stepper.resume();
        // (b) The index is completely built, and still not published.
        NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterBeforePublish);
        NOVA_CHECK(nothing_visible());
        KVStore::Snapshot just_before = s.acquire_snapshot();
        stepper.resume();
        // (c) Published: complete at once, for every value that was stored.
        NOVA_CHECK(stepper.wait_until_parked() == TestPoint::WriterAfterPublish);
        KVStore::Snapshot just_after = s.acquire_snapshot();
        bool complete = true;
        std::uint64_t g = 0;
        for (int grp = 0; grp < groups; ++grp) {
            complete = complete && probe(s, nullptr, static_cast<std::uint64_t>(grp), size, 5).exact;
            complete = complete && snapshot_consistent(s, just_after, static_cast<std::uint64_t>(grp), size, &g);
        }
        NOVA_CHECK(complete);
        stepper.resume();
        writer.join();
        stepper.disarm();
        NOVA_CHECK(created);
        // Snapshots taken before the publication keep a version without an
        // index, whatever happens later.
        for (std::uint64_t gen = 6; gen < 60; ++gen) NOVA_CHECK(commit_group(s, 3, size, gen));
        NovaKvInspector::settle(s);
        for (const KVStore::Snapshot* old : {&before_creation, &just_before}) {
            for (int grp = 0; grp < groups; ++grp) NOVA_CHECK(probe(s, old, static_cast<std::uint64_t>(grp), size, 5).matches == 0);
            Row r;
            NOVA_CHECK(s.get(*old, 3000, &r, sizeof(r)) && r.generation == 5);
        }
        NOVA_CHECK(snapshot_consistent(s, just_after, 3, size, &g) && g == 5);
        check_valid(s);
        std::printf("  create_index over 320 rows: no entry visible while being built or when built; all visible after the swap\n");
    }
    NovaKvInspector::set_default_rcu_options(0, 0);
}

// ---- 3. A reader parked inside index_get ------------------------------------------------------------
// Parked after loading the root: it must answer from that version, index and
// rows alike, although 600 indexed transactions and cleaner passes happen
// before it continues. Parked before loading it: it answers from the newest.
void test_reader_parked_in_index_get(TestPoint point, const char* label) {
    NovaKvInspector::set_default_rcu_options(0, 1);
    {
        KVStore s;
        const int size = 16;
        NOVA_CHECK(s.create_index(kFieldOff, kFieldLen));
        NOVA_CHECK(commit_group(s, 2, size, 1));
        NovaKvInspector::settle(s);
        Stepper stepper;
        stepper.arm({point});
        View old_view, new_view;
        std::thread reader([&]() {
            stepper.bind_this_thread();
            old_view = probe(s, nullptr, 2, size, 1);        // parks inside this call
            stepper.disarm();
            new_view = probe(s, nullptr, 2, size, 600);
        });
        NOVA_CHECK(stepper.wait_until_parked() == point);
        for (std::uint64_t gen = 2; gen <= 600; ++gen) {
            NOVA_CHECK(commit_group(s, 2, size, gen));
            if (gen % 25 == 0) NovaKvInspector::settle(s);
        }
        NovaKvInspector::settle(s);
        stepper.resume();
        reader.join();
        if (point == TestPoint::ReaderAfterRootLoad) {
            NOVA_CHECK(old_view.exact);                      // generation 1, complete: old index + old rows
        } else {
            NOVA_CHECK(old_view.matches == 0);               // newest version: generation 1 is gone from its index
        }
        NOVA_CHECK(new_view.exact);
        NOVA_CHECK(NovaKvInspector::settle(s));
        check_valid(s);
        std::printf("  index_get parked %-24s 599 indexed transactions meanwhile; it returned %d of 16 old entries\n", label,
                    old_view.matches);
    }
    NovaKvInspector::set_default_rcu_options(0, 0);
}

// ---- 4. Stress: index lookups and snapshots against many indexed commits -------------------------------
void stress_index_under_commits(int size) {
    KVStore s;                                      // background cleaner
    NOVA_CHECK(s.create_index(kFieldOff, kFieldLen));
    const std::uint64_t groups = 5;
    const std::uint64_t generations = size <= 4 ? 3000 : (size <= 16 ? 1200 : 400);
    for (std::uint64_t grp = 0; grp < groups; ++grp) NOVA_CHECK(commit_group(s, grp, size, 1));

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> newest{1};
    std::atomic<std::uint64_t> inconsistent{0}, partial{0}, backwards{0}, snapshots{0}, lookups{0};
    std::vector<std::thread> threads;
    // Snapshot readers: rows and index of one version must agree completely.
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t]() {
            std::uint64_t n = 0;
            while (!done.load() || n < 100) {
                const std::uint64_t grp = (n + static_cast<std::uint64_t>(t)) % groups;
                const std::uint64_t floor = newest.load();
                KVStore::Snapshot snap = s.acquire_snapshot();
                std::uint64_t g = 0;
                if (!snapshot_consistent(s, snap, grp, size, &g)) inconsistent.fetch_add(1);
                if (g < floor) backwards.fetch_add(1);
                ++n;
            }
            snapshots.fetch_add(n);
        });
    }
    // Plain index_get: one call is one version, so for any generation the answer
    // is the whole group or nothing, never some of its keys.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t]() {
            std::uint64_t n = 0;
            while (!done.load() || n < 1000) {
                const std::uint64_t grp = (n + static_cast<std::uint64_t>(t)) % groups;
                const std::uint64_t g = newest.load() + (n % 3);      // just committed, being committed, or not yet
                const View v = probe(s, nullptr, grp, size, g);
                if (v.matches != 0 && !v.exact) partial.fetch_add(1);
                ++n;
            }
            lookups.fetch_add(n);
        });
    }
    bool ok = true;
    for (std::uint64_t gen = 2; gen <= generations; ++gen) {
        for (std::uint64_t grp = 0; grp < groups; ++grp) ok = ok && commit_group(s, grp, size, gen);
        // Single-key index maintenance in between: a row that leaves the index
        // (too short), comes back, and is deleted.
        const long extra = 900000 + static_cast<long>(gen % 7);
        const Row r = make_row(99, gen, extra);
        ok = ok && s.put(extra, &r, sizeof(r));
        if (gen % 3 == 0) ok = ok && s.put(extra, &r, 4);
        if (gen % 5 == 0) s.erase(extra);
        newest.store(gen);
    }
    done.store(true);
    for (auto& th : threads) th.join();
    NOVA_CHECK(ok);
    NOVA_CHECK(inconsistent.load() == 0);
    NOVA_CHECK(partial.load() == 0);
    NOVA_CHECK(backwards.load() == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.reader_slots_owned == 0);
    NOVA_CHECK(st.index_segments_released + 64 >= st.index_segments_built);   // old index segments were reclaimed
    check_valid(s);
    std::printf("  %3d-key indexed transactions: %llu publications; %llu snapshots (0 with rows and index disagreeing), "
                "%llu plain lookups (0 partial); %llu index segments built, %llu reclaimed\n",
                size, static_cast<unsigned long long>(st.versions_published),
                static_cast<unsigned long long>(snapshots.load()), static_cast<unsigned long long>(lookups.load()),
                static_cast<unsigned long long>(st.index_segments_built),
                static_cast<unsigned long long>(st.index_segments_released));
}

// ---- 5. create_index while readers are reading and the writer is writing ---------------------------------
void stress_create_index_under_load() {
    KVStore s;
    const std::uint64_t groups = 30;
    const int size = 6;
    for (std::uint64_t grp = 0; grp < groups; ++grp) NOVA_CHECK(commit_group(s, grp, size, 1));
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> partial{0}, torn{0}, seen_before{0}, seen_after{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t]() {
            std::uint64_t n = static_cast<std::uint64_t>(t);
            bool appeared = false;
            while (!done.load()) {
                // The static groups 10..29 are never rewritten: for them the
                // index either does not exist yet (0) or is complete.
                const std::uint64_t grp = 10 + n % 20;
                const View v = probe(s, nullptr, grp, size, 1);
                if (v.matches != 0 && !v.exact) partial.fetch_add(1);
                if (v.matches == 0 && appeared) partial.fetch_add(1);        // an index never disappears again
                if (v.exact) appeared = true;
                (v.exact ? seen_after : seen_before).fetch_add(1);
                Row r;
                if (!s.get(static_cast<long>(grp) * 1000, &r, sizeof(r)) || r.check != row_check(r)) torn.fetch_add(1);
                ++n;
            }
        });
    }
    std::thread writer([&]() {                      // concurrent writer activity on groups 0..9
        for (std::uint64_t gen = 2; !done.load(); ++gen) {
            for (std::uint64_t grp = 0; grp < 10; ++grp) commit_group(s, grp, size, gen);
        }
    });
    while (seen_before.load() < 2000) std::this_thread::yield();
    NOVA_CHECK(s.create_index(kFieldOff, kFieldLen));            // serialized with the writer by the writer mutex
    while (seen_after.load() < 2000) std::this_thread::yield();
    done.store(true);
    for (auto& r : readers) r.join();
    writer.join();
    NOVA_CHECK(partial.load() == 0 && torn.load() == 0);
    // After creation the writer's commits kept the index in step.
    KVStore::Snapshot snap = s.acquire_snapshot();
    std::uint64_t g = 0;
    for (std::uint64_t grp = 0; grp < groups; ++grp) NOVA_CHECK(snapshot_consistent(s, snap, grp, size, &g));
    { KVStore::Snapshot gone = std::move(snap); }
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
    std::printf("  create_index under load: %llu lookups before it existed (all empty), %llu after (all complete), 0 partial\n",
                static_cast<unsigned long long>(seen_before.load()), static_cast<unsigned long long>(seen_after.load()));
}

}  // namespace

int main() {
    Stepper::install();
    for (int size : {1, 4, 16, 100}) test_indexed_transaction_publication(size);
    test_index_creation_is_atomic();
    test_reader_parked_in_index_get(TestPoint::ReaderAfterValidate, "before loading the root:");
    test_reader_parked_in_index_get(TestPoint::ReaderAfterRootLoad, "after loading the root:");
    for (int size : {1, 4, 16, 100}) stress_index_under_commits(size);
    stress_create_index_under_load();

    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::IndexSegment) == 0);
    return nova_test::finish("index_concurrency_test");
}
