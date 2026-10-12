// reclamation_test.cpp -- deterministic schedules for epoch-based reclamation
// (Part B, milestone B2).
//
// The store is built with a Manual cleaner: no reclamation pass runs unless a
// test thread runs one. Threads are parked at named pause points
// (src/test_points.h) with a mutex/condition-variable rendezvous; no sleeps.
// Every schedule is therefore the same on every run, and every "the cleaner
// tries now" in the comments below is a real call to reclaim_pass().
//
// What must hold in all of them: a version is destroyed only after two gated
// epoch advances beyond its retirement tag; a pinned participant is never
// overtaken by more than one advance; nothing a pinned participant can reach
// is destroyed or has its chunk ID reused; and once it leaves, reclamation
// proceeds. ASan turns any violation of the first three into a report.
#include "versioned_store.h"

#include "alloc.h"
#include "test_points.h"
#include "test_util.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "reclamation_test must be built with NOVA_KV_TEST_HOOKS"
#endif

using nova::BatchOp;
using nova::TestPoint;
using nova::VersionedStore;

namespace {

using Bytes = std::vector<unsigned char>;
const nova::StoreOptions kManual{nova::ReclaimMode::Epoch, nova::CleanerMode::Manual};

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 41 + i * 13 + 7);
    return v;
}

// ---- Pause-point scheduler: each gate parks the one thread bound to it ---------
class Gate {
public:
    explicit Gate(TestPoint point) : point_(point) {}
    void bind_this_thread() { tl_gate = this; }
    void wait_until_parked() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return parked_; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }
    static void install() { nova::test_hooks::set_point_hook(&Gate::hook, nullptr); }

private:
    static void hook(TestPoint point, void*) {
        Gate* g = tl_gate;
        if (g == nullptr || point != g->point_) return;
        std::unique_lock<std::mutex> lk(g->m_);
        if (g->parked_) return;  // one-shot
        g->parked_ = true;
        g->cv_.notify_all();
        g->cv_.wait(lk, [g] { return g->released_; });
    }
    static thread_local Gate* tl_gate;
    const TestPoint point_;
    std::mutex m_;
    std::condition_variable cv_;
    bool parked_ = false;
    bool released_ = false;
};
thread_local Gate* Gate::tl_gate = nullptr;

bool put(VersionedStore& s, long key, const Bytes& v) { return s.put(key, v.data(), v.size()); }

bool reads(const VersionedStore& s, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    if (!s.get(key, buf.data(), expect.size())) return false;
    return std::memcmp(buf.data(), expect.data(), expect.size()) == 0 && buf[expect.size()] == 0xCD;
}

