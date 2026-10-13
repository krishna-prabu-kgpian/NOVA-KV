// index_test.cpp -- Part C.2: the secondary index through the public API
// (create_index, index_get, snapshot-scoped index_get).
//
// Built for every variant. On the lock-based baselines the only thing to test
// is that the store says it has no index. On the rcu build every expected
// answer comes from an INDEPENDENT REFERENCE MODEL kept by the test (an ordered
// map from key to the value bytes last written), never from the store's own
// index: the model is filtered by brute force for each probe.
//
// Contract under test (src/kvstore.cpp, docs/secondary_index.md):
//   - one index per store over value bytes [offset, offset + len);
//   - a value shorter than offset + len is not indexed;
//   - index_get compares byte for byte, returns keys in ascending order, writes
//     at most max_keys (the smallest ones), and returns the number written;
//   - the index always describes the same version as the data it is read with.
#include "kvstore.h"

#include "hash.h"
#include "inspect.h"
#include "test_util.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
using Model = std::map<long, Bytes>;

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// A value of `len` bytes whose bytes [off, off + field.size()) are `field` (as
// far as they fit) and whose other bytes depend on `noise`.
Bytes row(std::size_t len, std::size_t off, const Bytes& field, std::uint64_t noise) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(noise * 37 + i * 11 + 5);
    for (std::size_t i = 0; i < field.size() && off + i < len; ++i) v[off + i] = field[i];
    return v;
}

Bytes field_of(const char* text, std::size_t len) {
    Bytes f(len, 0);
    for (std::size_t i = 0; i < len && text[i] != '\0'; ++i) f[i] = static_cast<unsigned char>(text[i]);
    return f;
}

bool put(KVStore& s, Model& m, long key, const Bytes& v) {
    m[key] = v;
    return s.put(key, v.data(), v.size());
}

// The reference answer: every key of the model whose value contains `field` at
// [off, off + field.size()), ascending.
std::vector<long> expected(const Model& m, std::size_t off, const Bytes& field) {
    std::vector<long> out;
    for (const auto& kv : m) {
        const Bytes& v = kv.second;
        if (v.size() < off + field.size()) continue;
        if (std::memcmp(v.data() + off, field.data(), field.size()) == 0) out.push_back(kv.first);
    }
    return out;
}

// Calls index_get (current version, or `snap`'s) with room for `capacity` keys
// behind which a guard region must stay untouched.
std::vector<long> lookup(const KVStore& s, const KVStore::Snapshot* snap, const Bytes& field, int capacity) {
    const long guard = LONG_MIN + 4242;
    std::vector<long> buf(static_cast<std::size_t>(capacity > 0 ? capacity : 0) + 4, guard);
    const int n = snap != nullptr
                      ? s.index_get(*snap, field.data(), field.size(), buf.data(), capacity)
                      : s.index_get(field.data(), field.size(), buf.data(), capacity);
    NOVA_CHECK(n >= 0 && n <= (capacity > 0 ? capacity : 0));
    for (std::size_t i = static_cast<std::size_t>(n); i < buf.size(); ++i) NOVA_CHECK(buf[i] == guard);
    return std::vector<long>(buf.begin(), buf.begin() + n);
}

