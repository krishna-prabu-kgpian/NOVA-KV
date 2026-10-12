// versioned_store_test.cpp -- single-threaded tests of the Part B engine
// (src/versioned_store.h): one publication per logical write, copy-on-write
// with sharing of untouched segments, immutability of every retained version,
// and the ownership accounting (segment owners, chunk pins).
//
// Built against the hook-enabled engine so that object counts can be checked
// after each store is destroyed.
#include "versioned_store.h"

#include "test_points.h"
#include "test_util.h"

#include <climits>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

using nova::BatchOp;
using nova::Segment;
using nova::Version;
using nova::VersionedStore;

namespace {

using Bytes = std::vector<unsigned char>;
using Model = std::unordered_map<long, Bytes>;

// Epoch-based reclamation in both cases; what differs is who runs the passes.
// Manual: nobody until the test calls reclaim_pass()/drain(), so every retired
// version stays (and unprotected version pointers stay valid) until then.
// Inline: the writer runs passes after each commit, so with no reader pinned
// each replaced version is reclaimed before the write returns.
const nova::StoreOptions kManual{nova::ReclaimMode::Epoch, nova::CleanerMode::Manual};
const nova::StoreOptions kInline{nova::ReclaimMode::Epoch, nova::CleanerMode::Inline};


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

bool put(VersionedStore& s, long key, const Bytes& v) { return s.put(key, v.data(), v.size()); }

bool holds_at(const VersionedStore& s, const Version* v, long key, const Bytes& expect) {
    Bytes buf(expect.size() + 8, 0xCD);
    if (!s.get_at_unprotected(v, key, buf.data(), expect.size())) return false;
    for (std::size_t i = expect.size(); i < buf.size(); ++i) {
        if (buf[i] != 0xCD) return false;
    }
    return expect.empty() || std::memcmp(buf.data(), expect.data(), expect.size()) == 0;
}

bool holds(const VersionedStore& s, long key, const Bytes& expect) {
    return holds_at(s, s.current_version_unprotected(), key, expect);
}

bool missing_at(const VersionedStore& s, const Version* v, long key) {
    unsigned char probe[8];
    return !s.get_at_unprotected(v, key, probe, sizeof(probe)) && !s.get_at_unprotected(v, key, nullptr, 0);
}

// The version equals the model exactly, over a known universe of keys.
bool version_equals(const VersionedStore& s, const Version* v, const Model& model,
                    const std::vector<long>& universe) {
    for (long k : universe) {
        auto it = model.find(k);
        if (it == model.end() ? !missing_at(s, v, k) : !holds_at(s, v, k, it->second)) return false;
    }
    return true;
}

void check_valid(VersionedStore& s) {
    const char* err = s.validate();
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// Keys with pairwise different partitions / all in one partition.
std::vector<long> keys_in_distinct_partitions(std::size_t n) {
    std::vector<long> keys;
    bool used[nova::kPartitionCount] = {};
    for (long k = 1; keys.size() < n; ++k) {
        const std::uint32_t p = nova::partition_of(nova::hash_key(k));
        if (!used[p]) {
            used[p] = true;
            keys.push_back(k);
        }
    }
    return keys;
}

std::vector<long> keys_in_partition(std::uint32_t partition, std::size_t n) {
    std::vector<long> keys;
    for (long k = 1; keys.size() < n; ++k) {
        if (nova::partition_of(nova::hash_key(k)) == partition) keys.push_back(k);
    }
    return keys;
}

std::uint32_t part(long key) { return nova::partition_of(nova::hash_key(key)); }

BatchOp op_put(long key, const Bytes& v) { return BatchOp{BatchOp::Kind::Put, key, v.data(), v.size()}; }
BatchOp op_erase(long key) { return BatchOp{BatchOp::Kind::Erase, key, nullptr, 0}; }

// ---- Basics and the exact number of publications -------------------------------
void test_basic_and_publication_count() {
    VersionedStore s(kManual);
    NOVA_CHECK(s.ok());
    const Version* v0 = s.current_version_unprotected();
    NOVA_CHECK(v0 != nullptr && v0->sequence == 0);  // readers never see a null root
    NOVA_CHECK(missing_at(s, v0, 1));

    const Bytes a = pattern(32, 1), b = pattern(48, 2);
    NOVA_CHECK(put(s, 1, a));
    NOVA_CHECK(s.stats().versions_published == 1 && s.current_version_unprotected()->sequence == 1);
    NOVA_CHECK(holds(s, 1, a));
    NOVA_CHECK(put(s, 1, b));  // overwrite
    NOVA_CHECK(s.stats().versions_published == 2);
    NOVA_CHECK(holds(s, 1, b));

    // Erasing an absent key publishes nothing and allocates no version.
    const Version* before = s.current_version_unprotected();
    NOVA_CHECK(!s.erase(999));
    NOVA_CHECK(s.current_version_unprotected() == before && s.stats().versions_published == 2);

    NOVA_CHECK(s.erase(1));
    NOVA_CHECK(s.stats().versions_published == 3 && missing_at(s, s.current_version_unprotected(), 1));
    NOVA_CHECK(!s.erase(1));
    NOVA_CHECK(s.stats().versions_published == 3);

    // Rejected arguments publish nothing.
    const unsigned char one = 0;
    NOVA_CHECK(!s.put(2, nullptr, 8));
    NOVA_CHECK(!s.put(2, &one, (std::size_t{1} << 30) + 1));
    NOVA_CHECK(s.stats().versions_published == 3 && s.stats().commits_failed == 0);

    // Zero-length value: present, distinguishable from missing, no chunk.
    NOVA_CHECK(s.put(3, nullptr, 0));
    NOVA_CHECK(s.get(3, nullptr, 0) && !s.get(4, nullptr, 0));
    NOVA_CHECK(s.stats().versions_published == 4);
    // Every replaced version is still retained: no reclamation pass has run.
    NOVA_CHECK(s.stats().versions_retained == 4 && s.stats().versions_reclaimed == 0);
    check_valid(s);
}

// ---- Copy-on-write: one segment cloned, 255 shared -------------------------------
void test_sharing_of_unchanged_segments() {
    VersionedStore s(kManual);
    const auto keys = keys_in_distinct_partitions(40);
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(put(s, keys[i], pattern(16, i)));
    const Version* v1 = s.current_version_unprotected();
    const std::uint64_t built = s.stats().segments_built;

    NOVA_CHECK(put(s, keys[7], pattern(16, 700)));  // touches exactly one partition
    const Version* v2 = s.current_version_unprotected();
    NOVA_CHECK(v2 != v1 && v2->sequence == v1->sequence + 1);
    NOVA_CHECK(s.stats().segments_built == built + 1);

    std::uint32_t shared = 0, different = 0, populated = 0;
    for (std::uint32_t p = 0; p < nova::kPartitionCount; ++p) {
        if (v2->segments[p] == nullptr) {
            NOVA_CHECK(v1->segments[p] == nullptr);
            continue;
        }
        ++populated;
        if (v2->segments[p] == v1->segments[p]) ++shared;
        else ++different;
    }
    NOVA_CHECK(populated == 40 && different == 1 && shared == 39);
    NOVA_CHECK(v2->segments[part(keys[7])] != v1->segments[part(keys[7])]);
    // Ownership: a shared segment is owned by every version that points at it.
    // The old segment of the touched partition is owned by the versions up to
    // v1 only; the new one by v2 only.
    NOVA_CHECK(v2->segments[part(keys[7])]->owners.load() == 1);
    const Segment* untouched = v2->segments[part(keys[39])];  // created by the 40th put
    NOVA_CHECK(untouched->owners.load() == 2);                // v1 and v2
    const Segment* oldest = v2->segments[part(keys[0])];      // created by the 1st put
    NOVA_CHECK(oldest->owners.load() == 41);                  // versions 1 .. 41
    check_valid(s);

    // Old and new state are both intact.
    NOVA_CHECK(holds_at(s, v1, keys[7], pattern(16, 7)));
    NOVA_CHECK(holds_at(s, v2, keys[7], pattern(16, 700)));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 7) NOVA_CHECK(holds_at(s, v2, keys[i], pattern(16, i)));
    }
}