void check_valid(VersionedStore& s) {
    const char* err = s.validate();
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// The cleaner "tries hard": many passes, none of which may wait.
void passes(VersionedStore& s, int n) {
    for (int i = 0; i < n; ++i) s.reclaim_pass();
}

// ---- 1. The grace period itself: two gated advances, never age ---------------------
void test_two_advances_required() {
    VersionedStore s(kManual);
    NOVA_CHECK(put(s, 1, pattern(100, 1)));  // retires the empty version, tagged with epoch 0
    NOVA_CHECK(s.backlog() == 1 && s.epoch() == 0);
    NOVA_CHECK(s.reclaim_pass());            // first advance: 0 -> 1. Not yet eligible.
    NOVA_CHECK(s.epoch() == 1 && s.backlog() == 1 && s.stats().versions_reclaimed == 0);
    NOVA_CHECK(s.reclaim_pass());            // second advance: 1 -> 2. Eligible, destroyed.
    NOVA_CHECK(s.epoch() == 2 && s.backlog() == 0 && s.stats().versions_reclaimed == 1);
    NOVA_CHECK(!s.reclaim_pass());           // nothing retired: no advance, no work
    NOVA_CHECK(s.epoch() == 2);

    // Age does not count. With a participant pinned, a thousand commits and ten
    // thousand passes later, nothing retired since it arrived has been destroyed.
    VersionedStore::Pin pin = s.acquire_pin();
    const std::uint64_t pinned_epoch = s.epoch();
    for (std::uint64_t i = 0; i < 1000; ++i) NOVA_CHECK(put(s, 1, pattern(100, 10 + i)));
    passes(s, 10000);
    NOVA_CHECK(s.epoch() == pinned_epoch + 1);  // overtaken by exactly one advance, never two
    NOVA_CHECK(s.stats().versions_reclaimed == 1 && s.backlog() == 1000);
    NOVA_CHECK(s.stats().epoch.advance_blocked >= 9990);
    Bytes old(100);
    NOVA_CHECK(s.get_pinned(pin, 1, old.data(), old.size()) && old == pattern(100, 1));
    NOVA_CHECK(reads(s, 1, pattern(100, 1009)));  // an ordinary reader sees the latest
    s.release_pin(pin);
    NOVA_CHECK(!pin);
    NOVA_CHECK(s.drain());                        // now everything goes
    NOVA_CHECK(s.stats().versions_reclaimed == 1001);
    check_valid(s);
}

// ---- 2. Reader parked at each step of its entry --------------------------------------
// `where` is the pause point; `sees_old` says whether the reader, once resumed,
// must return the value that was current when it STARTED (it had already loaded
// the root) or the latest one (it had not).
void reader_parked(TestPoint where, bool holds_old_root, const char* name) {
    VersionedStore s(kManual);
    const long key = 77;
    const std::size_t big = 40000;  // one value per chunk: chunk identity is visible
    const Bytes original = pattern(big, 1);
    NOVA_CHECK(put(s, key, original));
    NOVA_CHECK(s.drain());
    const std::uint64_t e0 = s.epoch();
    const std::uint64_t reclaimed0 = s.stats().versions_reclaimed;
    const std::uint64_t freed0 = s.stats().arena.chunks_freed;

    Gate gate(where);
    Bytes got(big, 0);
    bool found = false;
    std::thread reader([&]() {
        gate.bind_this_thread();
        found = s.get(key, got.data(), got.size());
    });
    gate.wait_until_parked();

    // The writer replaces the value 2000 times, the cleaner tries after each.
    Bytes latest;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        latest = pattern(big, 100 + i);
        NOVA_CHECK(put(s, key, latest));
        passes(s, 3);
    }
    const nova::VersionedStats mid = s.stats();
    if (where == TestPoint::ReaderBeforeAnnounce) {
        // The reader has announced nothing yet: it holds nothing back, and it
        // has dereferenced nothing. Reclamation proceeds past it.
        NOVA_CHECK(mid.versions_reclaimed > reclaimed0 + 1900);
        NOVA_CHECK(mid.arena.chunks_freed > freed0 + 1900);
    } else {
        // It has announced e0: the epoch moves at most one step beyond it.
        NOVA_CHECK(s.epoch() == e0 + 1);
        NOVA_CHECK(mid.epoch.advance_blocked > 5000);
        // Destroyable only with a tag <= e0 - 1: nothing retired since qualifies.
        NOVA_CHECK(mid.versions_reclaimed == reclaimed0);
        NOVA_CHECK(mid.arena.chunks_freed == freed0);   // no chunk freed ...
        NOVA_CHECK(mid.arena.ids_reused == 0);          // ... hence no chunk ID reused
        NOVA_CHECK(s.backlog() == 2000);
    }
    NOVA_CHECK(reads(s, key, latest));  // a reader arriving now sees the latest value

    gate.release();
    reader.join();
    NOVA_CHECK(found);
    if (holds_old_root) {
        NOVA_CHECK(got == original);    // 2000 publications later, byte for byte
    } else {
        NOVA_CHECK(got == latest);      // it (re)announced and then loaded the root
    }
    if (where == TestPoint::ReaderBeforeAnnounce || where == TestPoint::ReaderAfterAnnounce) {
        // Its first announcement was stale by the time it validated: it retried.
        NOVA_CHECK(s.stats().epoch.validation_retries == 1);
    }
    // The reader has left: reclamation completes and chunk IDs come back.
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.stats().versions_reclaimed == reclaimed0 + 2000);
    for (std::uint64_t i = 0; i < 20; ++i) NOVA_CHECK(put(s, key, pattern(big, 9000 + i)));
    NOVA_CHECK(s.stats().arena.ids_reused > 0);
    NOVA_CHECK(reads(s, key, pattern(big, 9019)));
    NOVA_CHECK(s.drain());
    check_valid(s);
    std::printf("  reader parked %-28s reclaimed while parked: %llu of 2000, epoch moved by %llu\n",
                name, static_cast<unsigned long long>(mid.versions_reclaimed - reclaimed0),
                static_cast<unsigned long long>(mid.epoch.epoch - e0));
}

