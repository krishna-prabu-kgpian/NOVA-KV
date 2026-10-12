// versioned_interleaving_test.cpp -- deterministic interleavings of readers and
// the writer around version publication (Part B, milestone B1).
//
// The engine is built with pause points (src/test_points.h). A test parks one
// chosen thread at an exact step, lets other threads run to completion, checks
// what they observe, then releases the parked thread. No sleeps are used: every
// hand-over is a mutex/condition-variable rendezvous, so each schedule is the
// same on every run.
//
// Schedules:
//   S1 reader parked right after loading the root, writer publishes thousands of
//      newer versions (overwrites, then erase): the reader still returns its
//      original value; a newly arriving reader sees the latest state.
//   S2 the same with the reader parked between finding the entry and copying
//      the bytes out.
//   S3 writer parked with a multi-key version fully built but not published:
//      readers see only the old state, for every key.
//   S4 writer parked after the root swap but before the old version is handed
//      to the retained list: readers see only the new state, the old version is
//      intact, and it is not lost when the writer resumes.
//   S5 (stress, bounded by counts) multi-key commits race with readers that
//      read several keys through one version: never a mix of two commits.
//   S6 writer parked with a new version built and its ownership taken, but not
//      published; another thread then releases EVERY older version (what a
//      cleaner will do in B2). No segment or chunk the new or the current
//      version uses may be destroyed.
#include "versioned_store.h"

#include "test_points.h"
#include "test_util.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "versioned_interleaving_test must be built with NOVA_KV_TEST_HOOKS"
#endif

using nova::BatchOp;
using nova::TestPoint;
using nova::Version;
using nova::VersionedStore;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 37 + i * 11 + 5);
    return v;
}

// Parks the one thread that opted in (thread-local flag) the first time it
// reaches the chosen point.
thread_local bool tl_park_this_thread = false;

class Gate {
public:
    explicit Gate(TestPoint point) : point_(point) {
        nova::test_hooks::set_point_hook(&Gate::hook, this);
    }
    ~Gate() { nova::test_hooks::set_point_hook(nullptr, nullptr); }

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
    bool was_parked() {
        std::lock_guard<std::mutex> lk(m_);
        return parked_;
    }

private:
    static void hook(TestPoint point, void* context) {
        Gate* g = static_cast<Gate*>(context);
        if (!tl_park_this_thread || point != g->point_) return;
        std::unique_lock<std::mutex> lk(g->m_);
        if (g->parked_) return;  // one-shot
        g->parked_ = true;
        g->cv_.notify_all();
        g->cv_.wait(lk, [g] { return g->released_; });
    }

    const TestPoint point_;
    std::mutex m_;
    std::condition_variable cv_;
    bool parked_ = false;
    bool released_ = false;
};

bool holds(const VersionedStore& s, const Version* v, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    if (!s.get_at(v, key, buf.data(), expect.size())) return false;
    return std::memcmp(buf.data(), expect.data(), expect.size()) == 0 && buf[expect.size()] == 0xCD;
}

std::vector<long> keys_in_distinct_partitions(std::size_t n) {
    std::vector<long> keys;
    bool used[nova::kPartitionCount] = {};
    for (long k = 1; keys.size() < n; ++k) {
        const std::uint32_t p = nova::partition_of(nova::hash_key(k));
        if (!used[p]) {
            used[p] = true;
            keys.push_back(k);
        }
    }
    return keys;
}