// ---- Multi-key writes: one publication, one clone per touched segment ---------
void test_batch_groups_by_segment() {
    VersionedStore s(kManual);
    const auto spread = keys_in_distinct_partitions(6);
    const auto same = keys_in_partition(part(spread[0]), 5);  // 5 keys in spread[0]'s partition
    const Bytes v = pattern(24, 9);

    // 6 keys in 6 partitions + 4 more keys in the first partition.
    std::vector<BatchOp> ops;
    for (long k : spread) ops.push_back(op_put(k, v));
    for (std::size_t i = 1; i < same.size(); ++i) ops.push_back(op_put(same[i], v));
    const Version* before = s.current_version_unprotected();
    NOVA_CHECK(s.commit(ops.data(), ops.size()));
    const Version* after = s.current_version_unprotected();
    NOVA_CHECK(s.stats().versions_published == 1);  // ONE publication for 10 keys
    NOVA_CHECK(after->sequence == before->sequence + 1);
    NOVA_CHECK(s.stats().segments_built == 6);      // one clone per touched partition, not per key
    NOVA_CHECK(same[0] == spread[0]);  // both scans start at key 1
    for (long k : spread) NOVA_CHECK(holds(s, k, v));
    for (long k : same) NOVA_CHECK(holds(s, k, v));
    NOVA_CHECK(after->segments[part(spread[0])]->live == 5);
    check_valid(s);

    // Order inside a batch: later operations win; `applied` reports each one.
    const Bytes w1 = pattern(8, 1), w2 = pattern(8, 2);
    const long k1 = spread[1], k2 = spread[2], k3 = spread[3], absent = 123456789;
    const BatchOp batch[] = {
        op_put(k1, w1),  op_put(k1, w2),   // duplicate key: last value wins
        op_erase(k2),    op_put(k2, w1),   // erase then put: present
        op_put(k3, w1),  op_erase(k3),     // put then erase: absent
        op_erase(absent),                  // absent: no effect
        op_erase(absent),
    };
    bool applied[8] = {};
    const std::uint64_t published = s.stats().versions_published;
    const std::uint64_t built = s.stats().segments_built;
    NOVA_CHECK(s.commit(batch, 8, applied));
    NOVA_CHECK(s.stats().versions_published == published + 1);
    NOVA_CHECK(applied[0] && applied[1] && applied[2] && applied[3] && applied[4] && applied[5]);
    NOVA_CHECK(!applied[6] && !applied[7]);
    NOVA_CHECK(holds(s, k1, w2) && holds(s, k2, w1) && missing_at(s, s.current_version_unprotected(), k3));
    // k1, k2, k3: three partitions rebuilt (k3's partition became empty -> dropped,
    // counted separately); the absent key's partition was left shared.
    NOVA_CHECK(s.stats().segments_built - built == 2);
    NOVA_CHECK(s.stats().segments_dropped == 1);
    NOVA_CHECK(s.current_version_unprotected()->segments[part(k3)] == nullptr);
    check_valid(s);

    // A batch with no effect publishes nothing; an invalid batch changes nothing.
    const Version* cur = s.current_version_unprotected();
    const BatchOp noop[] = {op_erase(absent), op_erase(absent + 1)};
    NOVA_CHECK(s.commit(noop, 2));
    NOVA_CHECK(s.current_version_unprotected() == cur && s.stats().commits_without_change == 1);
    const BatchOp invalid[] = {op_put(k1, w1), BatchOp{BatchOp::Kind::Put, k2, nullptr, 4}};
    NOVA_CHECK(!s.commit(invalid, 2));
    NOVA_CHECK(s.current_version_unprotected() == cur && holds(s, k1, w2));
    NOVA_CHECK(s.commit(nullptr, 0) && s.current_version_unprotected() == cur);  // empty batch
    check_valid(s);
}

