// reclamation_stress_test.cpp -- free-running stress of the complete Part B
// store: background cleaner, epoch-based reclamation, chunk-ID recycling.
//
// These runs are not deterministic; they exist to give the sanitizers real
// concurrency to look at (TSan: data races between readers, writer and
// cleaner; ASan: any object freed while a reader or a commit could reach it)
// and to check end-to-end properties: no torn or mixed read, memory that stops
// growing, and everything released at shutdown. Each is bounded by operation
// counts and start barriers, not by sleeps.
#include "versioned_store.h"

#include "test_points.h"
#include "test_util.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using nova::BatchOp;
using nova::VersionedStore;

namespace {

struct Record {
    std::uint64_t generation;
    std::uint64_t generation_inverted;
    long key;
    std::uint32_t length;  // total bytes of this record
    std::uint32_t pad;
};

unsigned char fill_byte(std::uint64_t generation, long key) {
    return static_cast<unsigned char>(generation * 29 + static_cast<std::uint64_t>(key) * 7 + 1);
}

// Variable-length self-describing record; occasionally larger than a chunk.
std::size_t make_record(std::vector<unsigned char>& out, std::uint64_t generation, long key) {
    const std::size_t extra = generation % 97 == 0 ? 70000 : (generation * 37) % 900;
    const std::size_t len = sizeof(Record) + extra;
    if (out.size() < len) out.resize(len);
    Record r{generation, ~generation, key, static_cast<std::uint32_t>(len), 0};
    std::memcpy(out.data(), &r, sizeof(r));
    std::memset(out.data() + sizeof(r), fill_byte(generation, key), extra);
    return len;
}

// 0 if `buf` (as filled by a successful get into a sentinel-filled buffer)
// holds exactly one complete record for `key`; otherwise 1.
int record_errors(const std::vector<unsigned char>& buf, long key, std::uint64_t* generation) {
    Record r;
    std::memcpy(&r, buf.data(), sizeof(r));
    if (r.generation_inverted != ~r.generation || r.key != key) return 1;
    if (r.length < sizeof(Record) || r.length > buf.size()) return 1;
    const unsigned char want = fill_byte(r.generation, key);
    for (std::size_t i = sizeof(Record); i < r.length; ++i) {
        if (buf[i] != want) return 1;
    }
    for (std::size_t i = r.length; i < r.length + 16 && i < buf.size(); ++i) {
        if (buf[i] != 0x5A) return 1;  // nothing copied beyond the record
    }
    *generation = r.generation;
    return 0;
}

// ---- 1. Readers, a writer, long-lived pins and thread churn, all at once ---------
void stress_readers_writer_cleaner(int batch) {
    VersionedStore s;  // defaults: epoch-based reclamation, background cleaner
    NOVA_CHECK(s.ok());
    s.set_reclaim_batch(batch);
    constexpr long kKeys = 64;
    constexpr int kReaders = 4;
    constexpr std::uint64_t kWrites = 40000;
    constexpr std::uint64_t kMinReads = 30000;

    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, writer_done{false};
    std::atomic<std::uint64_t> torn{0}, backwards{0}, pin_errors{0}, reads_total{0};
    std::atomic<std::uint64_t> reads_done[kReaders];
    for (auto& r : reads_done) r.store(0);
    // Generation of the last COMPLETED write per key: a read that starts after
    // it must never return an older generation.
    std::vector<std::atomic<std::uint64_t>> committed(static_cast<std::size_t>(kKeys));
    for (auto& c : committed) c.store(0);

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t]() {
            std::vector<unsigned char> buf(80000);
            std::uint64_t x = 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(t + 1);
            std::uint64_t n = 0, l_torn = 0, l_back = 0;
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            while (!writer_done.load() || n < kMinReads) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const long key = static_cast<long>(x % kKeys);
                const std::uint64_t floor = committed[static_cast<std::size_t>(key)].load();
                std::memset(buf.data(), 0x5A, 1100);  // covers every small record
                if (s.get(key, buf.data(), buf.size())) {
                    std::uint64_t g = 0;
                    Record head;
                    std::memcpy(&head, buf.data(), sizeof(head));
                    if (head.length <= 1000 || head.generation % 97 == 0) {
                        if (head.length > 1000) std::memset(buf.data() + head.length, 0x5A, 16);
                        l_torn += static_cast<std::uint64_t>(record_errors(buf, key, &g));
                        if (g < floor) ++l_back;  // older than a write that had completed
                    } else {
                        ++l_torn;
                    }
                }
                ++n;
                if ((n & 0x7F) == 0) reads_done[t].store(n);
            }
            reads_done[t].store(n);
            torn.fetch_add(l_torn);
            backwards.fetch_add(l_back);
            reads_total.fetch_add(n);
        });
    }

    // A thread that keeps taking a long-lived pin, reading every key through it
    // twice with a pause-free gap, and checking the two reads agree (a pinned
    // view never changes), then releasing it.
    std::thread pinner([&]() {
        std::vector<unsigned char> a(80000), b(80000);
        std::uint64_t errors = 0;
        while (!go.load()) std::this_thread::yield();
        while (!writer_done.load()) {
            VersionedStore::Pin pin = s.acquire_pin();
            for (long k = 0; k < kKeys; k += 5) {
                std::memset(a.data(), 0x5A, 1100);
                std::memset(b.data(), 0x5A, 1100);
                const bool fa = s.get_pinned(pin, k, a.data(), a.size());
                for (int spin = 0; spin < 200; ++spin) std::this_thread::yield();
                const bool fb = s.get_pinned(pin, k, b.data(), b.size());
                if (fa != fb) ++errors;
                if (fa && fb) {
                    Record ra, rb;
                    std::memcpy(&ra, a.data(), sizeof(ra));
                    std::memcpy(&rb, b.data(), sizeof(rb));
                    if (ra.generation != rb.generation || ra.length != rb.length ||
                        std::memcmp(a.data(), b.data(), ra.length) != 0) {
                        ++errors;
                    }
                }
            }
            s.release_pin(pin);
        }
        pin_errors.fetch_add(errors);
    });

    // Short-lived reader threads: registration and slot return under load.
    std::thread churn([&]() {
        while (!go.load()) std::this_thread::yield();
        while (!writer_done.load()) {
            std::thread t([&]() {
                std::vector<unsigned char> buf(80000);
                for (long k = 0; k < 8; ++k) (void)s.get(k, buf.data(), buf.size());
            });
            t.join();
        }
    });

    std::uint64_t writes = 0, erases = 0;
    bool writes_ok = true;
    std::thread writer([&]() {
        std::vector<unsigned char> rec;
        std::uint64_t x = 0xC0FFEE;
        while (ready.load() != kReaders) std::this_thread::yield();
        go.store(true);
        auto overlapped = [&]() {
            for (auto& r : reads_done) {
                if (r.load() < kMinReads / 2) return false;
            }
            return true;
        };
        for (std::uint64_t g = 1; g <= kWrites || !overlapped(); ++g) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            const long key = static_cast<long>(x % kKeys);
            if (x % 9 == 0) {
                s.erase(key);
                ++erases;
            } else if (x % 9 == 1) {
                // Multi-key commit: three keys, one publication.
                std::vector<unsigned char> r1, r2, r3;
                const long k2 = (key + 1) % kKeys, k3 = (key + 2) % kKeys;
                const std::size_t l1 = make_record(r1, g, key), l2 = make_record(r2, g, k2),
                                  l3 = make_record(r3, g, k3);
                const BatchOp ops[3] = {{BatchOp::Kind::Put, key, r1.data(), l1},
                                        {BatchOp::Kind::Put, k2, r2.data(), l2},
                                        {BatchOp::Kind::Put, k3, r3.data(), l3}};
                writes_ok = writes_ok && s.commit(ops, 3);
                committed[static_cast<std::size_t>(key)].store(g);
                committed[static_cast<std::size_t>(k2)].store(g);
                committed[static_cast<std::size_t>(k3)].store(g);
                ++writes;
            } else {
                const std::size_t len = make_record(rec, g, key);
                writes_ok = writes_ok && s.put(key, rec.data(), len);
                committed[static_cast<std::size_t>(key)].store(g);
                ++writes;
            }
        }
        writer_done.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();
    pinner.join();
    churn.join();

    NOVA_CHECK(writes_ok);
    NOVA_CHECK(torn.load() == 0);
    NOVA_CHECK(backwards.load() == 0);
    NOVA_CHECK(pin_errors.load() == 0);
    for (auto& r : reads_done) NOVA_CHECK(r.load() >= kMinReads);

    // Everyone has left. The cleaner must be able to finish the backlog, and
    // what remains must be just the current state.
    const nova::VersionedStats busy = s.stats();
    s.stop_cleaner();
    NOVA_CHECK(s.drain());
    const nova::VersionedStats st = s.stats();
    NOVA_CHECK(st.versions_retained == 0);
    NOVA_CHECK(st.versions_reclaimed == st.versions_published);
    NOVA_CHECK(st.reclaim_max_versions_in_pass <= static_cast<std::uint64_t>(batch));
    NOVA_CHECK(st.arena.chunks_freed > 100 && st.arena.ids_reused > 50);  // IDs recycled under readers
    NOVA_CHECK(st.epoch.slots_owned == 0);
    // Coverage, not correctness: the epoch really moved while everyone was
    // running. The number of advances is bounded by about two per cycle of the
    // pinning thread (bounded overtaking), i.e. by scheduling, so only a small
    // floor is asserted; the count is printed below. (The former floor of 100
    // was missed by about 1 sub-run in 70 on an idle machine: decision D-056.)
    NOVA_CHECK(st.epoch.advance_successes > 10);
    const char* err = s.validate();
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
    std::printf("  stress batch=%-4d writes=%llu erases=%llu reads=%llu published=%llu backlog_max=%llu "
                "passes=%llu epoch_advances=%llu (blocked %llu) chunk_ids_reused=%llu registrations=%llu\n",
                batch, static_cast<unsigned long long>(writes),
                static_cast<unsigned long long>(erases),
                static_cast<unsigned long long>(reads_total.load()),
                static_cast<unsigned long long>(st.versions_published),
                static_cast<unsigned long long>(busy.backlog_max),
                static_cast<unsigned long long>(st.reclaim_passes),
                static_cast<unsigned long long>(st.epoch.advance_successes),
                static_cast<unsigned long long>(st.epoch.advance_blocked),
                static_cast<unsigned long long>(st.arena.ids_reused),
                static_cast<unsigned long long>(st.epoch.registrations));
}