// ---- 3. Writer parked between publication and retirement ----------------------------
void writer_parked_around_retirement(TestPoint where, const char* name) {
    VersionedStore s(kManual);
    const Bytes v1 = pattern(500, 1), v2 = pattern(500, 2), other = pattern(64, 3);
    NOVA_CHECK(put(s, 5, v1));
    NOVA_CHECK(s.drain());
    // A reader that holds the version about to be replaced.
    Gate reader_gate(TestPoint::ReaderAfterRootLoad);
    Bytes got(500, 0);
    bool found = false;
    std::thread reader([&]() {
        reader_gate.bind_this_thread();
        found = s.get(5, got.data(), got.size());
    });
    reader_gate.wait_until_parked();

    Gate writer_gate(where);
    bool put_ok = false;
    std::thread writer([&]() {
        writer_gate.bind_this_thread();
        put_ok = s.put(5, v2.data(), v2.size());
    });
    writer_gate.wait_until_parked();
    // The root is swapped; the old version is owned by the parked writer and is
    // not in the queue. The cleaner can see nothing to do, and must not invent it.
    const std::uint64_t retired_before = s.stats().versions_retired;
    passes(s, 1000);
    NOVA_CHECK(s.stats().versions_retired == retired_before);
    NOVA_CHECK(s.stats().versions_reclaimed == retired_before);
    NOVA_CHECK(reads(s, 5, v2));  // new readers already see the new version

    writer_gate.release();
    writer.join();
    NOVA_CHECK(put_ok);
    NOVA_CHECK(s.stats().versions_retired == retired_before + 1);  // handed over, not lost
    // Still not destroyable: the first reader is pinned in an epoch <= the tag.
    passes(s, 1000);
    NOVA_CHECK(s.backlog() == 1);
    reader_gate.release();
    reader.join();
    NOVA_CHECK(found && got == v1);  // the replaced version was intact throughout
    NOVA_CHECK(s.drain());
    NOVA_CHECK(put(s, 6, other) && reads(s, 6, other));
    NOVA_CHECK(s.drain());
    check_valid(s);
    std::printf("  writer parked %-28s old version neither lost nor freed early\n", name);
}

// ---- 3b. Why the retirement tag is read AFTER the root swap -------------------------
// The writer is parked with its new version built but not published. The epoch
// advances once, and only then does a reader arrive: it validates the NEW epoch
// and still loads the OLD root, because the swap has not happened. The old
// version's tag must therefore be at least that reader's epoch, which is only
// guaranteed if the tag is read after the swap.
void reader_arrives_between_epoch_advance_and_publication() {
    VersionedStore s(kManual);
    const Bytes v1 = pattern(20000, 1), v2 = pattern(20000, 2);
    NOVA_CHECK(put(s, 5, v1));
    NOVA_CHECK(s.drain());
    NOVA_CHECK(put(s, 99, pattern(10, 3)));  // one retired version pending, so the cleaner will advance
    const std::uint64_t e = s.epoch();

    Gate writer_gate(TestPoint::WriterBeforePublish);
    bool put_ok = false;
    std::thread writer([&]() {
        writer_gate.bind_this_thread();
        put_ok = s.put(5, v2.data(), v2.size());
    });
    writer_gate.wait_until_parked();
    NOVA_CHECK(s.reclaim_pass() && s.epoch() == e + 1);  // the epoch moves while the writer waits

    Gate reader_gate(TestPoint::ReaderAfterRootLoad);
    Bytes got(20000, 0);
    bool found = false;
    std::thread reader([&]() {
        reader_gate.bind_this_thread();
        found = s.get(5, got.data(), got.size());
    });
    reader_gate.wait_until_parked();  // validated e + 1, holds the root that is about to be replaced

    writer_gate.release();
    writer.join();
    NOVA_CHECK(put_ok);
    // The cleaner may advance once more (the reader is in the current epoch),
    // and then no further. The replaced version must still be there.
    passes(s, 2000);
    NOVA_CHECK(s.epoch() == e + 2);
    NOVA_CHECK(s.backlog() >= 1);
    NOVA_CHECK(s.stats().arena.chunks_freed == 0);
    reader_gate.release();
    reader.join();
    NOVA_CHECK(found && got == v1);
    NOVA_CHECK(s.drain());
    check_valid(s);
}

