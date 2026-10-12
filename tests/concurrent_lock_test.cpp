// concurrent_lock_test.cpp -- legality of concurrent use of the two locked
// variants (global_lock, rwlock): one writer and several readers on the same
// store. Not built for the `single` variant, where concurrent use is illegal.
//
// Every stored record is self-describing (generation, its complement, key,
// length, generation-derived payload), so a reader detects a torn or mixed
// record by itself. Under ThreadSanitizer this run is also the data-race check
// for the lock placement. The run is bounded by operation counts and a start
// barrier, not by sleeps.
#include "kvstore.h"

#include "build_info.h"
#include "inspect.h"
#include "test_util.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int kReaders = 4;
constexpr long kKeys = 48;
constexpr std::uint64_t kWriterOps = 60000;
constexpr std::uint64_t kMinReadsPerReader = 20000;
constexpr std::size_t kMaxRecord = 24 + 200;
constexpr unsigned char kSentinel = 0x5A;

struct Header {
    std::uint64_t generation;
    std::uint64_t generation_inverted;
    std::uint32_t key;
    std::uint32_t length;  // total record length including this header
};

unsigned char payload_byte(std::uint64_t generation, long key) {
    return static_cast<unsigned char>(generation * 31 + static_cast<std::uint64_t>(key));
}

std::size_t make_record(unsigned char* out, std::uint64_t generation, long key) {
    const std::size_t len = sizeof(Header) + static_cast<std::size_t>(generation % 201);
    Header h{generation, ~generation, static_cast<std::uint32_t>(key),
             static_cast<std::uint32_t>(len)};
    std::memcpy(out, &h, sizeof(h));
    std::memset(out + sizeof(h), payload_byte(generation, key), len - sizeof(h));
    return len;
}

// Returns false if the bytes returned for `key` are not one complete record.
bool record_is_consistent(const unsigned char* buf, long key) {
    Header h;
    std::memcpy(&h, buf, sizeof(h));
    if (h.generation_inverted != ~h.generation) return false;
    if (h.key != static_cast<std::uint32_t>(key)) return false;
    if (h.length != sizeof(Header) + h.generation % 201) return false;
    const unsigned char want = payload_byte(h.generation, key);
    for (std::size_t i = sizeof(Header); i < h.length; ++i) {
        if (buf[i] != want) return false;
    }
    for (std::size_t i = h.length; i < kMaxRecord; ++i) {
        if (buf[i] != kSentinel) return false;  // nothing copied beyond the record
    }
    return true;
}

}  // namespace

int main() {
    KVStore store;
    std::atomic<int> readers_ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> writer_done{false};
    std::atomic<std::uint64_t> reads_done[kReaders];
    std::atomic<std::uint64_t> torn{0}, hits{0}, misses{0};
    for (auto& r : reads_done) r.store(0);

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t]() {
            unsigned char buf[kMaxRecord];
            std::uint64_t x = 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(t + 1);
            std::uint64_t local_hits = 0, local_misses = 0, local_torn = 0, n = 0;
            readers_ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            while (!writer_done.load() || n < kMinReadsPerReader) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const long key = static_cast<long>(x % kKeys);
                std::memset(buf, kSentinel, sizeof(buf));
                if (store.get(key, buf, sizeof(buf))) {
                    ++local_hits;
                    if (!record_is_consistent(buf, key)) ++local_torn;
                } else {
                    ++local_misses;
                }
                ++n;
                if ((n & 0xFF) == 0) reads_done[t].store(n);
            }
            reads_done[t].store(n);
            hits.fetch_add(local_hits);
            misses.fetch_add(local_misses);
            torn.fetch_add(local_torn);
        });
    }

    // Expected final content, known only to the writer.
    std::vector<std::uint64_t> final_generation(kKeys, 0);  // 0 = absent
    std::uint64_t writes = 0, erases = 0;
    std::thread writer([&]() {
        unsigned char rec[kMaxRecord];
        std::uint64_t x = 0xC0FFEE;
        while (readers_ready.load() != kReaders) std::this_thread::yield();
        go.store(true);
        // Keep writing until the op budget is spent AND every reader has
        // demonstrably overlapped with the writer.
        auto readers_overlapped = [&]() {
            for (auto& r : reads_done) {
                if (r.load() < kMinReadsPerReader / 2) return false;
            }
            return true;
        };
        for (std::uint64_t generation = 1; generation <= kWriterOps || !readers_overlapped();
             ++generation) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            const long key = static_cast<long>(x % kKeys);
            if (x % 5 == 0) {
                const bool erased = store.erase(key);
                if (erased != (final_generation[static_cast<std::size_t>(key)] != 0)) torn.fetch_add(1);
                final_generation[static_cast<std::size_t>(key)] = 0;
                ++erases;
            } else {
                const std::size_t len = make_record(rec, generation, key);
                if (!store.put(key, rec, len)) torn.fetch_add(1);
                final_generation[static_cast<std::size_t>(key)] = generation;
                ++writes;
            }
        }
        writer_done.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    NOVA_CHECK(torn.load() == 0);
    NOVA_CHECK(hits.load() > 0);
    for (auto& r : reads_done) NOVA_CHECK(r.load() >= kMinReadsPerReader);

    // Quiescent: the store must equal the writer's record of what it did.
    unsigned char buf[kMaxRecord], want[kMaxRecord];
    for (long key = 0; key < kKeys; ++key) {
        const std::uint64_t generation = final_generation[static_cast<std::size_t>(key)];
        std::memset(buf, kSentinel, sizeof(buf));
        const bool found = store.get(key, buf, sizeof(buf));
        NOVA_CHECK(found == (generation != 0));
        if (found && generation != 0) {
            const std::size_t len = make_record(want, generation, key);
            NOVA_CHECK(std::memcmp(buf, want, len) == 0);
        }
    }
    const char* err = NovaKvInspector::validate(store);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);

    std::printf("  variant=%s readers=%d puts=%llu erases=%llu read_hits=%llu read_misses=%llu torn=%llu\n",
                nova_kv_sync_mode_name(), kReaders, static_cast<unsigned long long>(writes),
                static_cast<unsigned long long>(erases),
                static_cast<unsigned long long>(hits.load()),
                static_cast<unsigned long long>(misses.load()),
                static_cast<unsigned long long>(torn.load()));
    return nova_test::finish("concurrent_lock_test");
}
