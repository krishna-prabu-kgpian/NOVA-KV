// index_failure_test.cpp -- Part C.2: the secondary index under allocation
// failure and under forced hash collisions (rcu hook build, public API).
//
// 1. create_index on a populated store, and commits to an indexed store (put,
//    erase, transactions), with the n-th allocation failing for n = 0, 1, 2, ...
//    until a run completes untouched. After EVERY injected failure:
//      - the operation reported failure and published nothing;
//      - primary data and index are exactly what they were (reference model);
//      - a snapshot taken beforehand still answers from its own version;
//      - the store's own invariants hold (in every retained version the index
//        equals what the primary data implies; owner counts; bytes);
//      - the same operation then succeeds, reclamation completes, and no
//        version, segment, chunk or index segment is left over.
// 2. The index hash is cut down to a few bits (test hook), so that many
//    different field values share one hash and one bucket: every answer must
//    still be exact, because candidates are checked byte for byte.
#include "kvstore.h"

#include "alloc.h"
#include "inspect.h"
#include "secondary_index.h"
#include "test_points.h"
#include "test_util.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "index_failure_test must be built with NOVA_KV_TEST_HOOKS"
#endif

namespace {

using Bytes = std::vector<unsigned char>;
using Model = std::map<long, Bytes>;

const std::size_t kOff = 4, kLen = 6;

Bytes field(std::uint64_t id) {
    Bytes f(kLen);
    for (std::size_t i = 0; i < kLen; ++i) f[i] = static_cast<unsigned char>((id >> (i * 4)) * 7 + i);
    f[1] = 0;  // an embedded zero byte in every field value
    return f;
}

Bytes row(std::size_t len, std::uint64_t field_id, std::uint64_t noise) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(noise * 37 + i * 11 + 5);
    const Bytes f = field(field_id);
    for (std::size_t i = 0; i < kLen && kOff + i < len; ++i) v[kOff + i] = f[i];
    return v;
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

std::vector<long> expected(const Model& m, const Bytes& f) {
    std::vector<long> out;
    for (const auto& kv : m) {
        if (kv.second.size() >= kOff + kLen && std::memcmp(kv.second.data() + kOff, f.data(), kLen) == 0) out.push_back(kv.first);
    }
    return out;
}

// True if the store (or the snapshot) answers every probe exactly as the model
// does, for every field value in `universe`, and holds exactly the model's data.
bool agrees(const KVStore& s, const KVStore::Snapshot* snap, const Model& m, std::uint64_t universe,
            bool index_expected) {
    static Bytes buf(4096);
    std::vector<long> out(m.size() + 8);
    for (std::uint64_t id = 0; id < universe; ++id) {
        const Bytes f = field(id);
        const std::vector<long> want = index_expected ? expected(m, f) : std::vector<long>();
        const int n = snap != nullptr ? s.index_get(*snap, f.data(), kLen, out.data(), static_cast<int>(out.size()))
                                      : s.index_get(f.data(), kLen, out.data(), static_cast<int>(out.size()));
        if (n != static_cast<int>(want.size())) return false;
        for (int i = 0; i < n; ++i) {
            if (out[static_cast<std::size_t>(i)] != want[static_cast<std::size_t>(i)]) return false;
        }
    }
    for (const auto& kv : m) {
        const Bytes& v = kv.second;
        const bool found = snap != nullptr ? s.get(*snap, kv.first, buf.data(), v.size())
                                           : s.get(kv.first, buf.data(), v.size());
        if (!found || (!v.empty() && std::memcmp(buf.data(), v.data(), v.size()) != 0)) return false;
    }
    std::vector<long> keys(m.size() + 8);
    if (snap != nullptr && s.keys(*snap, keys.data(), keys.size()) != m.size()) return false;
    if (snap == nullptr && NovaKvInspector::stats(s).live_keys != m.size()) return false;
    return true;
}

Model preload_model() {
    Model m;
    for (long k = 0; k < 300; ++k) {
        std::size_t size = 24;
        if (k % 11 == 0) size = kOff + kLen - 1;   // too short to index
        if (k % 13 == 0) size = 0;
        m[k] = row(size, static_cast<std::uint64_t>(k % 20), static_cast<std::uint64_t>(k));
    }
    return m;
}

void load(KVStore& s, const Model& m) {
    for (const auto& kv : m) NOVA_CHECK(s.put(kv.first, kv.second.data(), kv.second.size()));
}

void expect_no_leftovers() {
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::IndexSegment) == 0);
}