// ---- 4. The collector's scan against a reader that registers during it --------------
void scan_races_with_registration(TestPoint cleaner_point, const char* name) {
    VersionedStore s(kManual);
    const Bytes v1 = pattern(300, 1), v2 = pattern(300, 2);
    NOVA_CHECK(put(s, 9, v1));
    NOVA_CHECK(s.drain());
    // Fill the existing slot blocks so that the reader below gets a slot in a
    // block that does not exist yet when the scan starts.
    std::vector<VersionedStore::Pin> fillers;
    const std::uint64_t slots = s.stats().epoch.slots_total;
    for (std::uint64_t i = 0; i < slots; ++i) {
        fillers.push_back(s.acquire_pin());
    }
    for (auto& p : fillers) {  // keep the slots owned but not announcing
        p.slot->state.store(nova::kEpochInactive);
    }
    NOVA_CHECK(put(s, 9, v1));  // something retired, so the cleaner wants to advance
    const std::uint64_t e = s.epoch();

    Gate cleaner_gate(cleaner_point);
    bool advanced = false;
    std::thread cleaner([&]() {
        cleaner_gate.bind_this_thread();
        advanced = s.reclaim_pass();
    });
    cleaner_gate.wait_until_parked();  // mid-scan, or scan complete and about to advance

    // A brand-new reader registers (new slot block), announces, validates and
    // loads the root, all while the collector is in the middle of its attempt.
    Gate reader_gate(TestPoint::ReaderAfterRootLoad);
    Bytes got(300, 0);
    bool found = false;
    std::thread reader([&]() {
        reader_gate.bind_this_thread();
        found = s.get(9, got.data(), got.size());
    });
    reader_gate.wait_until_parked();
    NOVA_CHECK(s.stats().epoch.slots_total > slots);  // it really is in a new block

    cleaner_gate.release();
    cleaner.join();
    // Whether or not this scan saw the reader, the epoch may move by ONE.
    NOVA_CHECK(s.epoch() <= e + 1);
    (void)advanced;
    // The version the reader holds is now replaced and retired ...
    NOVA_CHECK(put(s, 9, v2));
    passes(s, 2000);
    // ... and no later scan can miss it: its slot was linked before it announced.
    NOVA_CHECK(s.epoch() <= e + 1);
    NOVA_CHECK(s.backlog() >= 1);
    NOVA_CHECK(s.stats().epoch.advance_blocked > 1000);

    reader_gate.release();
    reader.join();
    NOVA_CHECK(found && got == v1);
    for (auto& p : fillers) s.release_pin(p);
    NOVA_CHECK(s.drain());
    check_valid(s);
    std::printf("  cleaner parked %-27s reader in a new slot block was not overtaken\n", name);
}

// ---- 5. Audit finding F2: a commit seals its writable chunk while the cleaner runs --
// The commit appends a small value to the writable chunk W, then a value that
// does not fit, so W is sealed; it now holds an unpublished reference into W.
// The only segment listing W belongs to a retired version, which the cleaner
// reclaims at exactly this moment. `fail_after` makes the commit's next
// allocation fail instead of succeeding.
void commit_seals_chunk_while_cleaner_runs(bool fail_after) {
    VersionedStore s(kManual);
    const Bytes small(100, 1), small2(100, 2), big(65400, 3);
    NOVA_CHECK(s.put(1, small.data(), small.size()));  // key 1's value lives in W
    NOVA_CHECK(s.erase(1));                            // current version no longer references W
    NOVA_CHECK(s.backlog() == 2);
    const std::uint64_t chunks_before = s.stats().arena.chunks_live;

    Gate gate(TestPoint::WriterAfterAppends);
    const BatchOp ops[2] = {{BatchOp::Kind::Put, 2, small2.data(), small2.size()},
                            {BatchOp::Kind::Put, 3, big.data(), big.size()}};
    bool ok = false;
    std::thread writer([&]() {
        gate.bind_this_thread();
        ok = s.commit(ops, 2);
    });
    gate.wait_until_parked();
    NOVA_CHECK(s.stats().arena.chunks_live == chunks_before + 1);  // W sealed, a new chunk exists

    // The cleaner reclaims both retired versions: the last segment reference on
    // W is dropped NOW, while the commit still needs W.
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.stats().versions_reclaimed == 2 && s.stats().segments_released == 1);
    NOVA_CHECK(s.stats().arena.chunks_freed == 0);  // W survives on the commit's reference

    if (fail_after) nova::test_hooks::fail_nth_allocation(0);  // the writer is parked: no race
    gate.release();
    writer.join();
    const bool fired = nova::test_hooks::allocation_failure_fired();
    nova::test_hooks::fail_nth_allocation(-1);

    unsigned char probe[8];
    if (fail_after) {
        NOVA_CHECK(fired && !ok);
        // Rolled back: nothing published, W is the writable chunk again and
        // alive, the chunk the commit created is gone.
        NOVA_CHECK(!s.get(2, probe, 0) && !s.get(3, probe, 0));
        NOVA_CHECK(s.stats().versions_published == 2);
        NOVA_CHECK(s.stats().arena.chunks_live == chunks_before);
        NOVA_CHECK(s.put(2, small2.data(), small2.size()));  // and it is usable
    } else {
        NOVA_CHECK(ok);
        NOVA_CHECK(s.stats().versions_published == 3);
    }
    NOVA_CHECK(reads(s, 2, small2));  // bytes in W, intact
    if (!fail_after) NOVA_CHECK(reads(s, 3, big));
    NOVA_CHECK(s.drain());
    check_valid(s);
}

