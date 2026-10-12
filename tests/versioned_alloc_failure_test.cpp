// versioned_alloc_failure_test.cpp -- allocation-failure atomicity of the
// Part B write path (audit H11, blueprint INV-10).
//
// For each operation of a script, the operation is attempted with the 0th, 1st,
// 2nd ... engine allocation failing, until an attempt runs with no injected
// failure. After EVERY failed attempt:
//   * the call reported failure;
//   * no version was published (same root pointer, same sequence, same count);
//   * the store's content equals the reference model exactly;
//   * the arena holds the same chunks and the same committed bytes as before
//     (the failed commit's appends were rolled back);
//   * all ownership counts are consistent (validate());
// and the retry without injection must succeed. Run once retaining replaced
// versions (the B1 default) and once with quiescent release.
#include "versioned_store.h"

#include "alloc.h"
#include "test_points.h"
#include "test_util.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#ifndef NOVA_KV_TEST_HOOKS
#error "versioned_alloc_failure_test must be built with NOVA_KV_TEST_HOOKS"
#endif

using nova::BatchOp;
using nova::Version;
using nova::VersionedStore;
namespace hooks = nova::test_hooks;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes pattern(std::size_t len, std::uint64_t seed) {
    Bytes v(len);
    for (std::size_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 29 + i * 5 + 1);
    return v;
}

struct Harness {
    VersionedStore store;
    std::unordered_map<long, Bytes> model;
    Bytes buf = Bytes(200000);
    long injected_failures = 0;   // attempts that failed and had to change nothing
    long tolerated_failures = 0;  // attempts that completed despite a failed allocation
    long successes = 0;
    int errors = 0;

    void expect(bool cond, const char* what) {
        if (!cond) {
            ++errors;
            std::fprintf(stderr, "alloc-failure check failed: %s\n", what);
        }
    }

    void verify_content() {
        const Version* v = store.current_version();
        for (const auto& kv : model) {
            const Bytes& want = kv.second;
            if (!store.get_at(v, kv.first, buf.data(), buf.size())) {
                expect(false, "model key missing from the store");
            } else if (!want.empty() && std::memcmp(buf.data(), want.data(), want.size()) != 0) {
                expect(false, "value differs from the model");
            }
            if (!want.empty() && store.get_at(v, kv.first, buf.data(), want.size() - 1)) {
                expect(false, "value longer in the model than in the store");
            }
        }
        std::uint64_t live = 0;
        for (const nova::Segment* seg : v->segments) live += seg != nullptr ? seg->live : 0;
        expect(live == model.size(), "live key count differs from the model");
        const char* err = store.validate();
        if (err != nullptr) std::fprintf(stderr, "  validate: %s\n", err);
        expect(err == nullptr, "ownership invariants");
    }

    // Runs one logical write to completion through every possible failure.
    void run(const std::vector<BatchOp>& ops, const std::vector<bool>& expect_applied) {
        for (long k = 0;; ++k) {
            const Version* root_before = store.current_version();
            const nova::VersionedStats before = store.stats();
            const nova::PinnedChunk* writable_before = store.arena().current();
            const std::uint32_t used_before = writable_before != nullptr ? writable_before->used : 0;
            std::vector<char> applied(ops.size(), 0);
            static_assert(sizeof(bool) == 1, "bool array view");

            hooks::fail_nth_allocation(k);
            const bool ok = store.commit(ops.data(), ops.size(), reinterpret_cast<bool*>(applied.data()));
            const bool fired = hooks::allocation_failure_fired();
            hooks::fail_nth_allocation(-1);

            // A commit also completes when the failed allocation was one the
            // engine tolerates: the tombstone-removal rebuild inside an erase.
            // The erase then stands, with its tombstones left for later.
            if (fired && ok) {
                bool has_erase = false;
                for (const BatchOp& o : ops) has_erase = has_erase || o.kind == BatchOp::Kind::Erase;
                expect(has_erase, "only an erase may complete despite a failed allocation");
                ++tolerated_failures;
            }
            if (!fired || ok) {
                expect(ok, "commit without an injected failure must succeed");
                for (std::size_t i = 0; i < ops.size(); ++i) {
                    expect((applied[i] != 0) == expect_applied[i], "per-operation result");
                    if (ops[i].kind == BatchOp::Kind::Put) {
                        const unsigned char* p = static_cast<const unsigned char*>(ops[i].value);
                        model[ops[i].key] = Bytes(p, p + ops[i].length);
                    } else {
                        model.erase(ops[i].key);
                    }
                }
                ++successes;
                verify_content();
                return;
            }

            ++injected_failures;
            const nova::VersionedStats after = store.stats();
            expect(!ok, "commit must report failure when an allocation failed");
            expect(store.current_version() == root_before, "root pointer changed by a failed commit");
            expect(after.versions_published == before.versions_published, "a failed commit published");
            expect(after.versions_retained == before.versions_retained, "retained list changed");
            expect(after.commits_failed == before.commits_failed + 1, "failure not counted");
            expect(after.segments_built == before.segments_built, "segment published by a failed commit");
            expect(after.owner_refs_taken == before.owner_refs_taken, "owner count touched by a failed commit");
            expect(after.arena.pins_taken == before.arena.pins_taken, "chunk pinned by a failed commit");
            expect(after.arena.chunks_live == before.arena.chunks_live, "chunk leaked or lost by a failed commit");
            expect(after.arena.bytes_reserved == before.arena.bytes_reserved, "arena reservation changed");
            expect(after.arena.bytes_appended == before.arena.bytes_appended, "bytes committed by a failed commit");
            expect(after.index_bytes_held == before.index_bytes_held, "index memory accounting changed");
            expect(store.arena().current() == writable_before, "writable chunk changed by a failed commit");
            expect(writable_before == nullptr || writable_before->used == used_before,
                   "unpublished bytes left in the writable chunk by a failed commit");
            verify_content();
            if (k > 400) {
                expect(false, "operation never completed");
                return;
            }
        }
    }