// ---- Every retained version keeps exactly the state it was published with ------
void test_old_versions_never_change(std::uint64_t seed) {
    VersionedStore s(kManual);
    Model model;
    std::uint64_t x = seed;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    std::vector<long> universe;
    for (int i = 0; i < 300; ++i) universe.push_back(static_cast<long>(next()));
    universe.push_back(LONG_MIN);
    universe.push_back(LONG_MAX);

    struct Checkpoint {
        const Version* version;
        Model model;
        std::vector<std::vector<unsigned char>> raw_slots;  // bytes of each segment's slot array
    };
    std::vector<Checkpoint> checkpoints;
    auto take_checkpoint = [&]() {
        Checkpoint c{s.current_version_unprotected(), model, {}};
        for (const Segment* seg : c.version->segments) {
            std::vector<unsigned char> raw;
            if (seg != nullptr) {
                raw.resize(sizeof(nova::Entry) * seg->capacity);
                std::memcpy(raw.data(), seg->slots, raw.size());
            }
            c.raw_slots.push_back(std::move(raw));
        }
        checkpoints.push_back(std::move(c));
    };

    int errors = 0;
    for (int op = 0; op < 6000; ++op) {
        const long key = universe[next() % universe.size()];
        const std::uint64_t r = next() % 100;
        if (r < 55) {
            const std::uint64_t c = next() % 100;
            const std::size_t len = c < 5 ? 0 : (c < 97 ? 1 + next() % 200 : 66000 + next() % 3000);
            Bytes v = pattern(len, next());
            if (!put(s, key, v)) ++errors;
            model[key] = std::move(v);
        } else if (r < 80) {
            if (s.erase(key) != (model.erase(key) == 1)) ++errors;
        } else {
            // multi-key write of 2..9 operations
            const std::size_t n = 2 + next() % 8;
            std::vector<Bytes> values(n);
            std::vector<BatchOp> ops;
            for (std::size_t i = 0; i < n; ++i) {
                const long k = universe[next() % universe.size()];
                if (next() % 4 == 0) {
                    ops.push_back(op_erase(k));
                    model.erase(k);
                } else {
                    values[i] = pattern(next() % 64, next());
                    ops.push_back(op_put(k, values[i]));
                    model[k] = values[i];
                }
            }
            if (!s.commit(ops.data(), ops.size())) ++errors;
        }
        if (op % 400 == 0) take_checkpoint();
    }
    take_checkpoint();
    NOVA_CHECK(errors == 0);

    // Each checkpointed version: same logical content as when it was current,
    // and its segments' slot arrays are byte-for-byte what they were.
    int logical_mismatches = 0, byte_mismatches = 0;
    for (const Checkpoint& c : checkpoints) {
        if (!version_equals(s, c.version, c.model, universe)) ++logical_mismatches;
        for (std::uint32_t p = 0; p < nova::kPartitionCount; ++p) {
            const Segment* seg = c.version->segments[p];
            const auto& raw = c.raw_slots[p];
            if (seg == nullptr) {
                if (!raw.empty()) ++byte_mismatches;
            } else if (raw.size() != sizeof(nova::Entry) * seg->capacity ||
                       std::memcmp(raw.data(), seg->slots, raw.size()) != 0) {
                ++byte_mismatches;
            }
        }
    }
    NOVA_CHECK(logical_mismatches == 0);
    NOVA_CHECK(byte_mismatches == 0);
    NOVA_CHECK(version_equals(s, s.current_version_unprotected(), model, universe));  // the latest state too

    // No reclamation pass has run: one retained version per publication, no
    // segment released, and no chunk that any version references was freed.
    const nova::VersionedStats st = s.stats();
    NOVA_CHECK(st.versions_retained == st.versions_published);
    NOVA_CHECK(st.versions_reclaimed == 0 && st.segments_released == 0);
    NOVA_CHECK(st.arena.refs_dropped == 0);
    check_valid(s);

    // Running the reclamation passes (no reader is pinned, so the epoch
    // advances) frees the memory only old versions held; the current state is
    // unaffected.
    const std::uint64_t held_before = st.index_bytes_held;
    const std::uint64_t chunks_before = st.arena.chunks_live;
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.stats().versions_retained == 0);
    NOVA_CHECK(s.stats().versions_reclaimed == st.versions_published);
    NOVA_CHECK(s.stats().index_bytes_held < held_before);
    NOVA_CHECK(s.stats().arena.chunks_live < chunks_before && s.stats().arena.chunks_freed > 0);
    NOVA_CHECK(version_equals(s, s.current_version_unprotected(), model, universe));
    check_valid(s);
}

