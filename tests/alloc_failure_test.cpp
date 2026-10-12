// alloc_failure_test.cpp -- allocation-failure injection (audit H11).
//
// Linked against the *_testhooks store library, where the N-th engine
// allocation can be made to fail. For every operation of a script that reaches
// each allocation site (first segment, directory block, free-ID stack, chunk,
// segment growth, new chunk, oversized chunk, tombstone rebuild), the operation
// is retried with the 0th, 1st, 2nd ... allocation failing until it runs with
// no injected failure. After every failed attempt the store must be
// byte-for-byte what the reference model says, structurally valid, and the
// retry must succeed.
#include "kvstore.h"

#include "alloc.h"
#include "hash.h"
#include "inspect.h"
#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "alloc_failure_test must be built with NOVA_KV_TEST_HOOKS"
#endif

namespace {

using Bytes = std::vector<unsigned char>;
namespace hooks = nova::test_hooks;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 17 + i * 3 + 1);
    return v;
}

struct Harness {
    KVStore store;
    std::unordered_map<long, Bytes> model;
    Bytes buf = Bytes(200000);
    long injected_put_failures = 0;
    long injected_erase_faults = 0;
    int state_errors = 0;

    // Full comparison of the store with the model plus structural validation.
    void verify() {
        if (NovaKvInspector::validate(store) != nullptr) ++state_errors;
        if (NovaKvInspector::stats(store).live_keys != model.size()) ++state_errors;
        for (const auto& kv : model) {
            const Bytes& want = kv.second;
            if (!store.get(kv.first, buf.data(), buf.size())) {
                ++state_errors;
            } else if (!want.empty() && std::memcmp(buf.data(), want.data(), want.size()) != 0) {
                ++state_errors;
            }
            if (!want.empty() && store.get(kv.first, buf.data(), want.size() - 1)) ++state_errors;
        }
    }

    void put(long key, const Bytes& v) {
        for (long k = 0;; ++k) {
            hooks::fail_nth_allocation(k);
            const bool ok = store.put(key, v.data(), v.size());
            const bool fired = hooks::allocation_failure_fired();
            hooks::fail_nth_allocation(-1);
            if (!fired) {
                // No failure was injected: the put must have succeeded.
                if (!ok) ++state_errors;
                model[key] = v;
                return;
            }
            // An allocation failed: the put must report failure and must have
            // changed nothing observable.
            ++injected_put_failures;
            if (ok) ++state_errors;
            verify();
            if (k > 64) {  // no operation needs this many allocations
                ++state_errors;
                return;
            }
        }
    }

    // erase() needs no memory to succeed; a failed rebuild allocation must not
    // make it fail or lose data.
    void erase_with_failing_allocation(long key) {
        hooks::fail_nth_allocation(0);
        const bool ok = store.erase(key);
        const bool fired = hooks::allocation_failure_fired();
        hooks::fail_nth_allocation(-1);
        if (fired) ++injected_erase_faults;
        if (ok != (model.erase(key) == 1)) ++state_errors;
        if (fired) verify();
    }
};

}  // namespace

int main() {
    Harness h;

    // 1. First insert into an empty store: the segment array, a directory
    //    block, the free-ID stack and the first chunk are allocated here. The
    //    directory block and the free-ID stack survive a failed attempt (they
    //    are unreachable bookkeeping), so the retries fail at the segment array,
    //    the directory block and the free-ID stack: three injected failures.
    //    Chunk-allocation failure is reached in steps 3 and 4.
    h.put(1, pattern(32, 1));
    NOVA_CHECK(h.injected_put_failures == 3);
    h.verify();

    // 2. One partition filled past its initial capacity: segment growth.
    std::vector<long> same_partition;
    for (long k = 1000; same_partition.size() < 900; ++k) {
        if (nova::partition_of(nova::hash_key(k)) == 3) same_partition.push_back(k);
    }
    for (std::size_t i = 0; i < same_partition.size(); ++i) {
        h.put(same_partition[i], pattern(48, i));
    }
    NOVA_CHECK(NovaKvInspector::stats(h.store).segment_growths >= 1);

    // 3. Overwrites with kilobyte values: chunk rollover with live old values,
    //    chunk frees and ID reuse while failures are injected.
    for (std::uint64_t round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < 200; ++i) {
            h.put(same_partition[i], pattern(1500, round * 1000 + i));
        }
    }
    NOVA_CHECK(NovaKvInspector::stats(h.store).chunks_freed > 0);

    // 4. Values larger than a chunk: dedicated-chunk allocation.
    h.put(5000001, pattern(100000, 9));
    h.put(5000002, pattern(70000, 10));
    h.put(5000001, pattern(150000, 11));  // overwrite oversized with oversized

    // 5. Zero-length values need no arena memory but may need index memory.
    h.put(5000003, Bytes());
    h.put(same_partition[0], Bytes());

    // 6. Erase most of the grown partition with every rebuild allocation
    //    failing: erases still succeed and nothing else is lost.
    const long failures_before_erase = h.injected_put_failures;
    for (std::size_t i = 0; i < 800; ++i) h.erase_with_failing_allocation(same_partition[i]);
    NOVA_CHECK(h.injected_erase_faults > 0);
    NOVA_CHECK(h.injected_put_failures == failures_before_erase);
    h.verify();
    // The rebuilds were all denied, so the tombstones are still there ...
    NOVA_CHECK(NovaKvInspector::stats(h.store).tombstones >= 800);
    // ... and a later erase with memory available clears them.
    hooks::fail_nth_allocation(-1);
    NOVA_CHECK(h.store.erase(same_partition[800]));
    h.model.erase(same_partition[800]);
    NOVA_CHECK(NovaKvInspector::stats(h.store).tombstones == 0);
    h.verify();

    // 7. The store keeps working normally afterwards.
    for (std::size_t i = 0; i < 300; ++i) h.put(same_partition[i], pattern(64, i + 7));
    h.verify();

    NOVA_CHECK(h.state_errors == 0);
    NOVA_CHECK(h.injected_put_failures > 20);
    std::printf("  injected failures: %ld in put (all rolled back), %ld in erase (all tolerated)\n",
                h.injected_put_failures, h.injected_erase_faults);
    return nova_test::finish("alloc_failure_test");
}