// Every distinct field value present in the model, plus probes that match
// nothing, must give exactly the reference answer: complete, ascending, no
// duplicates; and a short buffer must get the smallest keys.
void check_against_model(const KVStore& s, const KVStore::Snapshot* snap, const Model& m, std::size_t off,
                         std::size_t len) {
    std::set<Bytes> fields;
    for (const auto& kv : m) {
        if (kv.second.size() >= off + len) fields.insert(Bytes(kv.second.begin() + static_cast<std::ptrdiff_t>(off),
                                                              kv.second.begin() + static_cast<std::ptrdiff_t>(off + len)));
    }
    std::uint64_t indexed_pairs = 0;
    for (const Bytes& f : fields) {
        const std::vector<long> want = expected(m, off, f);
        indexed_pairs += want.size();
        NOVA_CHECK(lookup(s, snap, f, static_cast<int>(want.size()) + 8) == want);
        NOVA_CHECK(lookup(s, snap, f, static_cast<int>(want.size())) == want);      // exactly enough room
        if (want.size() > 1) {                                                     // truncation: smallest keys
            const int k = static_cast<int>(want.size() / 2);
            NOVA_CHECK(lookup(s, snap, f, k) == std::vector<long>(want.begin(), want.begin() + k));
            NOVA_CHECK(lookup(s, snap, f, 1) == std::vector<long>(want.begin(), want.begin() + 1));
        }
    }
    for (std::uint64_t i = 0; i < 6; ++i) {                                        // values nobody has
        Bytes probe(len);
        for (std::size_t b = 0; b < len; ++b) probe[b] = static_cast<unsigned char>(0xF0 + i + b * 3);
        if (fields.count(probe) == 0) NOVA_CHECK(lookup(s, snap, probe, 16).empty());
    }
    if (snap == nullptr) NOVA_CHECK(NovaKvInspector::stats(s).index_pairs == indexed_pairs);
}

// The same agreement established WITHOUT the model, from the snapshot alone:
// enumerate its keys, fetch its rows, and check every field value found there.
void check_snapshot_agrees_with_itself(const KVStore& s, const KVStore::Snapshot& snap, std::size_t off,
                                       std::size_t len) {
    std::vector<long> keys(100000);
    const std::size_t n = s.keys(snap, keys.data(), keys.size());
    NOVA_CHECK(n < keys.size());
    Model seen;
    Bytes buf(4096);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t size = 0;
        while (size <= buf.size() && !s.get(snap, keys[i], buf.data(), size)) ++size;   // find the exact length
        NOVA_CHECK(size <= buf.size());
        seen[keys[i]] = Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(size));
    }
    check_against_model(s, &snap, seen, off, len);
}

// ---- Lock-based baselines: no index ---------------------------------------------------
void test_lock_variants_have_no_index() {
    KVStore s;
    const Bytes v = row(24, 0, field_of("SYM001", 8), 1);
    NOVA_CHECK(s.put(1, v.data(), v.size()));
    NOVA_CHECK(!s.create_index(0, 8));
    long out[4] = {7, 7, 7, 7};
    NOVA_CHECK(s.index_get(v.data(), 8, out, 4) == 0 && out[0] == 7);
}

// ---- Creation -----------------------------------------------------------------------------
void test_no_index_yet() {
    KVStore s;
    Model m;
    NOVA_CHECK(put(s, m, 1, row(24, 0, field_of("SYM001", 8), 1)));
    long out[4] = {7, 7, 7, 7};
    NOVA_CHECK(s.index_get("SYM001\0", 8, out, 4) == 0 && out[0] == 7);
    KVStore::Snapshot snap = s.acquire_snapshot();
    NOVA_CHECK(s.index_get(snap, "SYM001\0", 8, out, 4) == 0 && out[0] == 7);
    NOVA_CHECK(!NovaKvInspector::stats(s).indexed);
}

// The way the supplied generator does it: create first, insert afterwards.
void test_create_on_empty_store_then_insert() {
    KVStore s;
    Model m;
    const std::uint64_t pub0 = NovaKvInspector::stats(s).versions_published;
    NOVA_CHECK(s.create_index(0, 8));
    NOVA_CHECK(NovaKvInspector::stats(s).indexed);
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == pub0 + 1);  // one publication
    for (long k = 0; k < 50; ++k) {
        char sym[16];
        std::snprintf(sym, sizeof(sym), "SYM%03ld", k);
        NOVA_CHECK(put(s, m, k, row(20, 0, field_of(sym, 8), static_cast<std::uint64_t>(k))));
    }
    check_against_model(s, nullptr, m, 0, 8);
    NOVA_CHECK(lookup(s, nullptr, field_of("SYM007", 8), 8) == std::vector<long>{7});
    check_valid(s);
}