// ---- 6. Audit: end_commit and the cleaner drop the last references to one chunk ----
// The writer is parked inside end_commit, about to drop its commit reference on
// the sealed chunk W; its new segment did not keep any value in W. The cleaner
// drops W's last SEGMENT reference at that moment. Exactly one of the two drops
// reaches zero; W is destroyed once.
void end_commit_races_with_release() {
    VersionedStore s(kManual);
    const Bytes small(100, 1), big(65400, 3), big2(65400, 4);
    NOVA_CHECK(s.put(1, small.data(), small.size()));  // W: value of key 1
    NOVA_CHECK(s.erase(1));
    Gate gate(TestPoint::WriterEndCommitBeforeRelease);
    // One commit that writes key 2 twice: first a small value into W, then a
    // big one that seals W and supersedes the small one. Its final segment
    // references only the new chunk; W is referenced by the commit alone.
    const BatchOp ops[2] = {{BatchOp::Kind::Put, 2, small.data(), small.size()},
                            {BatchOp::Kind::Put, 2, big.data(), big.size()}};
    bool ok = false;
    std::thread writer([&]() {
        gate.bind_this_thread();
        ok = s.commit(ops, 2);
    });
    gate.wait_until_parked();
    const std::uint64_t freed_before = s.stats().arena.chunks_freed;
    NOVA_CHECK(s.drain());  // cleaner: drops the retired segment's reference on W
    NOVA_CHECK(s.stats().arena.chunks_freed == freed_before);  // the commit's reference remains
    gate.release();
    writer.join();
    NOVA_CHECK(ok);
    NOVA_CHECK(s.stats().arena.chunks_freed == freed_before + 1);  // destroyed exactly once
    NOVA_CHECK(reads(s, 2, big));
    NOVA_CHECK(s.put(4, big2.data(), big2.size()) && reads(s, 4, big2));
    NOVA_CHECK(s.stats().arena.ids_reused == 1);  // W's ID came back through the pool
    NOVA_CHECK(s.drain());
    check_valid(s);
}

// ---- 7. Batch limit and work budget ---------------------------------------------------
void test_batch_limit(int batch) {
    VersionedStore s(kManual);
    s.set_reclaim_batch(batch);
    const int effective = batch <= 0 ? 1 : batch;
    NOVA_CHECK(s.reclaim_batch() == effective);
    const std::uint64_t n = 3000;
    for (std::uint64_t i = 0; i < n; ++i) NOVA_CHECK(put(s, static_cast<long>(i % 50), pattern(64, i)));
    NOVA_CHECK(s.backlog() == n);
    std::uint64_t pass_count = 0, max_in_pass = 0;
    while (s.backlog() != 0) {
        const std::uint64_t before = s.stats().versions_reclaimed;
        NOVA_CHECK(s.reclaim_pass());
        const std::uint64_t freed = s.stats().versions_reclaimed - before;
        if (freed > max_in_pass) max_in_pass = freed;
        ++pass_count;
        if (pass_count > 10 * n) break;
    }
    NOVA_CHECK(s.backlog() == 0);
    NOVA_CHECK(max_in_pass <= static_cast<std::uint64_t>(effective));         // the assignment's bound
    NOVA_CHECK(s.stats().reclaim_max_versions_in_pass == max_in_pass);
    // Two passes are spent on the grace period, then ceil(n / batch) on destruction.
    const std::uint64_t needed = (n + static_cast<std::uint64_t>(effective) - 1) / static_cast<std::uint64_t>(effective);
    NOVA_CHECK(pass_count >= needed && pass_count <= needed + 2);
    NOVA_CHECK(s.stats().versions_reclaimed == n);
    check_valid(s);
    std::printf("  batch %5d: %llu versions in %llu passes, at most %llu per pass\n", batch,
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(pass_count),
                static_cast<unsigned long long>(max_in_pass));
}

