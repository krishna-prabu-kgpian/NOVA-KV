// kvstore_test.cpp -- the Part A sanity test (PDF p. 2: "basic insert, lookup,
// overwrite, and erase"), extended to the full A1 contract. Everything is
// exercised through the public KVStore API; the internal inspector is used only
// to check structural invariants and to confirm that growth, rebuilds and chunk
// recycling actually happened. Built once per synchronization variant.
#include "kvstore.h"

#include "hash.h"
#include "inspect.h"
#include "test_util.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (std::size_t i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        v[i] = static_cast<unsigned char>(x);
    }
    return v;
}

bool put(KVStore& s, long key, const Bytes& v) { return s.put(key, v.data(), v.size()); }

// True iff the store holds exactly `expect` under `key`. Reads into a buffer
// with guard bytes on both sides to catch any write outside [0, len).
bool holds(const KVStore& s, long key, const Bytes& expect) {
    const std::size_t pad = 16;
    Bytes buf(expect.size() + 2 * pad, 0xCD);
    if (!s.get(key, buf.data() + pad, expect.size())) return false;
    for (std::size_t i = 0; i < pad; ++i) {
        if (buf[i] != 0xCD || buf[pad + expect.size() + i] != 0xCD) return false;
    }
    return expect.empty() || std::memcmp(buf.data() + pad, expect.data(), expect.size()) == 0;
}

bool missing(const KVStore& s, long key) {
    unsigned char probe[8];
    return !s.get(key, probe, sizeof(probe)) && !s.get(key, nullptr, 0);
}