// ---- S1 / S2: a parked reader against thousands of publications ----------------
void reader_parked(TestPoint where, const char* name) {
    VersionedStore s;
    const long key = 4242;
    const Bytes original = pattern(3000, 1);
    NOVA_CHECK(s.put(key, original.data(), original.size()));
    for (long k = 0; k < 50; ++k) NOVA_CHECK(s.put(k, &k, sizeof(k)));
    const Version* v_at_park = s.current_version();

    Gate gate(where);
    Bytes reader_buf(3000, 0);
    bool reader_found = false;
    std::thread reader([&]() {
        tl_park_this_thread = true;
        reader_found = s.get(key, reader_buf.data(), reader_buf.size());
    });
    gate.wait_until_parked();  // the reader now holds the version it loaded

    // The writer replaces the value 3000 times (about 9 MB through ~140 chunks),
    // rewrites the neighbours, and finally erases the key.
    bool writes_ok = true;
    for (std::uint64_t i = 0; i < 3000; ++i) {
        const Bytes v = pattern(3000, 100 + i);
        writes_ok = writes_ok && s.put(key, v.data(), v.size());
        if (i % 500 == 0) {
            // A reader arriving NOW sees the latest published state.
            Bytes fresh(3000);
            writes_ok = writes_ok && s.get(key, fresh.data(), fresh.size()) && fresh == v;
        }
    }
    for (long k = 0; k < 50; ++k) writes_ok = writes_ok && s.erase(k);
    writes_ok = writes_ok && s.erase(key);
    NOVA_CHECK(writes_ok);
    NOVA_CHECK(s.current_version()->sequence == v_at_park->sequence + 3051);
    unsigned char probe[8];
    NOVA_CHECK(!s.get(key, probe, 0) && !s.get(key, nullptr, 0));  // gone for new readers

    gate.release();
    reader.join();
    NOVA_CHECK(gate.was_parked());
    // The parked reader completed against the version it had loaded: the key is
    // present there and its bytes are the original ones, untouched.
    NOVA_CHECK(reader_found);
    NOVA_CHECK(reader_buf == original);
    // That version is still there in full (B1 retains everything).
    NOVA_CHECK(holds(s, v_at_park, key, original));
    for (long k = 0; k < 50; ++k) {
        long out = -1;
        NOVA_CHECK(s.get_at(v_at_park, k, &out, sizeof(out)) && out == k);
    }
    NOVA_CHECK(s.stats().versions_retained == s.stats().versions_published);
    NOVA_CHECK(s.validate() == nullptr);
    std::printf("  %s: reader held version %llu across %llu newer publications\n", name,
                static_cast<unsigned long long>(v_at_park->sequence),
                static_cast<unsigned long long>(s.current_version()->sequence - v_at_park->sequence));
}

