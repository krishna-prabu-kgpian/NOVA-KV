// pinned_arena_test.cpp -- unit tests of the Part B value arena
// (src/pinned_arena.h): structural ownership by pins, commit / rollback, and
// lock-free directory reads concurrent with appends.
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

// One committed append whose chunk a "segment" pins, as the store does.
ValueRef committed(PinnedArena& a, const std::vector<unsigned char>& v) {
    ValueRef r{};
    a.begin_commit();
    const bool ok = a.append(v.data(), static_cast<std::uint32_t>(v.size()), &r);
    NOVA_CHECK(ok);
    if (r.length != 0) a.pin(r.chunk_id);
    a.end_commit();
    return r;
}

void test_zero_length_and_basic() {
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
    const ValueRef r2 = committed(a, v2);
    NOVA_CHECK(r2.chunk_id == r1.chunk_id && r2.offset == 40);
    NOVA_CHECK(a.chunk(r1.chunk_id)->pins.load() == 2);  // two segments' worth of pins
    NOVA_CHECK(a.data(r1) == p1);                        // stable address
    NOVA_CHECK(holds(a, r1, v1) && holds(a, r2, v2));
}

// A chunk stays, with its ID and bytes, until its LAST pin is dropped.
void test_pins_keep_chunks_alive_and_gate_id_reuse() {
    PinnedArena a(64);
    const auto v = pattern(40, 3);
    const ValueRef a1 = committed(a, v);  // chunk X
    const ValueRef a2 = committed(a, v);  // chunk Y; X sealed
    NOVA_CHECK(a1.chunk_id != a2.chunk_id);
    const std::uint32_t x = a1.chunk_id;
    a.pin(x);  // a second version's segment also references X
    NOVA_CHECK(a.chunk(x)->pins.load() == 2);

    a.unpin(x);  // one of the two owners goes away
    NOVA_CHECK(a.chunk(x) != nullptr);  // still referenced: must not be freed
    NOVA_CHECK(holds(a, a1, v));
    const ValueRef a3 = committed(a, v);  // needs a new chunk
    NOVA_CHECK(a3.chunk_id != x);         // X's ID is NOT reused while X is pinned
    NOVA_CHECK(a.stats().ids_reused == 0);
    NOVA_CHECK(holds(a, a1, v));

    a.unpin(x);  // last pin
    NOVA_CHECK(a.chunk(x) == nullptr);  // freed, directory slot cleared
    NOVA_CHECK(a.stats().chunks_freed == 1);
    const ValueRef a4 = committed(a, v);
    NOVA_CHECK(a4.chunk_id == x);  // only now may the ID come back
    NOVA_CHECK(a.stats().ids_reused == 1);
    NOVA_CHECK(holds(a, a2, v) && holds(a, a3, v) && holds(a, a4, v));
}

// The writable chunk is the writer's: it is never freed and never rewound, even
// when nothing pins it. Bytes already handed out are never overwritten.
void test_writable_chunk_is_never_reset() {
    PinnedArena a(64);
    const auto v1 = pattern(20, 4), v2 = pattern(20, 5);
    const ValueRef r1 = committed(a, v1);
    const std::uint32_t id = r1.chunk_id;
    a.unpin(id);  // nothing references the writable chunk any more
    NOVA_CHECK(a.chunk(id) != nullptr && a.chunk(id) == a.current());
    NOVA_CHECK(a.chunk(id)->used == 20);  // NOT rewound (the Part A arena did rewind)
    const ValueRef r2 = committed(a, v2);
    NOVA_CHECK(r2.chunk_id == id && r2.offset == 20);  // appended after, not over, r1's bytes
    NOVA_CHECK(std::memcmp(a.chunk(id)->data(), v1.data(), 20) == 0);
    // Once it is replaced as the writable chunk while unpinned, it is freed.
    a.unpin(id);
    const ValueRef r3 = committed(a, pattern(60, 6));
    NOVA_CHECK(r3.chunk_id != id);
    NOVA_CHECK(a.chunk(id) == nullptr);
}