void check_valid(const KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// ---- The four operations the assignment names --------------------------------
void test_insert_lookup_overwrite_erase() {
    KVStore s;
    const Bytes a = pattern(32, 1), b = pattern(32, 2);
    NOVA_CHECK(missing(s, 7));
    NOVA_CHECK(put(s, 7, a));              // insert
    NOVA_CHECK(holds(s, 7, a));            // lookup
    NOVA_CHECK(missing(s, 8));             // a different key is still absent
    NOVA_CHECK(put(s, 7, b));              // overwrite
    NOVA_CHECK(holds(s, 7, b));
    NOVA_CHECK(NovaKvInspector::stats(s).live_keys == 1);
    NOVA_CHECK(s.erase(7));                // erase
    NOVA_CHECK(missing(s, 7));
    NOVA_CHECK(!s.erase(7));               // erasing again reports absence
    NOVA_CHECK(!s.erase(12345));           // never inserted
    NOVA_CHECK(put(s, 7, a));              // re-insert after erase
    NOVA_CHECK(holds(s, 7, a));
    check_valid(s);
}

// put() must copy: the caller may reuse or destroy its buffer afterwards.
void test_put_copies_value() {
    KVStore s;
    Bytes v = pattern(64, 3);
    const Bytes original = v;
    NOVA_CHECK(put(s, 1, v));
    std::memset(v.data(), 0, v.size());
    NOVA_CHECK(holds(s, 1, original));
}

void test_key_range() {
    KVStore s;
    const long keys[] = {0, 1, -1, 2, -2, LONG_MAX, LONG_MIN, LONG_MAX - 1, LONG_MIN + 1,
                         1L << 32, -(1L << 32), 99900099L};
    std::uint64_t seed = 100;
    for (long k : keys) NOVA_CHECK(put(s, k, pattern(24, seed++)));
    seed = 100;
    for (long k : keys) NOVA_CHECK(holds(s, k, pattern(24, seed++)));
    NOVA_CHECK(NovaKvInspector::stats(s).live_keys == sizeof(keys) / sizeof(keys[0]));
    NOVA_CHECK(s.erase(LONG_MIN));
    NOVA_CHECK(missing(s, LONG_MIN));
    NOVA_CHECK(holds(s, LONG_MAX, pattern(24, 105)));
    NOVA_CHECK(holds(s, -1, pattern(24, 102)));
    check_valid(s);
}

// Zero-length values exist and are distinguishable from missing keys (D-012).
void test_zero_length_values() {
    KVStore s;
    unsigned char buf[4] = {9, 9, 9, 9};
    NOVA_CHECK(s.put(5, nullptr, 0));          // null is fine when len == 0
    NOVA_CHECK(s.get(5, nullptr, 0));          // present ...
    NOVA_CHECK(!s.get(6, nullptr, 0));         // ... unlike a missing key
    NOVA_CHECK(s.get(5, buf, sizeof(buf)));    // present with a real buffer too
    NOVA_CHECK(buf[0] == 9 && buf[3] == 9);    // nothing is written for an empty value
    const unsigned char dummy = 1;
    NOVA_CHECK(s.put(6, &dummy, 0));           // non-null pointer with len == 0
    NOVA_CHECK(s.get(6, nullptr, 0));
    NOVA_CHECK(NovaKvInspector::stats(s).chunks_live == 0);  // empty values use no arena
    // empty -> non-empty -> empty
    const Bytes v = pattern(10, 4);
    NOVA_CHECK(put(s, 5, v) && holds(s, 5, v));
    NOVA_CHECK(!s.get(5, nullptr, 0));         // now needs 10 bytes
    NOVA_CHECK(s.put(5, nullptr, 0));
    NOVA_CHECK(s.get(5, nullptr, 0));
    NOVA_CHECK(s.erase(5));
    NOVA_CHECK(!s.get(5, nullptr, 0));         // erased empty value is missing
    NOVA_CHECK(!s.erase(5));
    check_valid(s);
}

// Invalid pointers and undersized buffers (D-012).
void test_invalid_arguments_and_small_buffers() {
    KVStore s;
    const Bytes v = pattern(32, 5);
    NOVA_CHECK(!s.put(1, nullptr, 8));          // nullptr with len > 0
    NOVA_CHECK(missing(s, 1));                  // and nothing was stored
    NOVA_CHECK(put(s, 1, v));
    NOVA_CHECK(!s.put(1, nullptr, 8));          // a rejected put keeps the old value
    NOVA_CHECK(holds(s, 1, v));

    Bytes buf(64, 0xEE);
    const Bytes untouched = buf;
    NOVA_CHECK(!s.get(1, buf.data(), 31));      // one byte too small
    NOVA_CHECK(buf == untouched);               // buffer not modified, no truncated copy
    NOVA_CHECK(!s.get(1, buf.data(), 0));
    NOVA_CHECK(!s.get(1, buf.data(), 1));
    NOVA_CHECK(buf == untouched);
    NOVA_CHECK(!s.get(1, nullptr, 32));         // nullptr destination, sufficient size
    NOVA_CHECK(!s.get(1, nullptr, 0));

    NOVA_CHECK(s.get(1, buf.data(), 32));       // exact size
    NOVA_CHECK(std::memcmp(buf.data(), v.data(), 32) == 0);
    NOVA_CHECK(buf[32] == 0xEE);

    // A larger buffer receives exactly the value; the tail is left alone.
    Bytes big(64, 0xEE);
    NOVA_CHECK(s.get(1, big.data(), big.size()));
    NOVA_CHECK(std::memcmp(big.data(), v.data(), 32) == 0);
    bool tail_untouched = true;
    for (std::size_t i = 32; i < big.size(); ++i) tail_untouched = tail_untouched && big[i] == 0xEE;
    NOVA_CHECK(tail_untouched);

    // Longer than the supported maximum (1 GiB): rejected before the pointer is used.
    const unsigned char one = 0;
    NOVA_CHECK(!s.put(2, &one, (std::size_t{1} << 30) + 1));
    NOVA_CHECK(!s.put(2, &one, SIZE_MAX));
    NOVA_CHECK(missing(s, 2));
    check_valid(s);
}

// Arbitrary binary content and many different lengths, including the chunk
// boundary and values larger than a chunk.
void test_binary_and_variable_length_values() {
    KVStore s;
    Bytes zeros(100, 0x00), ones(100, 0xFF), mixed(256);
    for (std::size_t i = 0; i < mixed.size(); ++i) mixed[i] = static_cast<unsigned char>(i);
    NOVA_CHECK(put(s, 1, zeros) && put(s, 2, ones) && put(s, 3, mixed));
    NOVA_CHECK(holds(s, 1, zeros) && holds(s, 2, ones) && holds(s, 3, mixed));

    const std::size_t lengths[] = {0,     1,     2,     7,      8,      9,      31,     32,
                                   33,    255,   256,   4095,   4096,   65535,  65536,  65537,
                                   70000, 200000, 1u << 20};
    long key = 1000;
    for (std::size_t len : lengths) NOVA_CHECK(put(s, key++, pattern(len, len)));
    key = 1000;
    for (std::size_t len : lengths) NOVA_CHECK(holds(s, key++, pattern(len, len)));
    // Earlier values survived all the later appends.
    NOVA_CHECK(holds(s, 1, zeros) && holds(s, 2, ones) && holds(s, 3, mixed));
    check_valid(s);

    // Overwrite across size classes in both directions.
    NOVA_CHECK(put(s, 1000, pattern(1u << 20, 77)));
    NOVA_CHECK(holds(s, 1000, pattern(1u << 20, 77)));
    NOVA_CHECK(put(s, 1018, pattern(3, 78)));  // the 1 MiB value shrinks to 3 bytes
    NOVA_CHECK(holds(s, 1018, pattern(3, 78)));
    NOVA_CHECK(!s.get(1000, mixed.data(), mixed.size()));  // 256-byte buffer, 1 MiB value
    check_valid(s);
}

// Keys forced into the same partition AND the same start slot of the real
// store, found by scanning the production hash.
void test_collisions_in_the_store() {
    const std::uint64_t slot_mask = nova::kInitialSegmentCapacity - 1;
    const std::uint64_t h0 = nova::hash_key(1);
    std::vector<long> chain;
    for (long k = 2; chain.size() < 6; ++k) {
        const std::uint64_t h = nova::hash_key(k);
        if (nova::partition_of(h) == nova::partition_of(h0) &&
            ((h >> nova::kPartitionBits) & slot_mask) == ((h0 >> nova::kPartitionBits) & slot_mask)) {
            chain.push_back(k);
        }
    }
    chain.insert(chain.begin(), 1);  // 7 keys, one probe chain

    KVStore s;
    for (std::size_t i = 0; i < chain.size(); ++i) NOVA_CHECK(put(s, chain[i], pattern(20, i)));
    for (std::size_t i = 0; i < chain.size(); ++i) NOVA_CHECK(holds(s, chain[i], pattern(20, i)));
    NOVA_CHECK(NovaKvInspector::stats(s).segments_allocated == 1);  // really one segment

    NOVA_CHECK(s.erase(chain[2]));  // a middle link of the chain
    NOVA_CHECK(missing(s, chain[2]));
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i != 2) NOVA_CHECK(holds(s, chain[i], pattern(20, i)));
    }
    NOVA_CHECK(NovaKvInspector::stats(s).tombstones == 1);
    NOVA_CHECK(put(s, chain[6], pattern(20, 60)));  // overwrite beyond the tombstone
    NOVA_CHECK(holds(s, chain[6], pattern(20, 60)));
    NOVA_CHECK(NovaKvInspector::stats(s).live_keys == 6);  // not duplicated
    NOVA_CHECK(put(s, chain[2], pattern(20, 20)));  // re-insert reuses the tombstone
    NOVA_CHECK(NovaKvInspector::stats(s).tombstones == 0);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        NOVA_CHECK(holds(s, chain[i], pattern(20, i == 6 ? 60 : (i == 2 ? 20 : i))));
    }
    check_valid(s);
}

