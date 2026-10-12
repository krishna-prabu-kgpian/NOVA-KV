// arena_test.cpp -- unit tests of the chunked value arena (src/arena.h).
#include "arena.h"

#include "test_util.h"

#include <cstdint>
#include <cstring>
#include <vector>

using nova::Arena;
using nova::Chunk;
using nova::ValueRef;

namespace {

std::vector<unsigned char> pattern(std::uint32_t len, unsigned seed) {
    std::vector<unsigned char> v(len);
    for (std::uint32_t i = 0; i < len; ++i) v[i] = static_cast<unsigned char>(seed * 131u + i * 7u);
    return v;
}

bool holds(const Arena& a, const ValueRef& r, const std::vector<unsigned char>& expect) {
    if (r.length != expect.size()) return false;
    if (r.length == 0) return true;
    return std::memcmp(a.data(r), expect.data(), r.length) == 0;
}

void test_zero_length() {
    Arena a(64);
    ValueRef r{1, 2, 3};
    NOVA_CHECK(a.append(nullptr, 0, &r));
    NOVA_CHECK(r.chunk_id == nova::kNoChunk && r.offset == 0 && r.length == 0);
    NOVA_CHECK(a.stats().chunks_live == 0);  // an empty value owns no chunk
    a.release(r);                            // and releasing it is a no-op
    NOVA_CHECK(a.stats().chunks_live == 0 && a.stats().value_bytes_live == 0);
}

void test_boundaries_and_stable_addresses() {
    Arena a(64);
    const auto one = pattern(1, 1);
    ValueRef r1{};
    NOVA_CHECK(a.append(one.data(), 1, &r1));
    NOVA_CHECK(r1.offset == 0 && r1.length == 1);
    const std::byte* p1 = a.data(r1);

    const auto fill = pattern(63, 2);  // exactly fills the chunk: 1 + 63 = 64
    ValueRef r2{};
    NOVA_CHECK(a.append(fill.data(), 63, &r2));
    NOVA_CHECK(r2.chunk_id == r1.chunk_id && r2.offset == 1);
    NOVA_CHECK(a.stats().chunks_live == 1);
    NOVA_CHECK(a.chunk(r1.chunk_id)->used == 64);

    const auto next = pattern(1, 3);  // one more byte needs a second chunk
    ValueRef r3{};
    NOVA_CHECK(a.append(next.data(), 1, &r3));
    NOVA_CHECK(r3.chunk_id != r1.chunk_id && r3.offset == 0);
    NOVA_CHECK(a.stats().chunks_live == 2);
    NOVA_CHECK(a.chunk(r1.chunk_id)->sealed);
    NOVA_CHECK(!a.chunk(r3.chunk_id)->sealed);

    // Earlier values keep their address and bytes while the arena grows.
    NOVA_CHECK(a.data(r1) == p1);
    NOVA_CHECK(holds(a, r1, one) && holds(a, r2, fill) && holds(a, r3, next));

    // A value that does not fit the remaining space goes to a new chunk whole;
    // it is never split.
    const auto big = pattern(64, 4);
    ValueRef r4{};
    NOVA_CHECK(a.append(big.data(), 64, &r4));
    NOVA_CHECK(r4.offset == 0 && r4.chunk_id != r3.chunk_id);
    NOVA_CHECK(holds(a, r4, big));
    NOVA_CHECK(a.stats().value_bytes_live == 1 + 63 + 1 + 64);
}

void test_oversized_value() {
    Arena a(64);
    const auto small = pattern(10, 5);
    ValueRef rs{};
    NOVA_CHECK(a.append(small.data(), 10, &rs));
    const auto huge = pattern(1000, 6);  // > chunk size: dedicated chunk
    ValueRef rh{};
    NOVA_CHECK(a.append(huge.data(), 1000, &rh));
    NOVA_CHECK(rh.chunk_id != rs.chunk_id && rh.offset == 0 && rh.length == 1000);
    const Chunk* hc = a.chunk(rh.chunk_id);
    NOVA_CHECK(hc != nullptr && hc->capacity == 1000 && hc->sealed && hc->live_refs == 1);
    NOVA_CHECK(holds(a, rh, huge));
    // The ordinary writable chunk is unaffected and still accepts appends.
    NOVA_CHECK(a.current() == a.chunk(rs.chunk_id));
    ValueRef rs2{};
    NOVA_CHECK(a.append(small.data(), 10, &rs2));
    NOVA_CHECK(rs2.chunk_id == rs.chunk_id && rs2.offset == 10);
    // Releasing the oversized value frees its chunk at once.
    a.release(rh);
    NOVA_CHECK(a.chunk(rh.chunk_id) == nullptr);
    NOVA_CHECK(a.stats().chunks_freed == 1);
    NOVA_CHECK(holds(a, rs, small) && holds(a, rs2, small));
}

void test_release_frees_chunk_and_reuses_id() {
    Arena a(64);
    const auto v = pattern(40, 7);
    ValueRef a1{}, a2{}, a3{};
    NOVA_CHECK(a.append(v.data(), 40, &a1));  // chunk X
    NOVA_CHECK(a.append(v.data(), 40, &a2));  // chunk Y (40 + 40 > 64); X sealed
    NOVA_CHECK(a1.chunk_id != a2.chunk_id);
    const std::uint32_t x = a1.chunk_id;
    NOVA_CHECK(a.stats().chunk_ids_issued == 2);

    // While a1 is referenced its chunk must stay, whatever else happens.
    NOVA_CHECK(a.append(v.data(), 40, &a3));  // chunk Z
    NOVA_CHECK(a.chunk(x) != nullptr && holds(a, a1, v));
    NOVA_CHECK(a.stats().ids_reused == 0);

    a.release(a1);  // last reference to sealed X
    NOVA_CHECK(a.chunk(x) == nullptr);  // directory slot cleared
    NOVA_CHECK(a.stats().chunks_freed == 1 && a.stats().chunks_live == 2);

    // The next chunk allocation takes X's ID; no new ID is issued.
    ValueRef a4{};
    NOVA_CHECK(a.append(v.data(), 40, &a4));
    NOVA_CHECK(a4.chunk_id == x);
    NOVA_CHECK(a.stats().ids_reused == 1 && a.stats().chunk_ids_issued == 3);
    // Other values were not disturbed by the reuse.
    NOVA_CHECK(holds(a, a2, v) && holds(a, a3, v) && holds(a, a4, v));
}

// A sealed chunk shared by several values is freed only with the LAST of them.
void test_shared_chunk_survives_partial_release() {
    Arena a(64);
    const auto v1 = pattern(20, 21), v2 = pattern(20, 22), v3 = pattern(20, 23);
    ValueRef r1{}, r2{}, r3{}, other{};
    NOVA_CHECK(a.append(v1.data(), 20, &r1));
    NOVA_CHECK(a.append(v2.data(), 20, &r2));
    NOVA_CHECK(a.append(v3.data(), 20, &r3));
    NOVA_CHECK(r1.chunk_id == r2.chunk_id && r2.chunk_id == r3.chunk_id);
    const std::uint32_t shared = r1.chunk_id;
    NOVA_CHECK(a.append(v1.data(), 20, &other));  // 4 bytes left: seals the shared chunk
    NOVA_CHECK(other.chunk_id != shared && a.chunk(shared)->sealed);
    NOVA_CHECK(a.chunk(shared)->live_refs == 3);

    a.release(r2);
    NOVA_CHECK(a.chunk(shared) != nullptr && a.chunk(shared)->live_refs == 2);
    NOVA_CHECK(holds(a, r1, v1) && holds(a, r3, v3));
    a.release(r1);
    NOVA_CHECK(a.chunk(shared) != nullptr && a.chunk(shared)->live_refs == 1);
    NOVA_CHECK(holds(a, r3, v3));
    NOVA_CHECK(a.stats().chunks_freed == 0);
    a.release(r3);  // last reference
    NOVA_CHECK(a.chunk(shared) == nullptr);
    NOVA_CHECK(a.stats().chunks_freed == 1);
    NOVA_CHECK(holds(a, other, v1));
}

void test_writable_chunk_reset() {
    Arena a(64);
    const auto v = pattern(30, 8);
    ValueRef r{};
    NOVA_CHECK(a.append(v.data(), 30, &r));
    const std::uint32_t id = r.chunk_id;
    a.release(r);  // nothing references the writable chunk any more
    NOVA_CHECK(a.chunk(id) != nullptr && a.chunk(id)->used == 0);
    NOVA_CHECK(a.stats().chunks_freed == 0);
    // Overwrite-style churn of one value: append new, then release old.
    ValueRef cur{};
    NOVA_CHECK(a.append(v.data(), 30, &cur));
    for (unsigned i = 0; i < 10000; ++i) {
        const auto nv = pattern(30, i);
        ValueRef nr{};
        NOVA_CHECK(a.append(nv.data(), 30, &nr));
        a.release(cur);
        cur = nr;
        if (!holds(a, cur, nv)) NOVA_CHECK(false);
    }
    // Memory stays bounded: at most the writable chunk plus one sealed chunk.
    NOVA_CHECK(a.stats().chunks_live <= 2);
    NOVA_CHECK(a.stats().chunk_ids_issued <= 2);
    NOVA_CHECK(a.stats().value_bytes_live == 30);
}

void test_many_chunks_across_directory_blocks() {
    Arena a(64);
    const std::uint32_t n = 3 * nova::kChunkDirBlockSlots + 17;  // spans 4 directory blocks
    std::vector<ValueRef> refs(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto v = pattern(64, i);  // one value per chunk
        NOVA_CHECK(a.append(v.data(), 64, &refs[i]));
    }
    NOVA_CHECK(a.stats().chunks_live == n);
    bool all = true;
    for (std::uint32_t i = 0; i < n; ++i) all = all && holds(a, refs[i], pattern(64, i));
    NOVA_CHECK(all);
    // Free every other chunk, then allocate again: recycled IDs first.
    for (std::uint32_t i = 0; i + 1 < n; i += 2) a.release(refs[i]);
    const std::uint64_t issued = a.stats().chunk_ids_issued;
    std::uint32_t reused = 0;
    for (std::uint32_t i = 0; i + 1 < n; i += 2) {
        const auto v = pattern(64, i + 100000);
        NOVA_CHECK(a.append(v.data(), 64, &refs[i]));
        ++reused;
    }
    NOVA_CHECK(a.stats().chunk_ids_issued == issued);
    NOVA_CHECK(a.stats().ids_reused == reused);
    all = true;
    for (std::uint32_t i = 0; i < n; ++i) {
        all = all && holds(a, refs[i], pattern(64, (i % 2 == 0 && i + 1 < n) ? i + 100000 : i));
    }
    NOVA_CHECK(all);
}

void test_directory_exhaustion() {
    Arena a(64, 1);  // one directory block: kChunkDirBlockSlots chunk IDs
    const auto v = pattern(64, 9);
    std::vector<ValueRef> refs(nova::kChunkDirBlockSlots);
    for (auto& r : refs) NOVA_CHECK(a.append(v.data(), 64, &r));
    ValueRef extra{77, 77, 77};
    NOVA_CHECK(!a.append(v.data(), 64, &extra));    // full: fails cleanly
    NOVA_CHECK(!a.append(v.data(), 1000, &extra));  // oversized fails too
    NOVA_CHECK(a.stats().chunks_live == nova::kChunkDirBlockSlots);
    bool all = true;
    for (const auto& r : refs) all = all && holds(a, r, v);
    NOVA_CHECK(all);                                // existing values intact
    NOVA_CHECK(a.append(nullptr, 0, &extra));       // empty values need no chunk
    a.release(refs[5]);                             // freeing one ID makes room again
    NOVA_CHECK(a.append(v.data(), 64, &extra));
    NOVA_CHECK(extra.chunk_id == refs[5].chunk_id);
}

}  // namespace

int main() {
    test_zero_length();
    test_boundaries_and_stable_addresses();
    test_oversized_value();
    test_release_frees_chunk_and_reuses_id();
    test_shared_chunk_survives_partial_release();
    test_writable_chunk_reset();
    test_many_chunks_across_directory_blocks();
    test_directory_exhaustion();
    return nova_test::finish("arena_test");
}