// Backfill: the store already holds values, with duplicates, short values and
// empty values, when the index is created.
void test_create_on_populated_store() {
    KVStore s;
    Model m;
    const std::size_t off = 4, len = 6;
    for (long k = 0; k < 600; ++k) {
        char name[16];
        std::snprintf(name, sizeof(name), "F%03ld", k % 40);                // 40 values, 15 keys each
        std::size_t size = 32;
        if (k % 13 == 0) size = off + len - 1;                              // one byte too short: not indexed
        if (k % 17 == 0) size = 0;                                          // empty value: not indexed
        if (k % 19 == 0) size = off + len;                                  // exactly long enough: indexed
        NOVA_CHECK(put(s, m, k, row(size, off, field_of(name, len), static_cast<std::uint64_t>(k))));
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    const NovaKvStats before = NovaKvInspector::stats(s);
    NOVA_CHECK(s.create_index(off, len));
    const NovaKvStats after = NovaKvInspector::stats(s);
    NOVA_CHECK(after.versions_published == before.versions_published + 1);  // the index appears in ONE publication
    NOVA_CHECK(after.segments_built == before.segments_built);              // no primary segment was copied
    NOVA_CHECK(after.live_keys == before.live_keys);
    check_against_model(s, nullptr, m, off, len);
    NOVA_CHECK(expected(m, off, field_of("F005", len)).size() >= 10);       // duplicates really exist
    for (long k = 0; k < 600; ++k) {                                        // the data itself is untouched
        Bytes buf(64);
        NOVA_CHECK(s.get(k, buf.data(), m[k].size()));
    }
    check_valid(s);
}

void test_invalid_arguments() {
    KVStore s;
    Model m;
    NOVA_CHECK(put(s, m, 1, row(32, 0, field_of("AAAA", 4), 1)));
    const std::uint64_t pub0 = NovaKvInspector::stats(s).versions_published;
    NOVA_CHECK(!s.create_index(0, 0));                                       // empty field
    NOVA_CHECK(!s.create_index(0, (std::size_t{1} << 30) + 1));              // longer than any value
    NOVA_CHECK(!s.create_index(std::size_t{1} << 30, 1));                    // starts at the size limit
    NOVA_CHECK(!s.create_index(static_cast<std::size_t>(-1), 8));            // offset + len overflows
    NOVA_CHECK(!s.create_index(8, static_cast<std::size_t>(-1)));
    NOVA_CHECK(!NovaKvInspector::stats(s).indexed);
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == pub0);        // nothing was published

    NOVA_CHECK(s.create_index(0, 4));
    const std::uint64_t pub1 = NovaKvInspector::stats(s).versions_published;
    NOVA_CHECK(s.create_index(0, 4));                                        // the same index again: fine, no-op
    NOVA_CHECK(!s.create_index(0, 5));                                       // a different one: refused
    NOVA_CHECK(!s.create_index(1, 4));
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == pub1);
    check_against_model(s, nullptr, m, 0, 4);

    // index_get arguments.
    const Bytes f = field_of("AAAA", 4);
    long out[4] = {7, 7, 7, 7};
    NOVA_CHECK(s.index_get(f.data(), 4, out, 4) == 1 && out[0] == 1 && out[1] == 7);
    out[0] = 7;
    NOVA_CHECK(s.index_get(nullptr, 4, out, 4) == 0);
    NOVA_CHECK(s.index_get(f.data(), 4, nullptr, 4) == 0);
    NOVA_CHECK(s.index_get(f.data(), 4, out, 0) == 0);
    NOVA_CHECK(s.index_get(f.data(), 4, out, -3) == 0);
    NOVA_CHECK(s.index_get(f.data(), 3, out, 4) == 0);                       // not the index's field length
    NOVA_CHECK(s.index_get(f.data(), 0, out, 4) == 0);
    const Bytes longer = field_of("AAAAA", 5);
    NOVA_CHECK(s.index_get(longer.data(), 5, out, 4) == 0);
    NOVA_CHECK(out[0] == 7 && out[1] == 7);                                  // and the buffer was never written
}