// Enough keys that every segment has to grow past its initial capacity.
void test_many_keys_and_growth() {
    KVStore s;
    const long n = 300000;
    bool ok = true;
    for (long i = 0; i < n; ++i) {
        const long key = i * 100000 + (i % 100);  // sparse, like the workload's trade ids
        ok = ok && s.put(key, &i, sizeof(i));
    }
    NOVA_CHECK(ok);
    NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.live_keys == static_cast<std::uint64_t>(n));
    NOVA_CHECK(st.segments_allocated == nova::kPartitionCount);
    NOVA_CHECK(st.segment_growths >= nova::kPartitionCount);  // every segment grew
    NOVA_CHECK(st.slot_capacity * 7 >= st.live_keys * 10);    // load bound respected
    ok = true;
    for (long i = 0; i < n; ++i) {
        long out = -1;
        ok = ok && s.get(i * 100000 + (i % 100), &out, sizeof(out)) && out == i;
    }
    NOVA_CHECK(ok);
    NOVA_CHECK(missing(s, 1));  // i*100000 + i%100 never equals 1 ... 99 for i > 0
    check_valid(s);

    // Erase two thirds: tombstone rebuilds must run, survivors stay readable.
    ok = true;
    for (long i = 0; i < n; ++i) {
        if (i % 3 != 0) ok = ok && s.erase(i * 100000 + (i % 100));
    }
    NOVA_CHECK(ok);
    st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.live_keys == static_cast<std::uint64_t>(n / 3));
    NOVA_CHECK(st.segment_rebuilds > 0);
    NOVA_CHECK(st.tombstones * 5 < st.slot_capacity);
    ok = true;
    for (long i = 0; i < n; ++i) {
        long out = -1;
        const bool found = s.get(i * 100000 + (i % 100), &out, sizeof(out));
        ok = ok && (i % 3 == 0 ? (found && out == i) : !found);
    }
    NOVA_CHECK(ok);
    check_valid(s);
}

