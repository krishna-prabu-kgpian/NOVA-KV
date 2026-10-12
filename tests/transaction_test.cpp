// transaction_test.cpp -- Part C.1: the public Transaction API (Appendix A).
//
// Built once per store variant and written against the public API only (plus
// the internal inspector for statistics and invariant checks). Single-threaded:
// what a Transaction stages, publishes, refuses and discards. Concurrency is
// txn_concurrency_test.cpp; allocation failure is txn_failure_test.cpp.
//
// Semantics under test (src/kvstore.cpp, docs/transactions.md):
//   - put() stages a copy; nothing is visible before commit();
//   - several puts to one key: the last one wins;
//   - commit() publishes everything staged as ONE publication, each touched
//     partition cloned once (rcu build: checked through the counters);
//   - an empty commit succeeds and publishes nothing;
//   - commit() works once; afterwards puts are ignored and commits fail;
//   - an invalid put makes commit() fail with nothing published;
//   - destruction without commit() discards.
#include "kvstore.h"

#include "hash.h"
#include "inspect.h"
#include "test_util.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 131 + i * 17 + 3);
    return v;
}

bool put(KVStore& s, long key, const Bytes& v) { return s.put(key, v.data(), v.size()); }
void stage(Transaction& t, long key, const Bytes& v) { t.put(key, v.data(), v.size()); }

bool holds(const KVStore& s, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    if (!s.get(key, buf.data(), expect.size())) return false;
    if (std::memcmp(buf.data(), expect.data(), expect.size()) != 0) return false;
    if (buf[expect.size()] != 0xCD) return false;                       // nothing beyond the value
    if (!expect.empty() && s.get(key, buf.data(), expect.size() - 1)) return false;  // exact length
    return true;
}

// Absent: not found even with a buffer larger than any value these tests store.
bool missing(const KVStore& s, long key) {
    static std::vector<unsigned char> big(300000);
    return !s.get(key, nullptr, 0) && !s.get(key, big.data(), big.size());
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

const bool kVersioned = NovaKvInspector::is_versioned();

std::uint64_t published(const KVStore& s) { return NovaKvInspector::stats(s).versions_published; }
std::uint64_t clones(const KVStore& s) { return NovaKvInspector::stats(s).segments_built; }
std::uint32_t partition(long key) { return nova::partition_of(nova::hash_key(key)); }

// `count` keys starting the search at `from` that all fall into partition `p`.
std::vector<long> keys_in_partition(std::uint32_t p, std::size_t count, long from = 0) {
    std::vector<long> out;
    for (long k = from; out.size() < count; ++k) {
        if (partition(k) == p) out.push_back(k);
    }
    return out;
}

// ---- Staging -------------------------------------------------------------------------

void test_staging_is_invisible_until_commit() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(20, 1)));
    const std::uint64_t pub0 = published(s);
    {
        Transaction t(s);
        for (long k = 1; k <= 5; ++k) stage(t, k, pattern(30, 100 + static_cast<std::uint64_t>(k)));
        // Nothing staged is visible, and the existing value is untouched.
        NOVA_CHECK(holds(s, 1, pattern(20, 1)));
        for (long k = 2; k <= 5; ++k) NOVA_CHECK(missing(s, k));
        if (kVersioned) NOVA_CHECK(published(s) == pub0);
        NOVA_CHECK(t.commit());
        for (long k = 1; k <= 5; ++k) NOVA_CHECK(holds(s, k, pattern(30, 100 + static_cast<std::uint64_t>(k))));
        if (kVersioned) NOVA_CHECK(published(s) == pub0 + 1);  // five keys, ONE publication
    }
    for (long k = 1; k <= 5; ++k) NOVA_CHECK(holds(s, k, pattern(30, 100 + static_cast<std::uint64_t>(k))));
    check_valid(s);
}

// The caller's buffer may be changed, reused or freed right after put().
void test_values_are_copied_at_staging() {
    KVStore s;
    Transaction t(s);
    unsigned char scratch[64];
    for (long k = 0; k < 50; ++k) {
        const Bytes v = pattern(sizeof(scratch), static_cast<std::uint64_t>(k));
        std::memcpy(scratch, v.data(), sizeof(scratch));
        t.put(k, scratch, sizeof(scratch));
        std::memset(scratch, 0xEE, sizeof(scratch));  // reused for the next key
    }
    {
        Bytes* heap = new Bytes(pattern(5000, 777));
        t.put(1000, heap->data(), heap->size());
        std::memset(heap->data(), 0, heap->size());
        delete heap;                                   // destroyed before commit
    }
    NOVA_CHECK(t.commit());
    for (long k = 0; k < 50; ++k) NOVA_CHECK(holds(s, k, pattern(64, static_cast<std::uint64_t>(k))));
    NOVA_CHECK(holds(s, 1000, pattern(5000, 777)));
    check_valid(s);
}

