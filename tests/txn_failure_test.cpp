// txn_failure_test.cpp -- Part C.1: a Transaction under allocation failure.
//
// Built against the *_testhooks libraries (single and rcu), public API only.
// For each scenario the test makes the n-th allocation fail, for n = 0, 1, 2,
// ... until a run completes without the failure being reached. That covers
// every allocation between constructing the Transaction and the end of
// commit(): the Transaction's own state, the staged-operation array and value
// buffer (including their growth), the commit's operation array, the private
// segment copies and their growth, the chunk lists, value chunks, the chunk
// directory block, the segment headers, the new Version and the retirement
// record.
//
// After EVERY injected failure the commit must be all-or-nothing:
//   - commit() returned false  =>  the store holds exactly what it held before
//     (no key of the transaction visible, no value changed, no publication);
//   - commit() returned true   =>  every staged write is visible (an allocation
//     the engine could do without).
// and in both cases: ownership accounting is valid, a snapshot taken before the
// attempt still returns the original data, reclamation still works, the same
// transaction then succeeds, and nothing is leaked (object counters here, the
// leak checker under ASan).
#include "kvstore.h"

#include "alloc.h"
#include "hash.h"
#include "inspect.h"
#include "test_points.h"
#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "txn_failure_test must be built with NOVA_KV_TEST_HOOKS"
#endif

namespace {

using Bytes = std::vector<unsigned char>;
using Model = std::map<long, Bytes>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 97 + i * 29 + 11);
    return v;
}

struct Write {
    long key;
    Bytes value;
};

const bool kVersioned = NovaKvInspector::is_versioned();

bool store_equals(const KVStore& s, const Model& model, long key_limit) {
    static Bytes buf(300000);
    for (long k = 0; k < key_limit; ++k) {
        const auto it = model.find(k);
        if (it == model.end()) {
            if (s.get(k, nullptr, 0) || s.get(k, buf.data(), buf.size())) return false;
        } else {
            const Bytes& v = it->second;
            if (!s.get(k, buf.data(), v.size())) return false;
            if (!v.empty() && std::memcmp(buf.data(), v.data(), v.size()) != 0) return false;
            if (!v.empty() && s.get(k, buf.data(), v.size() - 1)) return false;
        }
    }
    return true;
}

bool snapshot_equals(const KVStore& s, const KVStore::Snapshot& snap, const Model& model, long key_limit) {
    static Bytes buf(300000);
    for (long k = 0; k < key_limit; ++k) {
        const auto it = model.find(k);
        if (it == model.end()) {
            if (s.get(snap, k, buf.data(), buf.size())) return false;
        } else {
            const Bytes& v = it->second;
            if (!s.get(snap, k, buf.data(), v.size())) return false;
            if (!v.empty() && std::memcmp(buf.data(), v.data(), v.size()) != 0) return false;
        }
    }
    std::vector<long> keys(model.size() + 8);
    return s.keys(snap, keys.data(), keys.size()) == model.size();
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

void run_scenario(const char* name, const Model& preload, const std::vector<Write>& writes, long key_limit) {
    Model applied = preload;
    for (const Write& w : writes) applied[w.key] = w.value;  // last write wins

    long failures = 0, rolled_back = 0, tolerated = 0;
    for (long n = 0;; ++n) {
        KVStore s;
        for (const auto& kv : preload) NOVA_CHECK(s.put(kv.first, kv.second.data(), kv.second.size()));
        NovaKvInspector::settle(s);
        std::optional<KVStore::Snapshot> snap;
        if (kVersioned) snap.emplace(s.acquire_snapshot());
        const std::uint64_t published0 = NovaKvInspector::stats(s).versions_published;

        nova::test_hooks::fail_nth_allocation(n);
        bool ok;
        {
            Transaction t(s);
            for (const Write& w : writes) t.put(w.key, w.value.data(), w.value.size());
            ok = t.commit();
            NOVA_CHECK(!t.commit());                 // never a second publication, whatever happened
        }                                            // staged memory released here
        const bool fired = nova::test_hooks::allocation_failure_fired();
        nova::test_hooks::fail_nth_allocation(-1);

        if (!fired) {
            // The n-th allocation does not exist: this was an undisturbed run.
            NOVA_CHECK(ok);
            NOVA_CHECK(store_equals(s, applied, key_limit));
            check_valid(s);
            break;
        }
        ++failures;
        const std::uint64_t published1 = NovaKvInspector::stats(s).versions_published;
        if (ok) {
            ++tolerated;
            NOVA_CHECK(store_equals(s, applied, key_limit));
            if (kVersioned) NOVA_CHECK(published1 == published0 + 1);
        } else {
            ++rolled_back;
            NOVA_CHECK(store_equals(s, preload, key_limit));          // nothing partial is visible
            if (kVersioned) NOVA_CHECK(published1 == published0);     // the root was not replaced
        }
        check_valid(s);                                               // ownership accounting
        if (kVersioned) NOVA_CHECK(snapshot_equals(s, *snap, preload, key_limit));  // the old view is intact

        // The store is fully usable afterwards: the same transaction commits,
        // the snapshot still shows the original data, reclamation completes.
        {
            Transaction again(s);
            for (const Write& w : writes) again.put(w.key, w.value.data(), w.value.size());
            NOVA_CHECK(again.commit());
        }
        NOVA_CHECK(store_equals(s, applied, key_limit));
        if (kVersioned) {
            NOVA_CHECK(snapshot_equals(s, *snap, preload, key_limit));
            snap.reset();
            NOVA_CHECK(NovaKvInspector::settle(s));
        }
        check_valid(s);
    }
    NOVA_CHECK(failures > 0 && rolled_back > 0);
    if (kVersioned) {
        NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
        NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
        NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    }
    std::printf("  %-44s %3zu writes: %3ld allocation points failed in turn; %3ld rolled back, %ld tolerated\n",
                name, writes.size(), failures, rolled_back, tolerated);
}

std::vector<long> keys_in_partition(std::uint32_t p, std::size_t count) {
    std::vector<long> out;
    for (long k = 0; out.size() < count; ++k) {
        if (nova::partition_of(nova::hash_key(k)) == p) out.push_back(k);
    }
    return out;
}

}  // namespace