// Repeated overwrites must not grow memory without bound: dead chunks are
// freed and their IDs reused.
void test_overwrite_churn_reclaims_chunks() {
    KVStore s;
    const long keys = 64;
    for (long k = 0; k < keys; ++k) NOVA_CHECK(put(s, k, pattern(1000, static_cast<std::uint64_t>(k))));
    bool ok = true;
    for (std::uint64_t round = 1; round <= 300; ++round) {
        for (long k = 0; k < keys; ++k) ok = ok && put(s, k, pattern(1000, round * 1000 + static_cast<std::uint64_t>(k)));
    }
    NOVA_CHECK(ok);
    for (long k = 0; k < keys; ++k) NOVA_CHECK(holds(s, k, pattern(1000, 300 * 1000 + static_cast<std::uint64_t>(k))));
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.value_bytes_live == 64 * 1000);
    NOVA_CHECK(st.chunks_freed > 0 && st.chunk_ids_reused > 0);
    // 19.2 MB were written in total; what is held must be a small multiple of
    // the 64 KB that is live.
    NOVA_CHECK(st.chunk_bytes_reserved <= 4 * 65536);
    NOVA_CHECK(st.chunk_ids_issued <= 4);
    check_valid(s);

    for (long k = 0; k < keys; ++k) NOVA_CHECK(s.erase(k));
    const NovaKvStats empty = NovaKvInspector::stats(s);
    NOVA_CHECK(empty.live_keys == 0 && empty.value_bytes_live == 0);
    NOVA_CHECK(empty.chunks_live <= 1);  // at most the reset writable chunk
    check_valid(s);
}

// Random operations compared against std::unordered_map, an independent
// reference model that shares no code with the store (test-only use).
void test_randomized_against_reference_model(std::uint64_t seed, int ops) {
    KVStore s;
    std::unordered_map<long, Bytes> model;
    std::uint64_t x = seed;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    // 3000 keys spread over the whole long range, plus the extremes.
    std::vector<long> pool;
    for (int i = 0; i < 3000; ++i) pool.push_back(static_cast<long>(next()));
    pool.push_back(LONG_MIN);
    pool.push_back(LONG_MAX);
    pool.push_back(0);
    pool.push_back(-1);

    Bytes buf(80000);
    int mismatches = 0;
    auto compare_all = [&]() {
        for (const auto& kv : model) {
            if (!holds(s, kv.first, kv.second)) ++mismatches;
        }
        for (long k : pool) {
            if (model.find(k) == model.end() && !missing(s, k)) ++mismatches;
        }
        if (NovaKvInspector::stats(s).live_keys != model.size()) ++mismatches;
        check_valid(s);
    };

    for (int op = 0; op < ops; ++op) {
        const long key = pool[next() % pool.size()];
        const std::uint64_t r = next() % 100;
        if (r < 55) {
            std::size_t len;
            const std::uint64_t c = next() % 1000;
            if (c < 50) len = 0;
            else if (c < 600) len = 1 + next() % 64;
            else if (c < 995) len = 1 + next() % 600;
            else len = 65000 + next() % 10000;  // around and above one chunk
            Bytes v = pattern(len, next());
            if (!s.put(key, v.data(), v.size())) ++mismatches;
            model[key] = std::move(v);
        } else if (r < 80) {
            const bool erased = s.erase(key);
            if (erased != (model.erase(key) == 1)) ++mismatches;
        } else {
            auto it = model.find(key);
            if (it == model.end()) {
                if (s.get(key, buf.data(), buf.size())) ++mismatches;
            } else {
                const Bytes& want = it->second;
                if (!s.get(key, buf.data(), buf.size())) ++mismatches;
                else if (!want.empty() && std::memcmp(buf.data(), want.data(), want.size()) != 0) ++mismatches;
                // One byte short must fail (when there is a byte to remove).
                if (!want.empty() && s.get(key, buf.data(), want.size() - 1)) ++mismatches;
            }
        }
        if (op % 20000 == 19999) compare_all();
    }
    compare_all();
    NOVA_CHECK(mismatches == 0);
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.chunks_freed > 0);
    std::printf("  randomized seed=%llu ops=%d: live=%llu chunks_live=%llu freed=%llu ids_reused=%llu "
                "growths=%llu rebuilds=%llu\n",
                static_cast<unsigned long long>(seed), ops,
                static_cast<unsigned long long>(st.live_keys),
                static_cast<unsigned long long>(st.chunks_live),
                static_cast<unsigned long long>(st.chunks_freed),
                static_cast<unsigned long long>(st.chunk_ids_reused),
                static_cast<unsigned long long>(st.segment_growths),
                static_cast<unsigned long long>(st.segment_rebuilds));
}