// ---- What is compared: exactly the bytes of the range ----------------------------------------
void test_field_bytes_and_boundaries() {
    KVStore s;
    Model m;
    const std::size_t off = 3, len = 5;
    NOVA_CHECK(s.create_index(off, len));
    const Bytes zeros(len, 0);
    const Bytes z1 = {0, 0, 1, 0, 0};          // embedded zero bytes; differs from `zeros` in the middle
    const Bytes z2 = {0, 0, 0, 0, 1};          // differs only in the last byte
    const Bytes z3 = {1, 0, 0, 0, 0};          // differs only in the first byte
    const Bytes ff(len, 0xFF);
    long k = 0;
    for (const Bytes* f : {&zeros, &z1, &z2, &z3, &ff, &zeros, &z1}) {
        // Different surroundings every time: bytes outside the range must not matter.
        NOVA_CHECK(put(s, m, k, row(off + len + static_cast<std::size_t>(k) % 5, off, *f, static_cast<std::uint64_t>(k) + 9)));
        ++k;
    }
    NOVA_CHECK(put(s, m, 100, row(off + len, off, ff, 1)));        // exactly long enough: indexed
    NOVA_CHECK(put(s, m, 101, row(off + len - 1, off, ff, 2)));    // one byte short: not indexed
    NOVA_CHECK(put(s, m, 102, row(off, off, ff, 3)));              // ends where the field would begin
    NOVA_CHECK(put(s, m, 103, Bytes()));                           // empty
    NOVA_CHECK(put(s, m, 104, row(1, off, ff, 4)));
    check_against_model(s, nullptr, m, off, len);
    NOVA_CHECK(lookup(s, nullptr, zeros, 8) == (std::vector<long>{0, 5}));
    NOVA_CHECK(lookup(s, nullptr, z1, 8) == (std::vector<long>{1, 6}));
    NOVA_CHECK(lookup(s, nullptr, z2, 8) == (std::vector<long>{2}));
    NOVA_CHECK(lookup(s, nullptr, z3, 8) == (std::vector<long>{3}));
    NOVA_CHECK(lookup(s, nullptr, ff, 8) == (std::vector<long>{4, 100}));
    check_valid(s);
}