// ---- Reclamation after every write: the ownership graph under constant release ---
void test_quiescent_reclaim_against_model(std::uint64_t seed) {
    VersionedStore s(kInline);
    Model model;
    std::uint64_t x = seed;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    std::vector<long> universe;
    for (int i = 0; i < 1500; ++i) universe.push_back(static_cast<long>(next()));
    int errors = 0;
    for (int op = 0; op < 60000; ++op) {
        const long key = universe[next() % universe.size()];
        const std::uint64_t r = next() % 100;
        if (r < 50) {
            const std::uint64_t c = next() % 1000;
            const std::size_t len = c < 40 ? 0 : (c < 995 ? 1 + next() % 500 : 65000 + next() % 9000);
            Bytes v = pattern(len, next());
            if (!put(s, key, v)) ++errors;
            model[key] = std::move(v);
        } else if (r < 75) {
            if (s.erase(key) != (model.erase(key) == 1)) ++errors;
        } else {
            const std::size_t n = 2 + next() % 15;
            std::vector<Bytes> values(n);
            std::vector<BatchOp> ops;
            for (std::size_t i = 0; i < n; ++i) {
                const long k = universe[next() % universe.size()];
                if (next() % 3 == 0) {
                    ops.push_back(op_erase(k));
                    model.erase(k);
                } else {
                    values[i] = pattern(next() % 300, next());
                    ops.push_back(op_put(k, values[i]));
                    model[k] = values[i];
                }
            }
            if (!s.commit(ops.data(), ops.size())) ++errors;
        }
        if (op % 5000 == 4999) {
            if (!version_equals(s, s.current_version_unprotected(), model, universe)) ++errors;
            if (s.validate() != nullptr) ++errors;
        }
    }
    NOVA_CHECK(errors == 0);
    NOVA_CHECK(version_equals(s, s.current_version_unprotected(), model, universe));
    const nova::VersionedStats st = s.stats();
    NOVA_CHECK(st.versions_retained == 0);
    NOVA_CHECK(st.versions_reclaimed == st.versions_published);
    NOVA_CHECK(st.arena.chunks_freed > 0 && st.arena.ids_reused > 0);  // chunk IDs really recycled
    NOVA_CHECK(st.segments_released > 0);
    // Every pin ever taken was dropped again, except those of live segments.
    std::uint64_t live_pins = 0;
    for (const Segment* seg : s.current_version_unprotected()->segments) {
        if (seg != nullptr) live_pins += seg->pin_count;
    }
    NOVA_CHECK(st.arena.refs_taken - st.arena.refs_dropped == live_pins);
    check_valid(s);
    std::printf("  inline-reclaim seed=%llu: published=%llu segments_built=%llu chunks_freed=%llu ids_reused=%llu\n",
                static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(st.versions_published),
                static_cast<unsigned long long>(st.segments_built),
                static_cast<unsigned long long>(st.arena.chunks_freed),
                static_cast<unsigned long long>(st.arena.ids_reused));
}

