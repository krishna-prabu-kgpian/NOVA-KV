// pinned_arena_test.cpp -- unit tests of the Part B value arena
// (src/pinned_arena.h): one reference count per chunk, one freeing rule,
// commit / rollback, the lock-free ID pool, and lock-free directory reads.
#include "pinned_arena.h"

#include "test_points.h"
#include "test_util.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using nova::PinnedArena;
using nova::PinnedChunk;
using nova::ValueRef;

namespace {

std::vector<unsigned char> pattern(std::uint32_t len, unsigned seed) {
    std::vector<unsigned char> v(len);
    for (std::uint32_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 131u + i * 7u + 3u);
    return v;
}

bool holds(const PinnedArena& a, const ValueRef& r, const std::vector<unsigned char>& expect) {
    if (r.length != expect.size()) return false;
    return r.length == 0 || std::memcmp(a.data(r), expect.data(), r.length) == 0;
}

std::uint32_t refs(const PinnedArena& a, std::uint32_t id) { return a.chunk(id)->refs.load(); }

// One committed append whose chunk a "segment" references, as the store does.
ValueRef committed(PinnedArena& a, const std::vector<unsigned char>& v) {
    ValueRef r{};
    a.begin_commit();
    const bool ok = a.append(v.data(), static_cast<std::uint32_t>(v.size()), &r);
    NOVA_CHECK(ok);
    if (r.length != 0) a.add_ref(r.chunk_id);
    a.end_commit();
    return r;
}

void test_zero_length_and_reference_kinds() {
    PinnedArena a(64);
    ValueRef r{1, 2, 3};
    a.begin_commit();
    NOVA_CHECK(a.append(nullptr, 0, &r));
    a.end_commit();
    NOVA_CHECK(r.chunk_id == nova::kNoChunk && r.offset == 0 && r.length == 0);
    NOVA_CHECK(a.stats().chunks_live == 0);

    const auto v1 = pattern(40, 1), v2 = pattern(20, 2);
    const ValueRef r1 = committed(a, v1);
    const std::byte* p1 = a.data(r1);
    // One writable reference + one segment reference.
    NOVA_CHECK(refs(a, r1.chunk_id) == 2);
    const ValueRef r2 = committed(a, v2);
    NOVA_CHECK(r2.chunk_id == r1.chunk_id && r2.offset == 40);
    NOVA_CHECK(refs(a, r1.chunk_id) == 3);
    NOVA_CHECK(a.data(r1) == p1);  // stable address
    NOVA_CHECK(holds(a, r1, v1) && holds(a, r2, v2));
}

// A chunk stays, with its ID and bytes, until its LAST reference is dropped.
void test_last_reference_frees_and_gates_id_reuse() {
    PinnedArena a(64);
    const auto v = pattern(40, 3);
    const ValueRef a1 = committed(a, v);  // chunk X (writable)
    const ValueRef a2 = committed(a, v);  // chunk Y; X sealed, its writable reference dropped
    NOVA_CHECK(a1.chunk_id != a2.chunk_id);
    const std::uint32_t x = a1.chunk_id;
    NOVA_CHECK(refs(a, x) == 1);  // the one segment reference
    a.add_ref(x);                 // a second segment lists X
    NOVA_CHECK(refs(a, x) == 2);

    a.release(x);
    NOVA_CHECK(a.chunk(x) != nullptr && holds(a, a1, v));  // still referenced
    const ValueRef a3 = committed(a, v);                   // needs a new chunk
    NOVA_CHECK(a3.chunk_id != x);                          // X's ID NOT reused while X lives
    NOVA_CHECK(a.stats().ids_reused == 0);

    a.release(x);  // last reference
    NOVA_CHECK(a.chunk(x) == nullptr);  // destroyed, directory slot cleared
    NOVA_CHECK(a.stats().chunks_freed == 1);
    const ValueRef a4 = committed(a, v);
    NOVA_CHECK(a4.chunk_id == x);  // only now may the ID come back
    NOVA_CHECK(a.stats().ids_reused == 1);
    NOVA_CHECK(holds(a, a2, v) && holds(a, a3, v) && holds(a, a4, v));
}

// The writable chunk is kept alive by the writer's own reference and is never
// rewound: bytes once appended are never overwritten.
void test_writable_reference() {
    PinnedArena a(64);
    const auto v1 = pattern(20, 4), v2 = pattern(20, 5);
    const ValueRef r1 = committed(a, v1);
    const std::uint32_t id = r1.chunk_id;
    a.release(id);  // no segment references it any more
    NOVA_CHECK(a.chunk(id) != nullptr && a.chunk(id) == a.current());
    NOVA_CHECK(refs(a, id) == 1);         // the writable reference alone keeps it
    NOVA_CHECK(a.chunk(id)->used == 20);  // NOT rewound
    const ValueRef r2 = committed(a, v2);
    NOVA_CHECK(r2.chunk_id == id && r2.offset == 20);
    NOVA_CHECK(std::memcmp(a.chunk(id)->data(), v1.data(), 20) == 0);
    a.release(id);
    // When it stops being writable its writable reference goes at commit end;
    // with no segment reference left that is the last one.
    const ValueRef r3 = committed(a, pattern(60, 6));
    NOVA_CHECK(r3.chunk_id != id);
    NOVA_CHECK(a.chunk(id) == nullptr);
    NOVA_CHECK(a.stats().chunks_freed == 1);
}

// AUDIT FINDING F2, at arena level: a commit appends to the writable chunk,
// then seals it, and meanwhile the last SEGMENT reference on it is dropped (the
// cleaner released the old segment). The commit's reference must keep the
// chunk alive until the commit has taken its own segment reference or rolled back.
void test_commit_reference_protects_sealed_chunk() {
    for (int outcome = 0; outcome < 2; ++outcome) {  // 0: commit succeeds, 1: rolls back
        PinnedArena a(64);
        const auto old_value = pattern(30, 7), fresh = pattern(30, 8), big = pattern(60, 9);
        const ValueRef old_ref = committed(a, old_value);  // chunk W, writable
        const std::uint32_t w = old_ref.chunk_id;
        NOVA_CHECK(refs(a, w) == 2);

        a.begin_commit();
        ValueRef in_w{}, in_next{};
        NOVA_CHECK(a.append(fresh.data(), 30, &in_w));      // unpublished reference into W
        NOVA_CHECK(in_w.chunk_id == w);
        NOVA_CHECK(a.append(big.data(), 60, &in_next));     // does not fit: W is sealed
        NOVA_CHECK(in_next.chunk_id != w && a.current() != a.chunk(w));
        NOVA_CHECK(refs(a, w) == 2);  // segment reference + the commit's (former writable) reference

        a.release(w);  // "cleaner": the only segment listing W is destroyed NOW
        NOVA_CHECK(a.chunk(w) != nullptr);          // must survive: the commit references it
        NOVA_CHECK(refs(a, w) == 1);
        NOVA_CHECK(holds(a, in_w, fresh));          // and its unpublished bytes are intact

        if (outcome == 0) {
            a.add_ref(w);  // the commit's new segment takes its own reference
            a.add_ref(in_next.chunk_id);
            a.end_commit();
            NOVA_CHECK(a.chunk(w) != nullptr && refs(a, w) == 1);
            NOVA_CHECK(holds(a, in_w, fresh) && holds(a, in_next, big));
            NOVA_CHECK(a.stats().chunks_freed == 0);
        } else {
            a.rollback_commit();
            // W is the writable chunk again, alive, with the failed commit's
            // bytes dropped; the chunk the commit created is gone.
            NOVA_CHECK(a.current() == a.chunk(w) && refs(a, w) == 1);
            NOVA_CHECK(a.chunk(w)->used == 30 && !a.chunk(w)->sealed);
            NOVA_CHECK(a.chunk(in_next.chunk_id) == nullptr);
            NOVA_CHECK(a.stats().chunks_freed == 1);
            const ValueRef again = committed(a, fresh);
            NOVA_CHECK(again.chunk_id == w && again.offset == 30);
        }
    }
}

void test_oversized() {
    PinnedArena a(64);
    const auto small = pattern(10, 7), huge = pattern(1000, 8);
    const ValueRef rs = committed(a, small);
    const ValueRef rh = committed(a, huge);
    NOVA_CHECK(rh.chunk_id != rs.chunk_id && rh.offset == 0);
    NOVA_CHECK(a.chunk(rh.chunk_id)->capacity == 1000 && a.chunk(rh.chunk_id)->sealed);
    NOVA_CHECK(refs(a, rh.chunk_id) == 1);            // only the segment reference
    NOVA_CHECK(a.current() == a.chunk(rs.chunk_id));  // ordinary appends continue there
    NOVA_CHECK(holds(a, rh, huge) && holds(a, rs, small));
    a.release(rh.chunk_id);
    NOVA_CHECK(a.chunk(rh.chunk_id) == nullptr);
}

// A failed commit's appends disappear without trace; earlier data is untouched.
void test_rollback() {
    PinnedArena a(64);
    const auto v = pattern(30, 9);
    const ValueRef keep = committed(a, v);
    const std::uint32_t cur = keep.chunk_id;
    const nova::PinnedArenaStats before = a.stats();

    a.begin_commit();
    ValueRef t1{}, t2{}, t3{}, t4{};
    NOVA_CHECK(a.append(v.data(), 30, &t1));                 // fits the writable chunk
    NOVA_CHECK(a.append(v.data(), 30, &t2));                 // new chunk; the old one is sealed
    NOVA_CHECK(a.append(pattern(500, 1).data(), 500, &t3));  // oversized chunk
    NOVA_CHECK(a.append(v.data(), 30, &t4));
    NOVA_CHECK(a.stats().chunks_live == 3);
    a.rollback_commit();

    NOVA_CHECK(a.stats().chunks_live == before.chunks_live);
    NOVA_CHECK(a.stats().bytes_reserved == before.bytes_reserved);
    NOVA_CHECK(a.stats().bytes_appended == before.bytes_appended);
    NOVA_CHECK(a.stats().rollbacks == 1);
    NOVA_CHECK(a.current() == a.chunk(cur) && refs(a, cur) == 2);
    NOVA_CHECK(a.current()->used == 30 && !a.current()->sealed);
    NOVA_CHECK(a.chunk(t2.chunk_id) == nullptr && a.chunk(t3.chunk_id) == nullptr);
    NOVA_CHECK(holds(a, keep, v));
    // The arena works normally afterwards and reuses the rolled-back IDs.
    const ValueRef again = committed(a, v);
    NOVA_CHECK(again.chunk_id == cur && again.offset == 30);
    const ValueRef more = committed(a, v);
    NOVA_CHECK(more.chunk_id == t2.chunk_id || more.chunk_id == t3.chunk_id);
    NOVA_CHECK(a.stats().ids_reused == 1);
    NOVA_CHECK(holds(a, keep, v) && holds(a, again, v) && holds(a, more, v));

    // Rollback of the very first commit of an arena (no chunk existed before).
    PinnedArena b(64);
    b.begin_commit();
    ValueRef f{};
    NOVA_CHECK(b.append(v.data(), 30, &f));
    b.rollback_commit();
    NOVA_CHECK(b.current() == nullptr && b.stats().chunks_live == 0);
    NOVA_CHECK(holds(b, committed(b, v), v));
}

// Chunks that a commit creates and nothing ends up referencing go at its end.
void test_end_commit_releases_commit_references() {
    PinnedArena a(64);
    const auto v = pattern(40, 10);
    a.begin_commit();
    ValueRef r1{}, r2{}, r3{};
    NOVA_CHECK(a.append(v.data(), 40, &r1));  // chunk A
    NOVA_CHECK(a.append(v.data(), 40, &r2));  // chunk B (A sealed)
    NOVA_CHECK(a.append(v.data(), 40, &r3));  // chunk C (B sealed)
    a.add_ref(r3.chunk_id);                   // only r3 ends up in a segment
    a.end_commit();
    NOVA_CHECK(a.chunk(r1.chunk_id) == nullptr && a.chunk(r2.chunk_id) == nullptr);
    NOVA_CHECK(a.chunk(r3.chunk_id) != nullptr && a.stats().chunks_live == 1);
    NOVA_CHECK(holds(a, r3, v));
}

void test_many_chunks_and_directory_exhaustion() {
    {
        PinnedArena a(64);
        const std::uint32_t n = 2 * nova::kChunkDirBlockSlots + 9;  // three directory blocks
        std::vector<ValueRef> all(n);
        for (std::uint32_t i = 0; i < n; ++i) all[i] = committed(a, pattern(64, i));
        bool ok = true;
        for (std::uint32_t i = 0; i < n; ++i) ok = ok && holds(a, all[i], pattern(64, i));
        NOVA_CHECK(ok);
        NOVA_CHECK(a.stats().chunks_live == n);
        // Free every other one and allocate again: recycled IDs first.
        std::uint32_t freed = 0;
        for (std::uint32_t i = 0; i + 1 < n; i += 2) {
            a.release(all[i].chunk_id);
            ++freed;
        }
        const std::uint64_t issued = a.stats().chunk_ids_issued;
        for (std::uint32_t i = 0; i + 1 < n; i += 2) all[i] = committed(a, pattern(64, i + 100000));
        NOVA_CHECK(a.stats().chunk_ids_issued == issued && a.stats().ids_reused == freed);
        ok = true;
        for (std::uint32_t i = 0; i < n; ++i) {
            ok = ok && holds(a, all[i], pattern(64, (i % 2 == 0 && i + 1 < n) ? i + 100000 : i));
        }
        NOVA_CHECK(ok);
    }
    PinnedArena a(64, 1);  // one block: kChunkDirBlockSlots IDs
    const auto v = pattern(64, 11);
    std::vector<ValueRef> all(nova::kChunkDirBlockSlots);
    for (auto& r : all) r = committed(a, v);
    a.begin_commit();
    ValueRef extra{};
    NOVA_CHECK(!a.append(v.data(), 64, &extra));  // full: clean failure
    NOVA_CHECK(!a.append(v.data(), 1000, &extra));
    a.rollback_commit();
    bool ok = true;
    for (const auto& r : all) ok = ok && holds(a, r, v);
    NOVA_CHECK(ok);
    a.release(all[7].chunk_id);
    const ValueRef again = committed(a, v);
    NOVA_CHECK(again.chunk_id == all[7].chunk_id);
}

// The ID pool with a destroying thread and the allocating writer at once, and
// readers resolving references throughout. A "cleaner" thread releases most of
// the chunks the writer has published; the writer keeps creating chunks, taking
// recycled IDs. Every 16th chunk is never released, and readers read only
// those, so the test itself never touches a chunk that may be gone. Run under
// TSan this is the data-race check for the directory, the reference counts and
// the ID pool; the content checks catch an ID handed out while its previous
// chunk was still referenced, and a directory slot cleared for a live chunk.
void test_concurrent_release_allocate_and_read() {
    PinnedArena a(256);
    const std::uint32_t total = 6000;
    constexpr std::uint32_t kKeepEvery = 16;
    std::vector<ValueRef> all(total);
    std::atomic<std::uint32_t> published{0};  // all[0 .. published) exist
    std::atomic<std::uint32_t> processed{0};  // the cleaner has dealt with all[0 .. processed)
    std::atomic<std::uint64_t> bad{0}, checked{0};
    auto fill_of = [](std::uint32_t i) { return static_cast<unsigned char>(i * 31u + 5u); };

    std::thread cleaner([&]() {
        std::uint32_t done = 0;
        while (done < total) {
            if (published.load() > done) {
                if (done % kKeepEvery != 0) a.release(all[done].chunk_id);
                ++done;
                processed.store(done);
            } else {
                std::this_thread::yield();
            }
        }
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&, t]() {
            std::uint64_t x = 88172645463325252ULL + static_cast<std::uint64_t>(t);
            std::uint64_t local_bad = 0, local_checked = 0;
            for (;;) {
                const std::uint32_t hi = published.load();
                if (hi == 0) {
                    std::this_thread::yield();
                    continue;
                }
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const std::uint32_t i = static_cast<std::uint32_t>(x % ((hi + kKeepEvery - 1) / kKeepEvery)) * kKeepEvery;
                const std::byte* p = a.data(all[i]);  // a kept chunk: its slot must never be cleared
                const auto want = static_cast<std::byte>(fill_of(i));
                for (std::uint32_t k = 0; k < all[i].length; ++k) {
                    if (p[k] != want) ++local_bad;
                }
                ++local_checked;
                if (hi == total && local_checked > 20000) break;
            }
            bad.fetch_add(local_bad);
            checked.fetch_add(local_checked);
        });
    }
    std::vector<unsigned char> buf(200);
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::uint32_t len = 130 + i % 60;  // one value per 256-byte chunk
        std::memset(buf.data(), fill_of(i), len);
        a.begin_commit();
        if (!a.append(buf.data(), len, &all[i])) bad.fetch_add(1);
        a.add_ref(all[i].chunk_id);
        a.end_commit();
        published.store(i + 1);  // seq_cst: everything above happens-before any use of all[i]
        // Test pacing only: do not run far ahead of the cleaner, so that IDs
        // are recycled whatever the machine's load (the arena never waits).
        while (i + 1 > processed.load() + 64) std::this_thread::yield();
    }
    cleaner.join();
    for (auto& r : readers) r.join();
    NOVA_CHECK(bad.load() == 0);
    NOVA_CHECK(checked.load() > 40000);
    const nova::PinnedArenaStats st = a.stats();
    const std::uint32_t kept = (total + kKeepEvery - 1) / kKeepEvery;
    // Everything released is gone, except possibly the last chunk, which is
    // still the writable one and keeps the writer's reference.
    NOVA_CHECK(st.chunks_live == kept || st.chunks_live == kept + 1);
    NOVA_CHECK(st.ids_reused > 1000);            // IDs really were recycled under load
    NOVA_CHECK(st.chunk_ids_issued + st.ids_reused == total);
    bool ok = true;
    for (std::uint32_t i = 0; i < total; i += kKeepEvery) {
        const auto want = static_cast<std::byte>(fill_of(i));
        const std::byte* p = a.data(all[i]);
        for (std::uint32_t k = 0; k < all[i].length; ++k) ok = ok && p[k] == want;
    }
    NOVA_CHECK(ok);
}

}  // namespace

int main() {
    test_zero_length_and_reference_kinds();
    test_last_reference_frees_and_gates_id_reuse();
    test_writable_reference();
    test_commit_reference_protects_sealed_chunk();
    test_oversized();
    test_rollback();
    test_end_commit_releases_commit_references();
    test_many_chunks_and_directory_exhaustion();
    test_concurrent_release_allocate_and_read();
#ifdef NOVA_KV_TEST_HOOKS
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
#endif
    return nova_test::finish("pinned_arena_test");
}