// ---- S3 / S4: readers around the instant of publication -------------------------
void writer_parked(TestPoint where, bool expect_new_state, const char* name) {
    VersionedStore s;
    const auto keys = keys_in_distinct_partitions(8);
    const long extra = 900001, doomed = 900002;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const Bytes v = pattern(64, 10 + i);
        NOVA_CHECK(s.put(keys[i], v.data(), v.size()));
    }
    const Bytes doomed_value = pattern(32, 77);
    NOVA_CHECK(s.put(doomed, doomed_value.data(), doomed_value.size()));
    const Version* v_old = s.current_version();
    const std::uint64_t published_before = s.stats().versions_published;
    const std::uint64_t retained_before = s.stats().versions_retained;

    // One logical write: overwrite 8 keys in 8 partitions, add a key, erase a key.
    std::vector<Bytes> new_values;
    for (std::size_t i = 0; i < keys.size(); ++i) new_values.push_back(pattern(64, 500 + i));
    const Bytes extra_value = pattern(40, 600);
    std::vector<BatchOp> ops;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        ops.push_back(BatchOp{BatchOp::Kind::Put, keys[i], new_values[i].data(), new_values[i].size()});
    }
    ops.push_back(BatchOp{BatchOp::Kind::Put, extra, extra_value.data(), extra_value.size()});
    ops.push_back(BatchOp{BatchOp::Kind::Erase, doomed, nullptr, 0});

    Gate gate(where);
    bool commit_ok = false;
    std::thread writer([&]() {
        tl_park_this_thread = true;
        commit_ok = s.commit(ops.data(), ops.size());
    });
    gate.wait_until_parked();

    // Several reader threads look at every key while the writer is parked.
    std::atomic<int> saw_old{0}, saw_new{0}, saw_mixed{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            for (int round = 0; round < 200; ++round) {
                int old_hits = 0, new_hits = 0;
                unsigned char buf[64];
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (!s.get(keys[i], buf, sizeof(buf))) continue;
                    if (std::memcmp(buf, pattern(64, 10 + i).data(), 64) == 0) ++old_hits;
                    if (std::memcmp(buf, new_values[i].data(), 64) == 0) ++new_hits;
                }
                unsigned char small[40];
                const bool has_extra = s.get(extra, small, sizeof(small));
                const bool has_doomed = s.get(doomed, small, sizeof(small));
                const int n = static_cast<int>(keys.size());
                if (old_hits == n && new_hits == 0 && !has_extra && has_doomed) saw_old.fetch_add(1);
                else if (new_hits == n && old_hits == 0 && has_extra && !has_doomed) saw_new.fetch_add(1);
                else saw_mixed.fetch_add(1);
            }
        });
    }
    for (auto& r : readers) r.join();

    NOVA_CHECK(saw_mixed.load() == 0);  // never a partial write
    if (expect_new_state) {
        // The root has been swapped: every reader sees the whole new state ...
        NOVA_CHECK(saw_new.load() == 800 && saw_old.load() == 0);
        NOVA_CHECK(s.current_version() != v_old);
        // ... and the replaced version, not yet on the retained list, is intact.
        for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(holds(s, v_old, keys[i], pattern(64, 10 + i)));
        NOVA_CHECK(holds(s, v_old, doomed, doomed_value));
    } else {
        // Fully built but unpublished: nothing of it is visible.
        NOVA_CHECK(saw_old.load() == 800 && saw_new.load() == 0);
        NOVA_CHECK(s.current_version() == v_old);
    }

    gate.release();
    writer.join();
    NOVA_CHECK(commit_ok && gate.was_parked());
    const Version* v_new = s.current_version();
    NOVA_CHECK(v_new != v_old && v_new->sequence == v_old->sequence + 1);
    NOVA_CHECK(s.stats().versions_published == published_before + 1);  // exactly one publication
    NOVA_CHECK(s.stats().versions_retained == retained_before + 1);    // old version handed over, not lost
    for (std::size_t i = 0; i < keys.size(); ++i) {
        NOVA_CHECK(holds(s, v_new, keys[i], new_values[i]));
        NOVA_CHECK(holds(s, v_old, keys[i], pattern(64, 10 + i)));
    }
    unsigned char small[40];
    NOVA_CHECK(holds(s, v_new, extra, extra_value) && !s.get_at(v_new, doomed, small, sizeof(small)));
    NOVA_CHECK(holds(s, v_old, doomed, doomed_value) && !s.get_at(v_old, extra, small, sizeof(small)));
    NOVA_CHECK(s.validate() == nullptr);
    std::printf("  %s: 800 multi-key observations, old=%d new=%d mixed=%d\n", name, saw_old.load(),
                saw_new.load(), saw_mixed.load());
}