// A retained version keeps its chunks alive and their IDs unavailable, even
// after every one of its values has been overwritten in the current state.
void test_retained_version_blocks_chunk_reuse() {
    VersionedStore s(kManual);
    const auto keys = keys_in_distinct_partitions(30);
    const std::size_t big = 40000;  // two values do not fit one 64 KiB chunk
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(put(s, keys[i], pattern(big, i)));
    const Version* old_version = s.current_version_unprotected();
    const std::uint64_t chunks_old = s.stats().arena.chunks_live;
    NOVA_CHECK(chunks_old == keys.size());

    // Overwrite everything, several times. With retention nothing may be freed.
    for (std::uint64_t round = 1; round <= 3; ++round) {
        for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(put(s, keys[i], pattern(big, round * 100 + i)));
    }
    NOVA_CHECK(s.stats().arena.chunks_freed == 0 && s.stats().arena.ids_reused == 0);
    NOVA_CHECK(s.stats().arena.chunks_live == 4 * keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        NOVA_CHECK(holds_at(s, old_version, keys[i], pattern(big, i)));       // old snapshot intact
        NOVA_CHECK(holds(s, keys[i], pattern(big, 300 + i)));                  // new readers: latest
    }
    check_valid(s);

    // Once the old versions have been reclaimed, their chunks are freed and
    // their IDs reused.
    NOVA_CHECK(s.drain());
    NOVA_CHECK(s.stats().arena.chunks_live == keys.size());
    NOVA_CHECK(s.stats().arena.chunks_freed == 3 * keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(put(s, keys[i], pattern(big, 900 + i)));
    NOVA_CHECK(s.stats().arena.ids_reused > 0);
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(holds(s, keys[i], pattern(big, 900 + i)));
    check_valid(s);
}

// Independent growth of one segment; chunk lookups across directory blocks.
void test_growth_and_many_chunks() {
    VersionedStore s(kInline);
    const auto keys = keys_in_partition(42, 3000);
    for (std::size_t i = 0; i < keys.size(); ++i) NOVA_CHECK(s.put(keys[i], &i, sizeof(i)));
    const Version* v = s.current_version_unprotected();
    std::uint32_t populated = 0;
    for (const Segment* seg : v->segments) populated += seg != nullptr ? 1u : 0u;
    NOVA_CHECK(populated == 1);                       // the other 255 partitions untouched
    NOVA_CHECK(v->segments[42]->live == 3000 && v->segments[42]->capacity == 8192);
    NOVA_CHECK(s.stats().segment_growths == 9);       // 16 -> 8192 in nine doublings
    bool ok = true;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::size_t out = 0;
        ok = ok && s.get(keys[i], &out, sizeof(out)) && out == i;
    }
    NOVA_CHECK(ok);
    check_valid(s);

    // 1100 values of 60 KB: one chunk each, more than one directory block.
    VersionedStore t(kInline);
    for (long k = 0; k < 1100; ++k) NOVA_CHECK(put(t, k, pattern(60000, static_cast<std::uint64_t>(k))));
    NOVA_CHECK(t.stats().arena.chunk_ids_issued > nova::kChunkDirBlockSlots);
    ok = true;
    for (long k = 0; k < 1100; ++k) ok = ok && holds(t, k, pattern(60000, static_cast<std::uint64_t>(k)));
    NOVA_CHECK(ok);
    NOVA_CHECK(put(t, 5, pattern(1 << 20, 77)) && holds(t, 5, pattern(1 << 20, 77)));  // 1 MiB
    check_valid(t);
}

}  // namespace

int main() {
    test_basic_and_publication_count();
    test_sharing_of_unchanged_segments();
    test_batch_groups_by_segment();
    test_old_versions_never_change(0xABCDEF12345ULL);
    test_old_versions_never_change(777);
    test_quiescent_reclaim_against_model(0x5EED5EEDULL);
    test_quiescent_reclaim_against_model(31337);
    test_retained_version_blocks_chunk_reuse();
    test_growth_and_many_chunks();

    // Every store above has been destroyed: nothing may be left behind.
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Version) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Segment) == 0);
    NOVA_CHECK(nova::test_hooks::live_objects(nova::LiveObject::Chunk) == 0);
    return nova_test::finish("versioned_store_test");
}