// ---- 2. Memory stops growing: a steady overwrite load with the real cleaner -------
void memory_stabilizes() {
    VersionedStore s;
    s.set_reclaim_batch(64);
    std::vector<unsigned char> v(2000, 7);
    std::atomic<bool> stop{false};
    std::thread reader([&]() {
        std::vector<unsigned char> buf(2000);
        while (!stop.load()) (void)s.get(3, buf.data(), buf.size());
    });
    std::uint64_t peak = 0, backlog_peak = 0;
    for (std::uint64_t i = 0; i < 60000; ++i) {
        v[0] = static_cast<unsigned char>(i);
        NOVA_CHECK(s.put(static_cast<long>(i % 32), v.data(), v.size()));
        if (i % 256 == 0) {
            // The writer must not outrun the cleaner without bound in this
            // test; if the backlog is large, let the cleaner catch up. (This
            // is test pacing, not something the store does.)
            while (s.backlog() > 20000) std::this_thread::yield();
            const std::uint64_t held = s.memory_held_bytes();
            if (held > peak) peak = held;
            if (s.backlog() > backlog_peak) backlog_peak = s.backlog();
        }
    }
    stop.store(true);
    reader.join();
    s.stop_cleaner();
    NOVA_CHECK(s.drain());
    const std::uint64_t settled = s.memory_held_bytes();
    // 60,000 writes of 2 KB each would be > 120 MB of values alone if nothing
    // were reclaimed, plus > 150 MB of versions and segments.
    NOVA_CHECK(peak < 80ull * 1024 * 1024);
    NOVA_CHECK(settled < 1ull * 1024 * 1024);  // 32 live values + one version + its segments
    NOVA_CHECK(s.stats().versions_reclaimed == 60000);
    NOVA_CHECK(s.validate() == nullptr);
    std::printf("  memory: 60000 writes, peak held %.1f MiB, settled %.1f KiB, backlog peak %llu\n",
                static_cast<double>(peak) / 1048576.0, static_cast<double>(settled) / 1024.0,
                static_cast<unsigned long long>(backlog_peak));
}