void test_last_write_wins() {
    KVStore s;
    NOVA_CHECK(put(s, 7, pattern(10, 1)));
    const std::uint64_t pub0 = published(s);
    Transaction t(s);
    stage(t, 7, pattern(40, 2));     // overwrites the published value ...
    stage(t, 8, pattern(8, 3));
    stage(t, 7, pattern(3000, 4));   // ... then itself, with a different length ...
    stage(t, 9, pattern(8, 5));
    stage(t, 7, pattern(12, 6));     // ... and the last one wins
    stage(t, 8, Bytes());            // last write to 8 is an empty value
    NOVA_CHECK(t.commit());
    NOVA_CHECK(holds(s, 7, pattern(12, 6)));
    NOVA_CHECK(holds(s, 8, Bytes()));
    NOVA_CHECK(holds(s, 9, pattern(8, 5)));
    if (kVersioned) NOVA_CHECK(published(s) == pub0 + 1);
    check_valid(s);
}

// ---- Size of a commit: 1, 4, 16, 100 keys; one publication; one clone per partition ----

void test_commit_sizes() {
    for (int n : {1, 4, 16, 100}) {
        KVStore s;
        // Some state beforehand, so that the commit both inserts and overwrites.
        for (long k = 0; k < 40; ++k) NOVA_CHECK(put(s, k * 3, pattern(24, 9000 + static_cast<std::uint64_t>(k))));
        NovaKvInspector::settle(s);
        const std::uint64_t pub0 = published(s), clones0 = clones(s);
        std::set<std::uint32_t> partitions;
        Transaction t(s);
        for (long k = 0; k < n; ++k) {
            stage(t, k, pattern(32, static_cast<std::uint64_t>(n) * 1000 + static_cast<std::uint64_t>(k)));
            partitions.insert(partition(k));
        }
        NOVA_CHECK(t.commit());
        for (long k = 0; k < n; ++k) {
            NOVA_CHECK(holds(s, k, pattern(32, static_cast<std::uint64_t>(n) * 1000 + static_cast<std::uint64_t>(k))));
        }
        for (long k = 0; k < 40; ++k) {
            if (k * 3 >= n) NOVA_CHECK(holds(s, k * 3, pattern(24, 9000 + static_cast<std::uint64_t>(k))));
        }
        if (kVersioned) {
            NOVA_CHECK(published(s) == pub0 + 1);
            NOVA_CHECK(clones(s) == clones0 + partitions.size());  // each touched partition cloned once
            std::printf("  %3d keys: 1 publication, %zu partitions touched, %llu segments cloned\n", n,
                        partitions.size(), static_cast<unsigned long long>(clones(s) - clones0));
        }
        if (n >= 16) NOVA_CHECK(partitions.size() >= 8);  // really spans several partitions
        check_valid(s);
    }
}

// 100 keys that all live in ONE partition: still one clone, however many of its
// keys the commit changes (it grows the segment several times privately).
void test_many_keys_one_partition() {
    KVStore s;
    const std::vector<long> ks = keys_in_partition(11, 100);
    const std::uint64_t pub0 = published(s), clones0 = clones(s);
    Transaction t(s);
    for (std::size_t i = 0; i < ks.size(); ++i) stage(t, ks[i], pattern(16, i));
    for (std::size_t i = 0; i < ks.size(); i += 3) stage(t, ks[i], pattern(20, 500 + i));  // repeats
    NOVA_CHECK(t.commit());
    for (std::size_t i = 0; i < ks.size(); ++i) {
        NOVA_CHECK(holds(s, ks[i], i % 3 == 0 ? pattern(20, 500 + i) : pattern(16, i)));
    }
    if (kVersioned) {
        NOVA_CHECK(published(s) == pub0 + 1);
        NOVA_CHECK(clones(s) == clones0 + 1);
    }
    check_valid(s);
}