// ---- 1a. create_index with every allocation failing in turn ---------------------------------
void test_create_index_under_allocation_failure() {
    const Model m = preload_model();
    long failures = 0;
    for (long n = 0;; ++n) {
        KVStore s;
        load(s, m);
        NovaKvInspector::settle(s);
        KVStore::Snapshot before = s.acquire_snapshot();
        const std::uint64_t published0 = NovaKvInspector::stats(s).versions_published;

        nova::test_hooks::fail_nth_allocation(n);
        const bool ok = s.create_index(kOff, kLen);
        const bool fired = nova::test_hooks::allocation_failure_fired();
        nova::test_hooks::fail_nth_allocation(-1);
        if (!fired) {
            NOVA_CHECK(ok);
            NOVA_CHECK(agrees(s, nullptr, m, 24, true));
            check_valid(s);
            break;
        }
        ++failures;
        NOVA_CHECK(!ok);                                                    // no failure of the build is tolerable
        const NovaKvStats st = NovaKvInspector::stats(s);
        NOVA_CHECK(!st.indexed && st.index_pairs == 0);                     // no index, not half of one
        NOVA_CHECK(st.versions_published == published0);
        NOVA_CHECK(agrees(s, nullptr, m, 24, false));                       // data intact, lookups find nothing
        check_valid(s);
        // The store is fully usable: writes work, and the index can be created.
        Model m2 = m;
        m2[5000] = row(24, 3, 1);
        NOVA_CHECK(s.put(5000, m2[5000].data(), m2[5000].size()));
        NOVA_CHECK(s.create_index(kOff, kLen));
        NOVA_CHECK(agrees(s, nullptr, m2, 24, true));
        NOVA_CHECK(agrees(s, &before, m, 24, false));                       // the old snapshot: old data, no index
        check_valid(s);
        { KVStore::Snapshot gone = std::move(before); }
        NOVA_CHECK(NovaKvInspector::settle(s));
        check_valid(s);
    }
    NOVA_CHECK(failures >= 4);
    expect_no_leftovers();
    std::printf("  create_index on 300 values: %ld allocation points failed in turn, all refused with no index visible\n", failures);
}

// ---- 1b. Commits to an indexed store with every allocation failing in turn ---------------------
// `apply` performs one operation on the store and updates the model the same
// way; it returns the operation's result.
void run_indexed_commit_scenario(const char* name, const std::function<bool(KVStore&, Model&)>& apply) {
    const Model m0 = preload_model();
    long failures = 0, rolled_back = 0, tolerated = 0;
    for (long n = 0;; ++n) {
        KVStore s;
        load(s, m0);
        NOVA_CHECK(s.create_index(kOff, kLen));
        NovaKvInspector::settle(s);
        KVStore::Snapshot before = s.acquire_snapshot();
        const NovaKvStats st0 = NovaKvInspector::stats(s);

        Model applied = m0;
        nova::test_hooks::fail_nth_allocation(n);
        const bool ok = apply(s, applied);
        const bool fired = nova::test_hooks::allocation_failure_fired();
        nova::test_hooks::fail_nth_allocation(-1);
        if (!fired) {
            NOVA_CHECK(ok);
            NOVA_CHECK(agrees(s, nullptr, applied, 24, true));
            check_valid(s);
            break;
        }
        ++failures;
        const NovaKvStats st1 = NovaKvInspector::stats(s);
        if (ok) {
            ++tolerated;
            NOVA_CHECK(agrees(s, nullptr, applied, 24, true));
        } else {
            ++rolled_back;
            NOVA_CHECK(agrees(s, nullptr, m0, 24, true));                   // data AND index as before
            NOVA_CHECK(st1.versions_published == st0.versions_published);
            NOVA_CHECK(st1.index_pairs == st0.index_pairs);
            NOVA_CHECK(st1.index_segments_built == st0.index_segments_built);  // nothing of the attempt was published
        }
        check_valid(s);                                                     // index == primary in every version; counts
        NOVA_CHECK(agrees(s, &before, m0, 24, true));                       // the earlier snapshot is untouched
        // Cleaner activity right after the failure, then the same operation again.
        NovaKvInspector::settle(s);
        Model again = m0;
        if (ok) again = applied;
        NOVA_CHECK(apply(s, again));
        NOVA_CHECK(agrees(s, nullptr, again, 24, true));
        NOVA_CHECK(agrees(s, &before, m0, 24, true));
        { KVStore::Snapshot gone = std::move(before); }
        NOVA_CHECK(NovaKvInspector::settle(s));
        check_valid(s);
    }
    NOVA_CHECK(failures > 0 && rolled_back > 0);
    expect_no_leftovers();
    std::printf("  %-52s %3ld allocation points failed in turn; %3ld rolled back, %ld tolerated\n", name, failures,
                rolled_back, tolerated);
}

bool put(KVStore& s, Model& m, long key, const Bytes& v) {
    if (!s.put(key, v.data(), v.size())) return false;
    m[key] = v;
    return true;
}