int main() {
    // The writer runs reclamation passes itself (rcu build): single-threaded,
    // so the failure counter of alloc.h is used by one thread only.
    NovaKvInspector::set_default_rcu_options(0, 2);

    Model preload;
    for (long k = 0; k < 60; k += 2) preload[k] = pattern(40 + static_cast<std::size_t>(k), static_cast<std::uint64_t>(k));

    // 1. One key into an EMPTY store: the first value chunk and the first block
    //    of the chunk directory are allocated inside this commit.
    run_scenario("1 key, empty store", Model{}, {Write{5, pattern(100, 1)}}, 64);

    // 2. Four keys in different partitions: overwrite and insert.
    run_scenario("4 keys, different partitions", preload,
                 {Write{0, pattern(30, 2)}, Write{1, pattern(31, 3)}, Write{2, pattern(32, 4)}, Write{3, pattern(33, 5)}},
                 64);

    // 3. Sixteen writes with repeated keys, an empty value, and values larger
    //    than a chunk (each gets a chunk of its own).
    {
        std::vector<Write> w;
        for (long k = 0; k < 12; ++k) w.push_back(Write{k, pattern(50, 100 + static_cast<std::uint64_t>(k))});
        w.push_back(Write{3, pattern(70000, 7)});
        w.push_back(Write{4, Bytes()});
        w.push_back(Write{3, pattern(90000, 8)});   // replaces the 70000-byte value staged above
        w.push_back(Write{40, pattern(66000, 9)});
        run_scenario("16 writes, repeats, empty and oversized", preload, w, 64);
    }

    // 4. A hundred keys that all fall into one partition: its private copy
    //    grows several times inside the commit.
    {
        std::vector<Write> w;
        long limit = 0;
        for (long k : keys_in_partition(9, 100)) {
            w.push_back(Write{k, pattern(24, static_cast<std::uint64_t>(k))});
            if (k >= limit) limit = k + 1;
        }
        run_scenario("100 keys, one partition", preload, w, limit);
    }

    // 5. A hundred keys across partitions with values that fill several chunks.
    {
        std::vector<Write> w;
        for (long k = 0; k < 100; ++k) w.push_back(Write{k, pattern(3000, 500 + static_cast<std::uint64_t>(k))});
        run_scenario("100 keys, many partitions, 300 KB", preload, w, 128);
    }
    return nova_test::finish("txn_failure_test");
}