void test_large_and_zero_length_values() {
    KVStore s;
    const std::size_t sizes[] = {0, 1, 100, 65536, 65537, 70000, 200000, 0, 33};
    {
        Transaction t(s);
        long k = 0;
        for (std::size_t len : sizes) stage(t, k++, pattern(len, len + 5));
        t.put(100, nullptr, 0);  // an empty value may be staged from a null pointer
        NOVA_CHECK(t.commit());
    }
    long k = 0;
    for (std::size_t len : sizes) NOVA_CHECK(holds(s, k++, pattern(len, len + 5)));
    NOVA_CHECK(s.get(100, nullptr, 0));   // present, empty: distinguishable from absent
    NOVA_CHECK(!s.get(101, nullptr, 0));
    check_valid(s);
    // Overwrite large with small and small with large in one commit.
    {
        Transaction t(s);
        stage(t, 6, pattern(4, 1));          // was 200000 bytes
        stage(t, 1, pattern(150000, 2));     // was 1 byte
        stage(t, 0, pattern(9, 3));          // was empty
        stage(t, 2, Bytes());                // becomes empty
        NOVA_CHECK(t.commit());
    }
    NOVA_CHECK(holds(s, 6, pattern(4, 1)) && holds(s, 1, pattern(150000, 2)));
    NOVA_CHECK(holds(s, 0, pattern(9, 3)) && holds(s, 2, Bytes()));
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// ---- commit(): empty, once, after failure --------------------------------------------

void test_empty_commit() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(8, 1)));
    const std::uint64_t pub0 = published(s);
    Transaction t(s);
    NOVA_CHECK(t.commit());             // nothing staged: success, nothing published
    if (kVersioned) NOVA_CHECK(published(s) == pub0);
    NOVA_CHECK(!t.commit());            // a Transaction commits once
    NOVA_CHECK(holds(s, 1, pattern(8, 1)));
    check_valid(s);
}

void test_commit_only_once() {
    KVStore s;
    Transaction t(s);
    stage(t, 1, pattern(8, 1));
    NOVA_CHECK(t.commit());
    const std::uint64_t pub1 = published(s);
    NOVA_CHECK(!t.commit());            // not published again
    stage(t, 2, pattern(8, 2));         // ignored: the Transaction is finished
    stage(t, 1, pattern(8, 99));
    NOVA_CHECK(!t.commit());
    NOVA_CHECK(holds(s, 1, pattern(8, 1)) && missing(s, 2));
    if (kVersioned) NOVA_CHECK(published(s) == pub1);
    check_valid(s);
}

void test_abandoned_transaction() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(8, 1)));
    const std::uint64_t pub0 = published(s);
    {
        Transaction t(s);
        for (long k = 0; k < 500; ++k) stage(t, k, pattern(300, static_cast<std::uint64_t>(k)));
    }                                    // destroyed without commit()
    { Transaction untouched(s); }        // and one that never staged anything
    NOVA_CHECK(holds(s, 1, pattern(8, 1)));
    NOVA_CHECK(missing(s, 0) && missing(s, 2) && missing(s, 499));
    if (kVersioned) NOVA_CHECK(published(s) == pub0);
    check_valid(s);
}

// put() returns void, so a put that cannot be staged makes commit() fail, with
// NONE of the transaction's writes published: neither those staged before the
// bad one nor those after it.
void test_invalid_put_fails_the_commit() {
    KVStore s;
    NOVA_CHECK(put(s, 1, pattern(8, 1)));
    const std::uint64_t pub0 = published(s);
    {
        Transaction t(s);
        stage(t, 1, pattern(8, 50));
        stage(t, 2, pattern(8, 51));
        t.put(3, nullptr, 16);          // null value with a length
        stage(t, 4, pattern(8, 52));
        NOVA_CHECK(!t.commit());
        NOVA_CHECK(!t.commit());
    }
    {
        Transaction t(s);
        unsigned char one = 0;
        stage(t, 5, pattern(8, 53));
        t.put(6, &one, (std::size_t{1} << 30) + 1);  // above the value-size limit; never dereferenced
        NOVA_CHECK(!t.commit());
    }
    NOVA_CHECK(holds(s, 1, pattern(8, 1)));
    for (long k = 2; k <= 6; ++k) NOVA_CHECK(missing(s, k));
    if (kVersioned) NOVA_CHECK(published(s) == pub0);
    check_valid(s);
}