    void put(long key, const Bytes& v) {
        run({BatchOp{BatchOp::Kind::Put, key, v.data(), v.size()}}, {true});
    }
    void erase(long key, bool present) {
        run({BatchOp{BatchOp::Kind::Erase, key, nullptr, 0}}, {present});
    }
};

void scenario(bool quiescent) {
    Harness h;
    h.store.set_quiescent_reclaim(quiescent);

    std::vector<long> part3, spread;
    bool used[nova::kPartitionCount] = {};
    for (long k = 1000; part3.size() < 60; ++k) {
        if (nova::partition_of(nova::hash_key(k)) == 3) part3.push_back(k);
    }
    for (long k = 1; spread.size() < 12; ++k) {
        const std::uint32_t p = nova::partition_of(nova::hash_key(k));
        if (p != 3 && !used[p]) {
            used[p] = true;
            spread.push_back(k);
        }
    }

    // 1. First write into an empty store: builder pins, slot array, directory
    //    block, free-ID stack, chunk, segment header, version, retirement node.
    h.put(part3[0], pattern(32, 1));
    const long first = h.injected_failures;
    // (builder pin list, slot array, directory block, free-ID stack, segment
    // header, and with retention the retirement node; chunk and version
    // allocation failures are reached by later operations.)
    h.expect(first >= 5, "first put should fail at five or more allocation sites");

    // 2. One partition filled through several growths (16 -> 128 slots).
    for (std::size_t i = 1; i < part3.size(); ++i) h.put(part3[i], pattern(40, i));

    // 3. Overwrites with values that roll chunks over; oversized values.
    for (std::size_t i = 0; i < 30; ++i) h.put(part3[i], pattern(9000, 100 + i));
    h.put(spread[0], pattern(100000, 7));
    h.put(spread[0], pattern(150000, 8));
    h.put(spread[1], Bytes());  // zero-length: no arena allocation

    // 4. Multi-key writes across partitions, mixing puts and erases; the arena
    //    appends of earlier operations must be rolled back when a later one fails.
    {
        std::vector<Bytes> vals;
        for (std::size_t i = 0; i < 12; ++i) vals.push_back(pattern(i % 3 == 0 ? 30000 : 64, 200 + i));
        std::vector<BatchOp> ops;
        std::vector<bool> applied;
        for (std::size_t i = 0; i < 12; ++i) {
            ops.push_back(BatchOp{BatchOp::Kind::Put, spread[i], vals[i].data(), vals[i].size()});
            applied.push_back(true);
        }
        ops.push_back(BatchOp{BatchOp::Kind::Erase, part3[5], nullptr, 0});
        applied.push_back(true);
        ops.push_back(BatchOp{BatchOp::Kind::Erase, 987654321, nullptr, 0});
        applied.push_back(false);
        ops.push_back(BatchOp{BatchOp::Kind::Put, part3[6], vals[0].data(), vals[0].size()});
        applied.push_back(true);
        h.run(ops, applied);
    }

    // 5. Erases: each needs memory for a replacement version in this design, so
    //    a failed erase must leave the key in place. Enough erases in one
    //    partition to cross the tombstone-rebuild threshold, and erasing a
    //    partition down to nothing.
    for (std::size_t i = 10; i < 50; ++i) h.erase(part3[i], true);
    for (std::size_t i = 2; i < 12; ++i) h.erase(spread[i], true);

    // 6. The store keeps working normally afterwards.
    for (std::size_t i = 10; i < 40; ++i) h.put(part3[i], pattern(64, 300 + i));
    h.verify_content();

    const nova::VersionedStats st = h.store.stats();
    NOVA_CHECK(h.errors == 0);
    NOVA_CHECK(h.injected_failures > 300);
    NOVA_CHECK(st.commits_failed == static_cast<std::uint64_t>(h.injected_failures));
    NOVA_CHECK(st.versions_published == static_cast<std::uint64_t>(h.successes));
    NOVA_CHECK(st.arena.rollbacks > 0);
    if (quiescent) {
        NOVA_CHECK(st.versions_retained == 0 && st.arena.chunks_freed > 0);
    } else {
        NOVA_CHECK(st.versions_retained == st.versions_published);
    }
    std::printf("  %s: %ld writes completed, %ld injected failures all rolled back "
                "(%llu arena rollbacks), %ld tolerated in erase, first put failed at %ld sites\n",
                quiescent ? "quiescent release" : "retain (B1 default)", h.successes,
                h.injected_failures, static_cast<unsigned long long>(st.arena.rollbacks),
                h.tolerated_failures, first);
}

}  // namespace

int main() {
    scenario(false);
    scenario(true);

    // Construction itself: if the initial version cannot be allocated the store
    // reports it instead of presenting a null root.
    hooks::fail_nth_allocation(0);
    {
        VersionedStore broken;
        NOVA_CHECK(hooks::allocation_failure_fired());
        NOVA_CHECK(!broken.ok());
    }
    hooks::fail_nth_allocation(-1);

    NOVA_CHECK(hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(hooks::live_objects(nova::LiveObject::Chunk) == 0);
    return nova_test::finish("versioned_alloc_failure_test");
}