void test_indexed_commits_under_allocation_failure() {
    run_indexed_commit_scenario("put: field value changes", [](KVStore& s, Model& m) {
        return put(s, m, 7, row(24, 19, 777));
    });
    run_indexed_commit_scenario("put: new key", [](KVStore& s, Model& m) { return put(s, m, 9001, row(24, 2, 1)); });
    run_indexed_commit_scenario("put: becomes too short to index", [](KVStore& s, Model& m) {
        return put(s, m, 8, row(kOff + kLen - 1, 0, 3));
    });
    run_indexed_commit_scenario("erase of an indexed key", [](KVStore& s, Model& m) {
        if (m.count(9) == 0) return true;      // (second application: already gone)
        if (!s.erase(9)) return false;
        m.erase(9);
        return true;
    });
    run_indexed_commit_scenario("transaction: 40 writes, repeats, many partitions", [](KVStore& s, Model& m) {
        Transaction t(s);
        Model pending;
        for (long i = 0; i < 40; ++i) {
            const long key = (i * 7) % 30 + (i % 3 == 0 ? 1000 : 0);     // repeats and new keys
            const Bytes v = row(i % 9 == 0 ? 3 : 24, static_cast<std::uint64_t>((i * 5) % 20), static_cast<std::uint64_t>(i) + 400);
            t.put(key, v.data(), v.size());
            pending[key] = v;
        }
        if (!t.commit()) return false;
        for (auto& kv : pending) m[kv.first] = kv.second;
        return true;
    });
    run_indexed_commit_scenario("transaction: 120 keys onto one field value", [](KVStore& s, Model& m) {
        Transaction t(s);
        Model pending;
        for (long key = 0; key < 120; ++key) {
            const Bytes v = row(24, 4, static_cast<std::uint64_t>(key) + 900);
            t.put(key, v.data(), v.size());
            pending[key] = v;
        }
        if (!t.commit()) return false;
        for (auto& kv : pending) m[kv.first] = kv.second;
        return true;
    });
}

// ---- 2. Forced hash collisions --------------------------------------------------------------------
std::uint64_t next(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

void test_forced_hash_collisions(unsigned bits) {
    nova::test_hooks::set_index_hash_bits(bits);
    {
        KVStore s;
        Model m;
        // Backfill under collisions, then maintenance under collisions.
        for (long k = 0; k < 200; ++k) NOVA_CHECK(put(s, m, k, row(24, static_cast<std::uint64_t>(k % 24), static_cast<std::uint64_t>(k))));
        NOVA_CHECK(s.create_index(kOff, kLen));
        NOVA_CHECK(agrees(s, nullptr, m, 24, true));
        std::uint64_t rng = 99 + bits;
        std::vector<std::pair<KVStore::Snapshot, Model>> held;
        for (int r = 0; r < 1500; ++r) {
            const std::uint64_t what = next(rng) % 10;
            const long key = static_cast<long>(next(rng) % 260);
            if (what < 5) {
                // A key moving between two field values that HASH THE SAME is the
                // case where a removal and an addition name one index pair.
                NOVA_CHECK(put(s, m, key, row(next(rng) % 7 == 0 ? 2 : 24, next(rng) % 24, next(rng))));
            } else if (what < 7) {
                NOVA_CHECK(s.erase(key) == (m.erase(key) == 1));
            } else {
                Transaction t(s);
                Model pending;
                for (int i = 0; i < 12; ++i) {
                    const long k2 = static_cast<long>(next(rng) % 260);
                    const Bytes v = row(24, next(rng) % 24, next(rng));
                    t.put(k2, v.data(), v.size());
                    pending[k2] = v;
                }
                NOVA_CHECK(t.commit());
                for (auto& kv : pending) m[kv.first] = kv.second;
            }
            if (r % 50 == 0) NOVA_CHECK(agrees(s, nullptr, m, 24, true));
            if (r % 300 == 0) held.emplace_back(s.acquire_snapshot(), m);
        }
        NOVA_CHECK(agrees(s, nullptr, m, 24, true));
        for (auto& h : held) NOVA_CHECK(agrees(s, &h.first, h.second, 24, true));
        check_valid(s);
        held.clear();
        NOVA_CHECK(NovaKvInspector::settle(s));
        check_valid(s);
        std::printf("  index hash cut to %2u bits: 24 field values share at most %llu hashes; every lookup exact\n", bits,
                    1ULL << bits);
    }
    nova::test_hooks::set_index_hash_bits(64);
}

}  // namespace

int main() {
    // The writer runs reclamation passes itself: single-threaded, so the
    // failure counter's "n-th allocation" is deterministic.
    NovaKvInspector::set_default_rcu_options(0, 2);
    test_create_index_under_allocation_failure();
    test_indexed_commits_under_allocation_failure();
    for (unsigned bits : {0u, 1u, 3u, 7u}) test_forced_hash_collisions(bits);
    expect_no_leftovers();
    return nova_test::finish("index_failure_test");
}