void test_oversized() {
    PinnedArena a(64);
    const auto small = pattern(10, 7), huge = pattern(1000, 8);
    const ValueRef rs = committed(a, small);
    const ValueRef rh = committed(a, huge);
    NOVA_CHECK(rh.chunk_id != rs.chunk_id && rh.offset == 0);
    NOVA_CHECK(a.chunk(rh.chunk_id)->capacity == 1000 && a.chunk(rh.chunk_id)->sealed);
    NOVA_CHECK(a.current() == a.chunk(rs.chunk_id));  // ordinary appends continue there
    NOVA_CHECK(holds(a, rh, huge) && holds(a, rs, small));
    a.unpin(rh.chunk_id);
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
    NOVA_CHECK(a.append(v.data(), 30, &t1));   // fits the writable chunk (30 + 30)
    NOVA_CHECK(a.append(v.data(), 30, &t2));   // new chunk; the old one is sealed
    NOVA_CHECK(a.append(pattern(500, 1).data(), 500, &t3));  // oversized chunk
    NOVA_CHECK(a.append(v.data(), 30, &t4));
    NOVA_CHECK(a.stats().chunks_live == 3);
    a.rollback_commit();

    NOVA_CHECK(a.stats().chunks_live == before.chunks_live);
    NOVA_CHECK(a.stats().bytes_reserved == before.bytes_reserved);
    NOVA_CHECK(a.stats().bytes_appended == before.bytes_appended);
    NOVA_CHECK(a.stats().rollbacks == 1);
    NOVA_CHECK(a.current() == a.chunk(cur));
    NOVA_CHECK(a.current()->used == 30 && !a.current()->sealed);
    NOVA_CHECK(a.chunk(t2.chunk_id) == nullptr && a.chunk(t3.chunk_id) == nullptr);
    NOVA_CHECK(holds(a, keep, v));
    // The arena works normally afterwards and reuses the rolled-back IDs.
    const ValueRef again = committed(a, v);
    NOVA_CHECK(again.chunk_id == cur && again.offset == 30);
    const ValueRef more = committed(a, v);
    NOVA_CHECK(more.chunk_id == t2.chunk_id || more.chunk_id == t3.chunk_id);
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

// Chunks that a commit seals and leaves unreferenced are freed at its end.
void test_end_commit_frees_unreferenced_sealed_chunks() {
    PinnedArena a(64);
    const auto v = pattern(40, 10);
    a.begin_commit();
    ValueRef r1{}, r2{}, r3{};
    NOVA_CHECK(a.append(v.data(), 40, &r1));  // chunk A
    NOVA_CHECK(a.append(v.data(), 40, &r2));  // chunk B (A sealed)
    NOVA_CHECK(a.append(v.data(), 40, &r3));  // chunk C (B sealed)
    // The batch superseded r1 and r2 itself: only r3 ends up in a segment.
    a.pin(r3.chunk_id);
    a.end_commit();
    NOVA_CHECK(a.chunk(r1.chunk_id) == nullptr && a.chunk(r2.chunk_id) == nullptr);
    NOVA_CHECK(a.chunk(r3.chunk_id) != nullptr && a.stats().chunks_live == 1);
    NOVA_CHECK(holds(a, r3, v));
}

void test_many_chunks_and_directory_exhaustion() {
    {
        PinnedArena a(64);
        const std::uint32_t n = 2 * nova::kChunkDirBlockSlots + 9;  // three directory blocks
        std::vector<ValueRef> refs(n);
        for (std::uint32_t i = 0; i < n; ++i) refs[i] = committed(a, pattern(64, i));
        bool all = true;
        for (std::uint32_t i = 0; i < n; ++i) all = all && holds(a, refs[i], pattern(64, i));
        NOVA_CHECK(all);
        NOVA_CHECK(a.stats().chunks_live == n);
    }
    PinnedArena a(64, 1);  // one block: kChunkDirBlockSlots IDs
    const auto v = pattern(64, 11);
    std::vector<ValueRef> refs(nova::kChunkDirBlockSlots);
    for (auto& r : refs) r = committed(a, v);
    a.begin_commit();
    ValueRef extra{};
    NOVA_CHECK(!a.append(v.data(), 64, &extra));    // full: clean failure
    NOVA_CHECK(!a.append(v.data(), 1000, &extra));
    a.rollback_commit();
    bool all = true;
    for (const auto& r : refs) all = all && holds(a, r, v);
    NOVA_CHECK(all);
    a.unpin(refs[7].chunk_id);                      // one chunk becomes unreachable
    const ValueRef again = committed(a, v);
    NOVA_CHECK(again.chunk_id == refs[7].chunk_id);
}

// Readers resolve references through the directory while the writer keeps
// appending and installing new chunks and directory blocks. Run under TSan this
// is the data-race check for the directory atomics and the "bytes beyond every
// published reference" rule. A reader only ever uses references the writer has
// already handed over through an atomic counter (the stand-in for publication).
void test_concurrent_directory_reads() {
    PinnedArena a(256);
    const std::uint32_t total = 3000;  // > 2 directory blocks at ~1 value per chunk
    std::vector<ValueRef> refs(total);
    std::atomic<std::uint32_t> published{0};
    std::atomic<std::uint64_t> bad{0}, checked{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t]() {
            std::uint64_t x = 88172645463325252ULL + static_cast<std::uint64_t>(t);
            std::uint64_t local_bad = 0, local_checked = 0;
            for (;;) {
                const std::uint32_t n = published.load();
                if (n == 0) {
                    std::this_thread::yield();
                    continue;
                }
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const std::uint32_t i = static_cast<std::uint32_t>(x % n);
                const std::byte* p = a.data(refs[i]);
                const auto want = static_cast<std::byte>(i * 31u + 5u);
                for (std::uint32_t k = 0; k < refs[i].length; ++k) {
                    if (p[k] != want) ++local_bad;
                }
                ++local_checked;
                if (n == total && local_checked > 20000) break;
            }
            bad.fetch_add(local_bad);
            checked.fetch_add(local_checked);
        });
    }
    std::vector<unsigned char> buf(200);
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::uint32_t len = 130 + i % 60;  // 1 value per 256-byte chunk
        std::memset(buf.data(), static_cast<int>(static_cast<unsigned char>(i * 31u + 5u)), len);
        a.begin_commit();
        const bool ok = a.append(buf.data(), len, &refs[i]);
        if (!ok) bad.fetch_add(1);
        a.pin(refs[i].chunk_id);
        a.end_commit();
        published.store(i + 1);  // seq_cst: everything above happens-before a reader's use
    }
    for (auto& r : readers) r.join();
    NOVA_CHECK(bad.load() == 0);
    NOVA_CHECK(checked.load() > 60000);
    NOVA_CHECK(a.stats().chunk_ids_issued > 2 * nova::kChunkDirBlockSlots);
}

}  // namespace

int main() {
    test_zero_length_and_basic();
    test_pins_keep_chunks_alive_and_gate_id_reuse();
    test_writable_chunk_is_never_reset();
    test_oversized();
    test_rollback();
    test_end_commit_frees_unreferenced_sealed_chunks();
    test_many_chunks_and_directory_exhaustion();
    test_concurrent_directory_reads();
#ifdef NOVA_KV_TEST_HOOKS
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
#endif
    return nova_test::finish("pinned_arena_test");
}