// A version whose release is far more work than one pass may do: it is taken
// apart over several passes and counts as reclaimed only at the end.
void test_cleanup_interrupted_and_resumed() {
    VersionedStore s(kManual);
    s.set_reclaim_batch(1);
    s.set_reclaim_work_per_version(16);  // one pass: 16 work units
    // 200 keys in 200 partitions, written by ONE commit, then rewritten by one
    // more. The first commit's 200 segments are owned by a single version.
    std::vector<long> keys;
    bool used[nova::kPartitionCount] = {};
    for (long k = 1; keys.size() < 200; ++k) {
        const std::uint32_t p = nova::partition_of(nova::hash_key(k));
        if (!used[p]) {
            used[p] = true;
            keys.push_back(k);
        }
    }
    std::vector<Bytes> a, b;
    std::vector<BatchOp> ops_a, ops_b;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        a.push_back(pattern(200, i));
        b.push_back(pattern(200, 1000 + i));
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ops_a.push_back(BatchOp{BatchOp::Kind::Put, keys[i], a[i].data(), a[i].size()});
        ops_b.push_back(BatchOp{BatchOp::Kind::Put, keys[i], b[i].data(), b[i].size()});
    }
    NOVA_CHECK(s.commit(ops_a.data(), ops_a.size()));
    NOVA_CHECK(s.commit(ops_b.data(), ops_b.size()));
    NOVA_CHECK(s.backlog() == 2);

    // Releasing the second retired version means 256 directory entries, 200
    // segment destructions and their chunk references: > 400 units, 16 per pass.
    std::uint64_t pass_count = 0, last_reclaimed = 0, passes_with_partial_work = 0;
    std::uint64_t last_segments = 0;
    while (s.backlog() != 0 && pass_count < 5000) {
        s.reclaim_pass();
        ++pass_count;
        const nova::VersionedStats st = s.stats();
        NOVA_CHECK(st.reclaim_max_units_in_pass <= 16);          // the budget is never exceeded
        NOVA_CHECK(st.versions_reclaimed - last_reclaimed <= 1); // nor the batch
        if (st.versions_reclaimed == last_reclaimed && st.segments_released > last_segments) {
            ++passes_with_partial_work;  // work was done, yet no version counted as reclaimed
        }
        // The first version to be released has 256 directory entries: at 16
        // units a pass it cannot be finished, and must not be counted, this early.
        if (pass_count == 10) NOVA_CHECK(st.versions_reclaimed == 0 && s.backlog() == 2);
        last_reclaimed = st.versions_reclaimed;
        last_segments = st.segments_released;
        // Readers are unaffected in the middle of a version's destruction.
        if (pass_count % 7 == 0) NOVA_CHECK(reads(s, keys[pass_count % keys.size()], b[pass_count % keys.size()]));
    }
    const nova::VersionedStats st = s.stats();
    NOVA_CHECK(s.backlog() == 0 && st.versions_reclaimed == 2);
    NOVA_CHECK(st.reclaim_interrupted_versions >= 10);
    NOVA_CHECK(passes_with_partial_work >= 10);
    NOVA_CHECK(pass_count >= 30);
    NOVA_CHECK(st.segments_released == 200);
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(reads(s, keys[i], b[i]));
    check_valid(s);
    std::printf("  resumable destruction: 2 versions took %llu passes of <= 16 units, %llu interruptions\n",
                static_cast<unsigned long long>(pass_count),
                static_cast<unsigned long long>(st.reclaim_interrupted_versions));
}