// ---- S6: older versions released while a new version is being prepared ----------
// Pre-B2 audit demonstration. The release is done with the B1 quiescent hatch:
// legal here because the only other thread is the writer, parked at a pause
// point (the gate's mutex orders its writes before this thread's reads), and
// there are no readers. It stands in for a cleaner that has established, by
// whatever means, that the OLD versions are unreachable.
void release_old_versions_while_writer_prepares() {
    VersionedStore s;
    const auto keys = keys_in_distinct_partitions(40);
    const std::size_t big = 30000;  // two per 64 KiB chunk: values of different keys share chunks
    // Build history: every key written three times, so each partition's segment
    // and each chunk has been referenced by many versions.
    for (std::uint64_t round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const Bytes v = pattern(big, round * 1000 + i);
            NOVA_CHECK(s.put(keys[i], v.data(), v.size()));
        }
    }
    const Version* v_current = s.current_version();
    const std::uint64_t retained_before = s.stats().versions_retained;
    NOVA_CHECK(retained_before == 120);

    // The new version: overwrites 3 keys, erases 1. It SHARES the other 36
    // segments with the current version, and its 3 rebuilt segments keep
    // nothing old; the shared segments reference chunks written long ago.
    std::vector<Bytes> new_values;
    for (std::size_t i = 0; i < 3; ++i) new_values.push_back(pattern(big, 9000 + i));
    std::vector<BatchOp> ops;
    for (std::size_t i = 0; i < 3; ++i) {
        ops.push_back(BatchOp{BatchOp::Kind::Put, keys[i], new_values[i].data(), new_values[i].size()});
    }
    ops.push_back(BatchOp{BatchOp::Kind::Erase, keys[3], nullptr, 0});

    Gate gate(TestPoint::WriterBeforePublish);
    bool commit_ok = false;
    std::thread writer([&]() {
        tl_park_this_thread = true;
        commit_ok = s.commit(ops.data(), ops.size());
    });
    gate.wait_until_parked();  // new version built, pins and owner references taken

    // "Cleaner": every one of the 120 older versions goes away now.
    s.release_retained_quiescent();
    NOVA_CHECK(s.stats().versions_retained == 0);
    NOVA_CHECK(s.stats().versions_released == 120);
    NOVA_CHECK(s.stats().segments_released > 0 && s.stats().arena.chunks_freed > 0);  // it really freed things

    // The CURRENT version (still published; readers may be using it) is whole.
    NOVA_CHECK(s.current_version() == v_current);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        NOVA_CHECK(holds(s, v_current, keys[i], pattern(big, 2000 + i)));
    }
    // Every segment the current version points at is still owned: by it, and,
    // where the prepared version shares it, by that one too.
    std::uint32_t shared_twice = 0, owned_once = 0, zero = 0;
    for (const nova::Segment* seg : v_current->segments) {
        if (seg == nullptr) continue;
        const std::uint32_t owners = seg->owners.load();
        if (owners == 2) ++shared_twice;
        else if (owners == 1) ++owned_once;
        else ++zero;
    }
    NOVA_CHECK(zero == 0);
    NOVA_CHECK(shared_twice == 36);  // shared with the not-yet-published version
    NOVA_CHECK(owned_once == 4);     // the 4 partitions the new version replaced

    gate.release();
    writer.join();
    NOVA_CHECK(commit_ok && gate.was_parked());

    // The prepared version, published after the release, is whole as well.
    const Version* v_new = s.current_version();
    NOVA_CHECK(v_new != v_current);
    for (std::size_t i = 0; i < 3; ++i) NOVA_CHECK(holds(s, v_new, keys[i], new_values[i]));
    unsigned char probe[8];
    NOVA_CHECK(!s.get_at(v_new, keys[3], probe, 0) && !s.get_at(v_new, keys[3], nullptr, 0));
    for (std::size_t i = 4; i < keys.size(); ++i) {
        NOVA_CHECK(holds(s, v_new, keys[i], pattern(big, 2000 + i)));  // through shared segments
    }
    // And the replaced version is still intact on the retained list.
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(holds(s, v_current, keys[i], pattern(big, 2000 + i)));
    NOVA_CHECK(s.stats().versions_retained == 1);
    const char* err = s.validate();
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
    std::printf("  S6: 120 older versions released while a prepared version shared 36 segments; "
                "no shared segment or chunk lost\n");
}

// ---- S5: atomic visibility under free-running concurrency -------------------------
struct Record {
    std::uint64_t generation;
    std::uint64_t generation_inverted;
    long key;
    unsigned char fill[40];
};

Record make_record(std::uint64_t generation, long key) {
    Record r;
    r.generation = generation;
    r.generation_inverted = ~generation;
    r.key = key;
    std::memset(r.fill, static_cast<int>(static_cast<unsigned char>(generation * 13 + static_cast<std::uint64_t>(key))), sizeof(r.fill));
    return r;
}

bool record_ok(const Record& r, long key) {
    if (r.generation_inverted != ~r.generation || r.key != key) return false;
    const unsigned char want = static_cast<unsigned char>(r.generation * 13 + static_cast<std::uint64_t>(key));
    for (unsigned char b : r.fill) {
        if (b != want) return false;
    }
    return true;
}