// ---- The way the supplied workload generator uses it ----------------------------------
// std::optional<Transaction>, re-emplaced after every commit, and a final
// commit() on a transaction that may be empty.
void test_generator_usage_pattern() {
    for (int txn_size : {1, 4, 16, 7}) {
        KVStore s;
        std::optional<Transaction> txn;
        txn.emplace(s);
        long count = 0;
        for (long k = 0; k < 160; ++k) {
            const Bytes v = pattern(48, static_cast<std::uint64_t>(k));
            txn->put(k, v.data(), v.size());
            if (++count % txn_size == 0) {
                NOVA_CHECK(txn->commit());
                txn.emplace(s);
            }
        }
        NOVA_CHECK(txn->commit());       // the tail, or an empty transaction
        for (long k = 0; k < 160; ++k) NOVA_CHECK(holds(s, k, pattern(48, static_cast<std::uint64_t>(k))));
        if (kVersioned) NOVA_CHECK(published(s) == static_cast<std::uint64_t>((160 + txn_size - 1) / txn_size));
        check_valid(s);
    }
}

void test_transactions_on_independent_stores() {
    KVStore a, b;
    Transaction ta(a), tb(b);
    stage(ta, 1, pattern(8, 1));
    stage(tb, 1, pattern(8, 2));
    stage(tb, 2, pattern(8, 3));
    NOVA_CHECK(ta.commit());
    NOVA_CHECK(holds(a, 1, pattern(8, 1)) && missing(a, 2) && missing(b, 1));
    NOVA_CHECK(tb.commit());
    NOVA_CHECK(holds(a, 1, pattern(8, 1)) && holds(b, 1, pattern(8, 2)) && holds(b, 2, pattern(8, 3)));
}

// ---- Randomized: transactions mixed with put and erase, against a reference model -------

std::uint64_t next(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

void test_randomized_against_reference_model(std::uint64_t seed, int rounds) {
    KVStore s;
    std::unordered_map<long, Bytes> model;
    std::uint64_t rng = seed;
    std::uint64_t commits = 0, staged_total = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::uint64_t what = next(rng) % 10;
        if (what < 2) {
            const long key = static_cast<long>(next(rng) % 600);
            const Bytes v = pattern(next(rng) % 90, next(rng));
            NOVA_CHECK(put(s, key, v));
            model[key] = v;
        } else if (what < 4) {
            const long key = static_cast<long>(next(rng) % 600);
            NOVA_CHECK(s.erase(key) == (model.erase(key) == 1));
        } else {
            const std::uint64_t n = what < 7 ? next(rng) % 5 : next(rng) % 120;
            std::unordered_map<long, Bytes> pending;
            Transaction t(s);
            for (std::uint64_t i = 0; i < n; ++i) {
                const long key = static_cast<long>(next(rng) % 600);   // repeats within a batch
                const std::size_t len = next(rng) % 50 == 0 ? 70000 : next(rng) % 200;
                const Bytes v = pattern(len, next(rng));
                stage(t, key, v);
                pending[key] = v;
            }
            staged_total += n;
            if (next(rng) % 6 == 0) continue;   // abandoned
            NOVA_CHECK(t.commit());
            ++commits;
            for (auto& kv : pending) model[kv.first] = kv.second;
        }
        if (r % 97 == 0) {
            for (long key = 0; key < 600; ++key) {
                const auto it = model.find(key);
                NOVA_CHECK(it == model.end() ? missing(s, key) : holds(s, key, it->second));
            }
        }
    }
    for (long key = 0; key < 600; ++key) {
        const auto it = model.find(key);
        NOVA_CHECK(it == model.end() ? missing(s, key) : holds(s, key, it->second));
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
    NOVA_CHECK(NovaKvInspector::stats(s).live_keys == model.size());
    std::printf("  randomized seed=%llu: %llu transactions committed, %llu writes staged, %zu keys live\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(commits),
                static_cast<unsigned long long>(staged_total), model.size());
}

}  // namespace

int main() {
    // Single-threaded test. For the rcu build the writer runs the reclamation
    // passes itself after each write, so that counters are deterministic. It is
    // the same epoch-based protocol. No effect on the lock-based builds.
    NovaKvInspector::set_default_rcu_options(0, 2);

    test_staging_is_invisible_until_commit();
    test_values_are_copied_at_staging();
    test_last_write_wins();
    test_commit_sizes();
    test_many_keys_one_partition();
    test_large_and_zero_length_values();
    test_empty_commit();
    test_commit_only_once();
    test_abandoned_transaction();
    test_invalid_put_fails_the_commit();
    test_generator_usage_pattern();
    test_transactions_on_independent_stores();
    test_randomized_against_reference_model(0xC0FFEEULL, 4000);
    test_randomized_against_reference_model(20261012ULL, 4000);
    return nova_test::finish("transaction_test");
}