// ---- 7b. get() when a reader slot cannot be allocated ------------------------------
// The only resource a read can lack is a slot block on a thread's first read.
// get() must then throw std::bad_alloc: never report a present key as absent,
// never read unprotected. The store is unaffected and the read works afterwards.
void test_get_when_no_reader_slot_can_be_allocated() {
    VersionedStore s(kManual);
    NOVA_CHECK(put(s, 7, pattern(100, 7)));
    std::vector<VersionedStore::Pin> pins;
    for (int i = 0; i < 32; ++i) pins.push_back(s.acquire_pin());  // every existing slot is taken
    const std::uint64_t slots = s.stats().epoch.slots_total;
    std::thread t([&]() {                                          // a thread with no slot yet
        std::vector<unsigned char> out(100);
        nova::test_hooks::fail_slot_block_allocations(1);
        bool threw = false, returned = false, found = false;
        try {
            found = s.get(7, out.data(), out.size());
            returned = true;
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        NOVA_CHECK(threw && !returned && !found);
        NOVA_CHECK(s.stats().epoch.slots_total == slots);          // nothing half-registered
        NOVA_CHECK(s.get(7, out.data(), out.size()));              // memory is back
        NOVA_CHECK(out == pattern(100, 7));
    });
    t.join();
    // A pin cannot be had either, and likewise leaves nothing behind.
    while (s.stats().epoch.slots_owned < s.stats().epoch.slots_total) pins.push_back(s.acquire_pin());
    nova::test_hooks::fail_slot_block_allocations(1);              // the next pin needs a third block
    bool threw = false;
    try {
        pins.push_back(s.acquire_pin());
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    NOVA_CHECK(threw);
    NOVA_CHECK(s.stats().epoch.slots_owned == pins.size());
    for (VersionedStore::Pin& p : pins) s.release_pin(p);
    NOVA_CHECK(put(s, 7, pattern(100, 8)));
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.validate() == nullptr);
}

// ---- 8. A long-lived pin (the Snapshot foundation) across threads -------------------
void test_long_lived_pin_moved_between_threads() {
    VersionedStore s(kManual);
    const int nkeys = 60;
    for (long k = 0; k < nkeys; ++k) NOVA_CHECK(put(s, k, pattern(30000, static_cast<std::uint64_t>(k))));
    NOVA_CHECK(s.drain());
    VersionedStore::Pin pin = s.acquire_pin();  // a consistent view of all 60 keys
    const std::uint64_t pin_epoch = pin.slot->state.load();
    const std::uint64_t reclaimed0 = s.stats().versions_reclaimed;
    const std::uint64_t retired0 = s.stats().versions_retired;
    const std::uint64_t freed0 = s.stats().arena.chunks_freed;

    // 5000 commits: every key rewritten many times, some erased.
    for (std::uint64_t i = 0; i < 5000; ++i) {
        const long k = static_cast<long>(i % nkeys);
        if (i % 11 == 10) {
            s.erase(k);
        } else {
            NOVA_CHECK(put(s, k, pattern(30000, 10000 + i)));
        }
        if (i % 5 == 0) passes(s, 2);
    }
    passes(s, 5000);
    const std::uint64_t commits = s.stats().versions_retired - retired0;
    NOVA_CHECK(commits == 5000);
    NOVA_CHECK(s.stats().versions_reclaimed == reclaimed0);  // no premature reclamation
    NOVA_CHECK(s.stats().arena.chunks_freed == freed0);      // not one chunk of the old state lost
    NOVA_CHECK(s.backlog() == commits);
    NOVA_CHECK(s.epoch() == pin_epoch + 1);

    // Moving the handle is copying it; the slot's announcement is untouched.
    VersionedStore::Pin moved = pin;
    pin = VersionedStore::Pin{};
    NOVA_CHECK(!pin && moved);
    bool all_old = true;
    std::thread other([&]() {
        // Another thread reads the whole old state through the moved pin.
        NOVA_CHECK(moved.slot->state.load() == pin_epoch);
        Bytes buf(30000);
        for (long k = 0; k < nkeys; ++k) {
            all_old = all_old && s.get_pinned(moved, k, buf.data(), buf.size()) &&
                      buf == pattern(30000, static_cast<std::uint64_t>(k));
        }
        passes(s, 100);  // still pinned: still nothing
        s.release_pin(moved);
    });
    other.join();
    NOVA_CHECK(all_old);
    NOVA_CHECK(s.stats().versions_reclaimed == reclaimed0);  // nothing until the pin was released
    NOVA_CHECK(s.stats().epoch.slots_owned == 0);

    // The pin is gone: reclamation makes progress and finishes.
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.stats().versions_reclaimed == reclaimed0 + commits);
    NOVA_CHECK(s.stats().arena.chunks_freed > freed0 + 2000);  // the old chunks really went
    check_valid(s);
}