// ---- 3. Repeated construction and shutdown, with work still queued ---------------
void repeated_init_and_shutdown() {
    for (int round = 0; round < 200; ++round) {
        auto s = std::make_unique<VersionedStore>();
        NOVA_CHECK(s->ok());
        std::atomic<bool> stop{false};
        std::thread reader([&]() {
            unsigned char buf[64];
            while (!stop.load()) (void)s->get(1, buf, sizeof(buf));
        });
        const unsigned char v[64] = {static_cast<unsigned char>(round)};
        for (int i = 0; i < 200 + round; ++i) NOVA_CHECK(s->put(i % 7, v, sizeof(v)));
        // Shutdown contract: external operations stop first ...
        stop.store(true);
        reader.join();
        // ... then the store is destroyed, whatever is still in the queue and
        // wherever the cleaner happens to be.
        s.reset();
    }
    // A store that is never used, and one destroyed straight after a write.
    { VersionedStore unused; NOVA_CHECK(unused.ok()); }
    {
        VersionedStore s;
        const unsigned char v[8] = {1};
        NOVA_CHECK(s.put(1, v, sizeof(v)));
    }
}

}  // namespace

int main() {
    for (int batch : {1, 8, 64, 1024}) stress_readers_writer_cleaner(batch);
    memory_stabilizes();
    repeated_init_and_shutdown();
#ifdef NOVA_KV_TEST_HOOKS
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
#endif
    return nova_test::finish("reclamation_stress_test");
}