void atomic_visibility_stress() {
    VersionedStore s;
    const auto keys = keys_in_distinct_partitions(6);
    constexpr std::uint64_t kGenerations = 15000;
    constexpr int kReaders = 3;
    constexpr std::uint64_t kMinRounds = 4000;

    auto commit_generation = [&](std::uint64_t g) {
        Record recs[6];
        BatchOp ops[6];
        for (std::size_t i = 0; i < 6; ++i) {
            recs[i] = make_record(g, keys[i]);
            ops[i] = BatchOp{BatchOp::Kind::Put, keys[i], &recs[i], sizeof(Record)};
        }
        return s.commit(ops, 6);
    };
    NOVA_CHECK(commit_generation(1));

    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, writer_done{false};
    std::atomic<std::uint64_t> rounds[kReaders];
    std::atomic<std::uint64_t> mixed{0}, torn{0}, went_backwards{0}, missing{0};
    for (auto& r : rounds) r.store(0);

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t]() {
            std::uint64_t last_generation = 0, n = 0;
            std::uint64_t l_mixed = 0, l_torn = 0, l_back = 0, l_missing = 0;
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            while (!writer_done.load() || n < kMinRounds) {
                // (a) Several keys through ONE version: all from the same commit.
                const Version* v = s.current_version();
                std::uint64_t g = 0;
                for (std::size_t i = 0; i < 6; ++i) {
                    Record r;
                    if (!s.get_at(v, keys[i], &r, sizeof(r))) {
                        ++l_missing;
                        continue;
                    }
                    if (!record_ok(r, keys[i])) ++l_torn;
                    if (i == 0) g = r.generation;
                    else if (r.generation != g) ++l_mixed;
                }
                if (g < last_generation) ++l_back;  // versions only move forward
                last_generation = g;
                // (b) The public read path: one complete record, never torn.
                Record r;
                const long key = keys[n % 6];
                if (!s.get(key, &r, sizeof(r))) ++l_missing;
                else if (!record_ok(r, key)) ++l_torn;
                ++n;
                if ((n & 0x3F) == 0) rounds[t].store(n);
            }
            rounds[t].store(n);
            mixed.fetch_add(l_mixed);
            torn.fetch_add(l_torn);
            went_backwards.fetch_add(l_back);
            missing.fetch_add(l_missing);
        });
    }

    bool commits_ok = true;
    std::uint64_t last = 1;
    std::thread writer([&]() {
        while (ready.load() != kReaders) std::this_thread::yield();
        go.store(true);
        auto overlapped = [&]() {
            for (auto& r : rounds) {
                if (r.load() < kMinRounds / 2) return false;
            }
            return true;
        };
        for (std::uint64_t g = 2; g <= kGenerations || !overlapped(); ++g) {
            commits_ok = commits_ok && commit_generation(g);
            last = g;
        }
        writer_done.store(true);
    });
    writer.join();
    for (auto& r : readers) r.join();

    NOVA_CHECK(commits_ok);
    NOVA_CHECK(mixed.load() == 0);
    NOVA_CHECK(torn.load() == 0);
    NOVA_CHECK(went_backwards.load() == 0);
    NOVA_CHECK(missing.load() == 0);
    for (auto& r : rounds) NOVA_CHECK(r.load() >= kMinRounds);
    for (std::size_t i = 0; i < 6; ++i) {
        Record r;
        NOVA_CHECK(s.get(keys[i], &r, sizeof(r)) && r.generation == last);
    }
    NOVA_CHECK(s.stats().versions_published == last);  // one publication per 6-key commit
    NOVA_CHECK(s.stats().segments_built == 6 * last);  // six clones per commit, no more
    NOVA_CHECK(s.validate() == nullptr);
    std::printf("  stress: %llu six-key commits, reader rounds %llu/%llu/%llu, mixed=0 torn=0\n",
                static_cast<unsigned long long>(last),
                static_cast<unsigned long long>(rounds[0].load()),
                static_cast<unsigned long long>(rounds[1].load()),
                static_cast<unsigned long long>(rounds[2].load()));
}

}  // namespace

int main() {
    reader_parked(TestPoint::ReaderAfterRootLoad, "S1 reader parked after root load");
    reader_parked(TestPoint::ReaderBeforeCopyOut, "S2 reader parked before copy-out");
    writer_parked(TestPoint::WriterBeforePublish, false, "S3 writer parked before publish");
    writer_parked(TestPoint::WriterAfterPublish, true, "S4 writer parked after publish");
    atomic_visibility_stress();
    release_old_versions_while_writer_prepares();

    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    return nova_test::finish("versioned_interleaving_test");
}