// A small key space concentrated in one partition, so that segment growth and
// tombstone rebuilds are part of the randomized comparison as well.
void test_randomized_single_partition(std::uint64_t seed, int ops) {
    std::vector<long> pool;
    for (long k = 0; pool.size() < 2500; ++k) {
        if (nova::partition_of(nova::hash_key(k)) == 17) pool.push_back(k);
    }
    KVStore s;
    std::unordered_map<long, Bytes> model;
    std::uint64_t x = seed;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    int mismatches = 0;
    for (int op = 0; op < ops; ++op) {
        // Alternate phases that mostly insert and mostly erase.
        const bool erase_phase = (op / 4000) % 2 == 1;
        const long key = pool[next() % pool.size()];
        if (next() % 100 < (erase_phase ? 25u : 75u)) {
            Bytes v = pattern(next() % 40, next());
            if (!s.put(key, v.data(), v.size())) ++mismatches;
            model[key] = std::move(v);
        } else {
            if (s.erase(key) != (model.erase(key) == 1)) ++mismatches;
        }
    }
    for (long k : pool) {
        auto it = model.find(k);
        if (it == model.end() ? !missing(s, k) : !holds(s, k, it->second)) ++mismatches;
    }
    NOVA_CHECK(mismatches == 0);
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.segments_allocated == 1);
    NOVA_CHECK(st.live_keys == model.size());
    NOVA_CHECK(st.segment_growths > 0 && st.segment_rebuilds > 0);
    check_valid(s);
}

// Two stores are fully independent.
void test_independent_stores() {
    KVStore a, b;
    NOVA_CHECK(put(a, 1, pattern(8, 1)));
    NOVA_CHECK(missing(b, 1));
    NOVA_CHECK(put(b, 1, pattern(8, 2)));
    NOVA_CHECK(holds(a, 1, pattern(8, 1)) && holds(b, 1, pattern(8, 2)));
    NOVA_CHECK(a.erase(1));
    NOVA_CHECK(holds(b, 1, pattern(8, 2)));
}

}  // namespace

int main() {
    test_insert_lookup_overwrite_erase();
    test_put_copies_value();
    test_key_range();
    test_zero_length_values();
    test_invalid_arguments_and_small_buffers();
    test_binary_and_variable_length_values();
    test_collisions_in_the_store();
    test_many_keys_and_growth();
    test_overwrite_churn_reclaims_chunks();
    test_randomized_against_reference_model(0x1234567ULL, 150000);
    test_randomized_against_reference_model(0xDEADBEEFCAFEULL, 150000);
    test_randomized_against_reference_model(42, 150000);
    test_randomized_single_partition(7, 120000);
    test_independent_stores();
    return nova_test::finish("kvstore_test");
}