// ---- 9. Reader slots: per thread, reused after exit ---------------------------------
void test_reader_slots_reused_after_thread_exit() {
    VersionedStore s(kManual);
    NOVA_CHECK(put(s, 1, pattern(50, 1)));
    for (int round = 0; round < 300; ++round) {
        std::thread t([&]() {
            for (int i = 0; i < 20; ++i) NOVA_CHECK(reads(s, 1, pattern(50, 1)));
        });
        t.join();
        NOVA_CHECK(s.stats().epoch.slots_owned == 0);  // given back at thread exit
    }
    const nova::EpochStats st = s.stats().epoch;
    NOVA_CHECK(st.registrations == 300);   // one registration per thread, not per read
    NOVA_CHECK(st.slots_total == 32);      // 300 threads, one after another: one block
    // 100 threads alive at once: more slots than a block holds, all served.
    std::mutex m;
    std::condition_variable cv;
    int ready = 0;
    bool go = false;
    std::vector<std::thread> crowd;
    for (int t = 0; t < 100; ++t) {
        crowd.emplace_back([&]() {
            NOVA_CHECK(reads(s, 1, pattern(50, 1)));
            std::unique_lock<std::mutex> lk(m);
            ++ready;
            cv.notify_all();
            cv.wait(lk, [&] { return go; });
            lk.unlock();
            NOVA_CHECK(reads(s, 1, pattern(50, 1)));
        });
    }
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return ready == 100; });
    }
    NOVA_CHECK(s.stats().epoch.slots_owned == 100);
    NOVA_CHECK(s.stats().epoch.slots_total >= 100);
    {
        std::lock_guard<std::mutex> lk(m);
        go = true;
    }
    cv.notify_all();
    for (auto& t : crowd) t.join();
    NOVA_CHECK(s.stats().epoch.slots_owned == 0);
    NOVA_CHECK(s.drain());
}

// ---- 10. Comparator modes -------------------------------------------------------------
void test_comparator_modes() {
    {   // Disabled: retired versions are kept until the store is destroyed.
        VersionedStore s(nova::StoreOptions{nova::ReclaimMode::Disabled, nova::CleanerMode::Manual});
        for (std::uint64_t i = 0; i < 500; ++i) NOVA_CHECK(put(s, 1, pattern(100, i)));
        passes(s, 1000);
        NOVA_CHECK(!s.drain());
        NOVA_CHECK(s.backlog() == 500 && s.stats().versions_reclaimed == 0 && s.epoch() == 0);
        NOVA_CHECK(reads(s, 1, pattern(100, 499)));
        check_valid(s);
    }
    {   // Stop-the-world: frees the whole list at once, with readers excluded.
        VersionedStore s(nova::StoreOptions{nova::ReclaimMode::StopTheWorld, nova::CleanerMode::Manual});
        for (std::uint64_t i = 0; i < 500; ++i) NOVA_CHECK(put(s, 1, pattern(100, i)));
        NOVA_CHECK(!s.reclaim_pass());  // below the trigger: nothing
        NOVA_CHECK(s.backlog() == 500);
        for (std::uint64_t i = 500; i < 3000; ++i) NOVA_CHECK(put(s, 1, pattern(100, i)));
        NOVA_CHECK(s.reclaim_pass());   // one sweep, everything
        NOVA_CHECK(s.backlog() == 0 && s.stats().versions_reclaimed == 3000 && s.stats().stw_pauses == 1);
        NOVA_CHECK(s.stats().reclaim_max_versions_in_pass == 0);  // not an epoch pass: no batch bound applied
        NOVA_CHECK(reads(s, 1, pattern(100, 2999)));
        check_valid(s);
    }
}

}  // namespace

int main() {
    Gate::install();
    test_two_advances_required();
    reader_parked(TestPoint::ReaderBeforeAnnounce, false, "before announcing:");
    reader_parked(TestPoint::ReaderAfterAnnounce, false, "after announcing:");
    reader_parked(TestPoint::ReaderAfterValidate, false, "after validating:");
    reader_parked(TestPoint::ReaderAfterRootLoad, true, "after loading the root:");
    reader_parked(TestPoint::ReaderBeforeCopyOut, true, "before copying bytes:");
    writer_parked_around_retirement(TestPoint::WriterAfterPublish, "after the root swap:");
    writer_parked_around_retirement(TestPoint::WriterAfterRetireTag, "after tagging the old one:");
    reader_arrives_between_epoch_advance_and_publication();
    scan_races_with_registration(TestPoint::CleanerMidScan, "in the middle of its scan:");
    scan_races_with_registration(TestPoint::CleanerBeforeAdvance, "after its scan:");
    commit_seals_chunk_while_cleaner_runs(false);
    commit_seals_chunk_while_cleaner_runs(true);
    end_commit_races_with_release();
    for (int batch : {1, 8, 64, 256, 1024}) test_batch_limit(batch);
    test_batch_limit(0);
    test_batch_limit(-5);
    test_cleanup_interrupted_and_resumed();
    test_get_when_no_reader_slot_can_be_allocated();
    test_long_lived_pin_moved_between_threads();
    test_reader_slots_reused_after_thread_exit();
    test_comparator_modes();

    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    return nova_test::finish("reclamation_test");
}