// ---- Maintenance: overwrite, delete, reuse ------------------------------------------------------
void test_overwrite_delete_and_key_reuse() {
    KVStore s;
    Model m;
    const std::size_t off = 2, len = 4;
    NOVA_CHECK(s.create_index(off, len));
    const Bytes a = field_of("AAAA", len), b = field_of("BBBB", len), c = field_of("CCCC", len);
    for (long k = 0; k < 30; ++k) NOVA_CHECK(put(s, m, k, row(16, off, k % 2 == 0 ? a : b, static_cast<std::uint64_t>(k))));
    check_against_model(s, nullptr, m, off, len);

    // Overwrite that moves the key to another field value.
    NOVA_CHECK(put(s, m, 4, row(16, off, b, 100)));
    NOVA_CHECK(put(s, m, 5, row(40, off, c, 101)));
    check_against_model(s, nullptr, m, off, len);

    // Overwrite that leaves the indexed bytes alone: the index is not touched.
    const std::uint64_t built = NovaKvInspector::stats(s).index_segments_built;
    NOVA_CHECK(put(s, m, 6, row(16, off, a, 999)));
    NOVA_CHECK(put(s, m, 6, row(300, off, a, 998)));
    NOVA_CHECK(NovaKvInspector::stats(s).index_segments_built == built);
    check_against_model(s, nullptr, m, off, len);

    // Long -> too short (leaves the index) -> long again (comes back).
    NOVA_CHECK(put(s, m, 8, row(off + len - 1, off, a, 1)));
    check_against_model(s, nullptr, m, off, len);
    NOVA_CHECK(put(s, m, 8, Bytes()));
    NOVA_CHECK(put(s, m, 8, row(off + len, off, c, 2)));
    check_against_model(s, nullptr, m, off, len);

    // Delete: the key must disappear from its posting list at once.
    for (long k = 0; k < 30; k += 3) {
        NOVA_CHECK(s.erase(k));
        m.erase(k);
    }
    NOVA_CHECK(!s.erase(0));
    check_against_model(s, nullptr, m, off, len);

    // Reuse of deleted keys, with a different field value.
    for (long k = 0; k < 30; k += 3) NOVA_CHECK(put(s, m, k, row(16, off, c, static_cast<std::uint64_t>(k) + 50)));
    check_against_model(s, nullptr, m, off, len);

    // Remove everything: the index must become empty, not stale.
    for (long k = 0; k < 30; ++k) {
        NOVA_CHECK(s.erase(k));
        m.erase(k);
    }
    check_against_model(s, nullptr, m, off, len);
    NOVA_CHECK(lookup(s, nullptr, a, 8).empty() && lookup(s, nullptr, b, 8).empty() && lookup(s, nullptr, c, 8).empty());
    NOVA_CHECK(NovaKvInspector::stats(s).index_pairs == 0);
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// ---- Transactions --------------------------------------------------------------------------------
void test_transactions_maintain_the_index() {
    KVStore s;
    Model m;
    const std::size_t off = 0, len = 3;
    NOVA_CHECK(s.create_index(off, len));
    const Bytes a = field_of("aaa", len), b = field_of("bbb", len), c = field_of("ccc", len);
    for (long k = 0; k < 40; ++k) NOVA_CHECK(put(s, m, k, row(12, off, a, static_cast<std::uint64_t>(k))));

    // One transaction: repeated writes to one key (a -> b -> c -> b), keys in
    // many partitions, inserts, a value too short to index, an unchanged field.
    const std::uint64_t pub0 = NovaKvInspector::stats(s).versions_published;
    {
        Transaction t(s);
        auto stage = [&](long key, const Bytes& v) {
            t.put(key, v.data(), v.size());
            m[key] = v;
        };
        stage(3, row(12, off, b, 1));
        stage(3, row(12, off, c, 2));
        stage(3, row(12, off, b, 3));                   // the last one decides: 3 is under "bbb"
        for (long k = 10; k < 30; ++k) stage(k, row(12, off, k % 2 == 0 ? b : c, static_cast<std::uint64_t>(k)));
        for (long k = 100; k < 130; ++k) stage(k, row(12, off, c, static_cast<std::uint64_t>(k)));
        stage(5, row(2, off, a, 4));                    // too short now: leaves the index
        stage(6, row(50, off, a, 5));                   // same field, other bytes
        stage(200, row(12, off, a, 6));
        stage(200, row(1, off, a, 7));                  // inserted and made unindexable in the same commit
        NOVA_CHECK(t.commit());
    }
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == pub0 + 1);   // data and index: ONE publication
    check_against_model(s, nullptr, m, off, len);
    NOVA_CHECK(lookup(s, nullptr, b, 64).front() == 3);

    // An abandoned transaction and a failed one change neither data nor index.
    const NovaKvStats before = NovaKvInspector::stats(s);
    {
        Transaction t(s);
        const Bytes v = row(12, off, b, 9);
        for (long k = 0; k < 40; ++k) t.put(k, v.data(), v.size());
    }
    {
        Transaction t(s);
        const Bytes v = row(12, off, b, 9);
        for (long k = 0; k < 40; ++k) t.put(k, v.data(), v.size());
        t.put(41, nullptr, 5);
        NOVA_CHECK(!t.commit());
    }
    NOVA_CHECK(NovaKvInspector::stats(s).versions_published == before.versions_published);
    NOVA_CHECK(NovaKvInspector::stats(s).index_segments_built == before.index_segments_built);
    check_against_model(s, nullptr, m, off, len);

    // Several transactions in a row.
    for (int round = 0; round < 20; ++round) {
        Transaction t(s);
        for (long k = 0; k < 16; ++k) {
            const long key = (round * 7 + k * 3) % 60;
            const Bytes v = row(12, off, (round + k) % 3 == 0 ? a : ((round + k) % 3 == 1 ? b : c),
                                static_cast<std::uint64_t>(round * 100 + k));
            t.put(key, v.data(), v.size());
            m[key] = v;
        }
        NOVA_CHECK(t.commit());
        check_against_model(s, nullptr, m, off, len);
    }
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// A commit that turns out to change nothing (a batch of erases of keys that do
// not exist) on an INDEXED store: nothing is published, the index is untouched,
// and the commit's private index bookkeeping is released (the leak checker of
// the ASan build is what detects it if it is not).
void test_commit_without_change_on_indexed_store() {
    KVStore s;
    Model m;
    NOVA_CHECK(s.create_index(0, 4));
    for (long k = 0; k < 20; ++k) NOVA_CHECK(put(s, m, k, row(8, 0, field_of("zzzz", 4), static_cast<std::uint64_t>(k))));
    const NovaKvStats before = NovaKvInspector::stats(s);
    for (int round = 0; round < 50; ++round) {
        NovaBatchOp ops[3] = {{false, 1000 + round, nullptr, 0}, {false, 2000, nullptr, 0}, {false, -5, nullptr, 0}};
        NOVA_CHECK(NovaKvInspector::commit_batch(s, ops, 3));     // succeeds, changes nothing
    }
    const NovaKvStats after = NovaKvInspector::stats(s);
    NOVA_CHECK(after.versions_published == before.versions_published);
    NOVA_CHECK(after.index_segments_built == before.index_segments_built);
    check_against_model(s, nullptr, m, 0, 4);
    check_valid(s);
}

// ---- Many keys per value (truncation) and many values (growth) -------------------------------------
void test_truncation_is_deterministic() {
    KVStore s;
    Model m;
    NOVA_CHECK(s.create_index(0, 8));
    const Bytes f = field_of("SAME", 8);
    for (long k = 999; k >= 0; k -= 2) NOVA_CHECK(put(s, m, k * 13 - 5000, row(16, 0, f, static_cast<std::uint64_t>(k))));  // 500 keys, inserted descending
    const std::vector<long> want = expected(m, 0, f);
    NOVA_CHECK(want.size() == 500);
    for (int cap : {1, 2, 10, 499, 500, 501, 4000}) {
        const std::vector<long> got = lookup(s, nullptr, f, cap);
        const std::size_t n = static_cast<std::size_t>(cap) < want.size() ? static_cast<std::size_t>(cap) : want.size();
        NOVA_CHECK(got == std::vector<long>(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(n)));  // the smallest keys, ascending
    }
    // Negative keys sort before positive ones.
    NOVA_CHECK(lookup(s, nullptr, f, 1).front() == want.front() && want.front() < 0);
    check_valid(s);
}

void test_many_distinct_values() {
    KVStore s;
    Model m;
    NOVA_CHECK(s.create_index(0, 8));
    for (long k = 0; k < 6000; ++k) {
        Bytes f(8);
        const std::uint64_t x = static_cast<std::uint64_t>(k) * 2654435761u;
        std::memcpy(f.data(), &x, 8);
        NOVA_CHECK(put(s, m, k, row(8, 0, f, 0)));
    }
    check_against_model(s, nullptr, m, 0, 8);
    for (long k = 0; k < 6000; k += 2) {            // shrink again
        NOVA_CHECK(s.erase(k));
        m.erase(k);
    }
    check_against_model(s, nullptr, m, 0, 8);
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
}

// ---- Snapshots: the index of the pinned version ------------------------------------------------------
void test_snapshot_uses_its_own_index() {
    KVStore a_store, other;
    Model m;
    const std::size_t off = 1, len = 4;
    const Bytes x = field_of("XXXX", len), y = field_of("YYYY", len);
    for (long k = 0; k < 20; ++k) NOVA_CHECK(put(a_store, m, k, row(10, off, x, static_cast<std::uint64_t>(k))));

    KVStore::Snapshot before_index = a_store.acquire_snapshot();   // a version that has no index
    NOVA_CHECK(a_store.create_index(off, len));
    KVStore::Snapshot at_creation = a_store.acquire_snapshot();
    const Model m_at_creation = m;

    // The store moves on: every key changes its field, some are deleted.
    for (long k = 0; k < 20; ++k) NOVA_CHECK(put(a_store, m, k, row(10, off, y, static_cast<std::uint64_t>(k) + 50)));
    for (long k = 0; k < 20; k += 4) {
        NOVA_CHECK(a_store.erase(k));
        m.erase(k);
    }
    KVStore::Snapshot later = a_store.acquire_snapshot();
    const Model m_later = m;
    for (long k = 20; k < 40; ++k) NOVA_CHECK(put(a_store, m, k, row(10, off, x, static_cast<std::uint64_t>(k))));

    // Each handle answers from its own version, primary data and index alike.
    NOVA_CHECK(lookup(a_store, &before_index, x, 64).empty());      // that version has no index, and never will
    NOVA_CHECK(lookup(a_store, &before_index, y, 64).empty());
    check_against_model(a_store, &at_creation, m_at_creation, off, len);
    check_against_model(a_store, &later, m_later, off, len);
    check_against_model(a_store, nullptr, m, off, len);
    NOVA_CHECK(lookup(a_store, &at_creation, x, 64).size() == 20 && lookup(a_store, &at_creation, y, 64).empty());
    NOVA_CHECK(lookup(a_store, &later, x, 64).empty() && lookup(a_store, &later, y, 64).size() == 15);
    NOVA_CHECK(lookup(a_store, nullptr, x, 64).size() == 20);
    check_snapshot_agrees_with_itself(a_store, at_creation, off, len);
    check_snapshot_agrees_with_itself(a_store, later, off, len);

    // A handle of another store, and a moved-from handle, are refused.
    NOVA_CHECK(other.create_index(off, len));
    Model om;
    NOVA_CHECK(put(other, om, 1, row(10, off, x, 1)));
    long out[4] = {7, 7, 7, 7};
    NOVA_CHECK(other.index_get(at_creation, x.data(), len, out, 4) == 0 && out[0] == 7);
    KVStore::Snapshot moved = std::move(at_creation);
    NOVA_CHECK(a_store.index_get(at_creation, x.data(), len, out, 4) == 0 && out[0] == 7);
    NOVA_CHECK(lookup(a_store, &moved, x, 64).size() == 20);
}

// ---- Randomized against the model, with snapshots kept across many index updates ---------------------
std::uint64_t next(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

void test_randomized_against_reference_model(std::uint64_t seed, int rounds, bool create_first) {
    KVStore s;
    Model m;
    const std::size_t off = 5, len = 3;
    std::uint64_t rng = seed;
    struct Held {
        KVStore::Snapshot snap;
        Model model;
        bool indexed;
    };
    std::vector<Held> held;
    bool indexed = false;
    if (create_first) {
        NOVA_CHECK(s.create_index(off, len));
        indexed = true;
    }
    auto random_row = [&]() {
        // 12 field values (so plenty of duplicates), and values of every length
        // around the boundary of the indexed range.
        const std::uint64_t pick = next(rng) % 12;
        Bytes f = {static_cast<unsigned char>('a' + pick % 4), static_cast<unsigned char>(pick % 3),
                   static_cast<unsigned char>(pick / 6)};
        const std::size_t sizes[] = {0, 1, off, off + len - 1, off + len, off + len + 1, 16, 40, 200};
        return row(sizes[next(rng) % 9], off, f, next(rng));
    };
    std::uint64_t updates = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::uint64_t what = next(rng) % 10;
        if (what < 4) {
            NOVA_CHECK(put(s, m, static_cast<long>(next(rng) % 400), random_row()));
            ++updates;
        } else if (what < 6) {
            const long key = static_cast<long>(next(rng) % 400);
            NOVA_CHECK(s.erase(key) == (m.erase(key) == 1));
        } else {
            Transaction t(s);
            Model pending;
            const std::uint64_t n = next(rng) % 40;
            for (std::uint64_t i = 0; i < n; ++i) {
                const long key = static_cast<long>(next(rng) % 400);
                const Bytes v = random_row();
                t.put(key, v.data(), v.size());
                pending[key] = v;
            }
            if (next(rng) % 5 != 0) {
                NOVA_CHECK(t.commit());
                for (auto& kv : pending) m[kv.first] = kv.second;
                updates += n;
            }
        }
        if (!indexed && r == rounds / 3) {              // backfill in the middle of the history
            NOVA_CHECK(s.create_index(off, len));
            indexed = true;
        }
        if (r % 211 == 0) held.push_back(Held{s.acquire_snapshot(), m, indexed});
        if (r % 97 == 0 && indexed) check_against_model(s, nullptr, m, off, len);
        if (held.size() > 12) held.erase(held.begin());  // old snapshots leave; their versions become reclaimable
    }
    check_against_model(s, nullptr, m, off, len);
    for (const Held& h : held) {
        if (h.indexed) {
            check_against_model(s, &h.snap, h.model, off, len);
            check_snapshot_agrees_with_itself(s, h.snap, off, len);
        } else {
            NOVA_CHECK(lookup(s, &h.snap, Bytes{'a', 0, 0}, 8).empty());
        }
    }
    check_valid(s);                                       // with the snapshots' versions still retained
    held.clear();
    NOVA_CHECK(NovaKvInspector::settle(s));
    check_valid(s);
    const NovaKvStats st = NovaKvInspector::stats(s);
    NOVA_CHECK(st.index_segments_built > 100);
    std::printf("  randomized seed=%llu create_first=%d: %llu values written, %llu index segments built, %llu released, "
                "%llu pairs at the end\n",
                static_cast<unsigned long long>(seed), create_first ? 1 : 0, static_cast<unsigned long long>(updates),
                static_cast<unsigned long long>(st.index_segments_built),
                static_cast<unsigned long long>(st.index_segments_released),
                static_cast<unsigned long long>(st.index_pairs));
}

}  // namespace

int main() {
    if (!NovaKvInspector::is_versioned()) {
        test_lock_variants_have_no_index();
        return nova_test::finish("index_test");
    }
    // Single-threaded: the writer runs the reclamation passes itself after each
    // write (the same epoch-based protocol), so counters are exact.
    NovaKvInspector::set_default_rcu_options(0, 2);

    test_no_index_yet();
    test_create_on_empty_store_then_insert();
    test_create_on_populated_store();
    test_invalid_arguments();
    test_field_bytes_and_boundaries();
    test_overwrite_delete_and_key_reuse();
    test_transactions_maintain_the_index();
    test_commit_without_change_on_indexed_store();
    test_truncation_is_deterministic();
    test_many_distinct_values();
    test_snapshot_uses_its_own_index();
    test_randomized_against_reference_model(0xABCDEF01ULL, 4000, true);
    test_randomized_against_reference_model(20261013ULL, 4000, false);
    test_randomized_against_reference_model(7, 4000, false);
    return nova_test::finish("index_test");
}
