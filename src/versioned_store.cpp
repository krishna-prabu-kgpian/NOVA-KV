#include "versioned_store.h"

#include "alloc.h"
#include "test_points.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace nova {

namespace {

std::atomic<bool> g_default_quiescent_reclaim{false};

std::uint64_t segment_bytes(const Segment* s) noexcept {
    return sizeof(Segment) + sizeof(ChunkPin) * std::uint64_t{s->pin_count} +
           sizeof(Entry) * std::uint64_t{s->capacity};
}

// The builders of one commit: one per touched partition. A single-key write
// uses the inline builder; larger batches allocate an array.
class BuilderSet {
public:
    BuilderSet() noexcept : heap_(nullptr), count_(0), constructed_(0) {}
    ~BuilderSet() {
        for (std::uint32_t i = 0; i < constructed_; ++i) at(i).~SegmentBuilder();
        free_bytes(heap_);
    }
    bool init(std::uint32_t count) noexcept {
        count_ = count;
        if (count > 1) {
            heap_ = static_cast<SegmentBuilder*>(
                alloc_bytes(sizeof(SegmentBuilder) * count, alignof(SegmentBuilder)));
            if (heap_ == nullptr) return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            new (&at(i)) SegmentBuilder();
            ++constructed_;
        }
        return true;
    }
    SegmentBuilder& at(std::uint32_t i) noexcept {
        return heap_ != nullptr ? heap_[i] : *reinterpret_cast<SegmentBuilder*>(inline_);
    }
    std::uint32_t count() const noexcept { return count_; }

private:
    alignas(SegmentBuilder) unsigned char inline_[sizeof(SegmentBuilder)];
    SegmentBuilder* heap_;
    std::uint32_t count_;
    std::uint32_t constructed_;
};

}  // namespace

void set_default_quiescent_reclaim(bool enabled) noexcept {
    g_default_quiescent_reclaim.store(enabled);
}

VersionedStore::VersionedStore() noexcept
    : current_(nullptr),
      latest_(nullptr),
      retired_(nullptr),
      quiescent_reclaim_(g_default_quiescent_reclaim.load()),
      stats_(),
      arena_() {
    // Readers never see a null root: an empty version exists from the start.
    void* raw = alloc_bytes(sizeof(Version), alignof(Version));
    if (raw == nullptr) return;
    Version* v = new (raw) Version();
    NOVA_COUNT_OBJECT(LiveObject::Version, 1);
    v->sequence = 0;
    for (Segment*& s : v->segments) s = nullptr;
    latest_ = v;
    stats_.index_bytes_held = sizeof(Version);
    current_.store(v, std::memory_order_seq_cst);
}

VersionedStore::~VersionedStore() {
    // Precondition (blueprint 2.2): no reader or writer is active any more.
    release_retained_quiescent();
    if (latest_ != nullptr) release_version(latest_);
    // The arena frees whatever chunk is left (at most the writable one, if the
    // ownership accounting above is correct; tests check exactly that).
}

// ---- Read path ---------------------------------------------------------------------

bool VersionedStore::read(const Version* v, long key, std::uint64_t h, void* value_out,
                          std::size_t value_out_len) const noexcept {
    const Entry* e = segment_find(v->segments[partition_of(h)], key, h);
    if (e == nullptr) return false;
    const std::uint32_t len = e->value.length;
    if (len > value_out_len) return false;  // never a truncated copy; buffer untouched
    if (len == 0) return true;              // present, empty value
    if (value_out == nullptr) return false;
    NOVA_TEST_POINT(TestPoint::ReaderBeforeCopyOut);
    // The segment pins the chunk, so the directory slot is non-null and the
    // bytes are immutable. Copy out; no internal pointer leaves the store.
    std::memcpy(value_out, arena_.data(e->value), len);
    return true;
}

bool VersionedStore::get(long key, void* value_out, std::size_t value_out_len) const noexcept {
    const std::uint64_t h = hash_key(key);
    // B2 inserts the reader's epoch announcement here, BEFORE the load. In B1
    // nothing is ever freed while readers may run, so no protection is needed.
    //
    // The one and only load of the root in this operation. seq_cst: it is
    // ordered after the writer's exchange that published this version, which
    // in turn is sequenced after every write that initialized the version, its
    // new segments, their slots, the chunk-directory entries and the value
    // bytes. So everything reachable from `v` is fully initialized.
    const Version* v = current_.load(std::memory_order_seq_cst);
    NOVA_TEST_POINT(TestPoint::ReaderAfterRootLoad);
    return read(v, key, h, value_out, value_out_len);
}

bool VersionedStore::get_at(const Version* v, long key, void* value_out,
                            std::size_t value_out_len) const noexcept {
    return read(v, key, hash_key(key), value_out, value_out_len);
}

// ---- Write path --------------------------------------------------------------------

bool VersionedStore::put(long key, const void* value, std::size_t value_len) noexcept {
    const BatchOp op{BatchOp::Kind::Put, key, value, value_len};
    return commit(&op, 1);
}

bool VersionedStore::erase(long key) noexcept {
    // An absent key is reported without building or publishing anything.
    const std::uint64_t h = hash_key(key);
    if (segment_find(latest_->segments[partition_of(h)], key, h) == nullptr) return false;
    const BatchOp op{BatchOp::Kind::Erase, key, nullptr, 0};
    bool applied = false;
    return commit(&op, 1, &applied) && applied;
}

bool VersionedStore::commit(const BatchOp* ops, std::size_t n, bool* applied) noexcept {
    if (n == 0) return true;
    if (n > 0xFFFFFFFFu) return false;

    // ---- Phase 0: validate every argument before anything is built. ----------
    for (std::size_t i = 0; i < n; ++i) {
        if (ops[i].kind != BatchOp::Kind::Put) continue;
        if (ops[i].length > kMaxValueBytes) return false;
        if (ops[i].length != 0 && ops[i].value == nullptr) return false;
    }

    Version* const base = latest_;

    // ---- Phase 1: group by partition; one builder per touched partition. -----
    // Readers and the writer use the same hash and the same partition function.
    std::uint16_t builder_of[kPartitionCount];
    std::uint32_t ops_in[kPartitionCount];
    std::memset(builder_of, 0xFF, sizeof(builder_of));
    std::uint32_t touched = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t p = partition_of(hash_key(ops[i].key));
        if (builder_of[p] == 0xFFFF) {
            builder_of[p] = static_cast<std::uint16_t>(touched);
            ops_in[touched] = 0;
            ++touched;
        }
        ++ops_in[builder_of[p]];
    }

    BuilderSet builders;
    Version* next = nullptr;
    RetiredNode* node = nullptr;
    bool arena_open = false;

    // Everything from here to "nothing can fail" works on private memory only.
    // On any failure: undo the arena appends, let the builders free what they
    // allocated, and report failure. No shared counter has been touched and
    // `current_` has not been written, so there is nothing else to undo.
    auto fail = [&]() {
        if (arena_open) arena_.rollback_commit();
        free_bytes(next);
        free_bytes(node);
        ++stats_.commits_failed;
        return false;
    };

    if (!builders.init(touched)) return fail();
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (builder_of[p] == 0xFFFF) continue;
        if (!builders.at(builder_of[p]).begin(base->segments[p], ops_in[builder_of[p]])) {
            return fail();
        }
    }

    // ---- Phase 2: apply the operations in order to the private copies. ------
    arena_.begin_commit();
    arena_open = true;
    bool any_change = false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t h = hash_key(ops[i].key);
        SegmentBuilder& b = builders.at(builder_of[partition_of(h)]);
        bool took_effect;
        if (ops[i].kind == BatchOp::Kind::Put) {
            if (!b.prepare_put(ops[i].key, h)) return fail();
            ValueRef ref;
            // The value bytes are copied now, while the caller's buffer is
            // certainly valid, into space no published reference covers.
            if (!arena_.append(ops[i].value, static_cast<std::uint32_t>(ops[i].length), &ref)) {
                b.abort_put();
                return fail();
            }
            b.commit_put(ops[i].key, h, ref);
            took_effect = true;
        } else {
            took_effect = b.erase(ops[i].key, h);
        }
        any_change = any_change || took_effect;
        if (applied != nullptr) applied[i] = took_effect;
    }

    if (!any_change) {
        // Only erases of absent keys: the state is unchanged, publish nothing.
        arena_.rollback_commit();
        ++stats_.commits_without_change;
        return true;
    }

    // ---- Phase 3: the remaining allocations. ---------------------------------
    for (std::uint32_t i = 0; i < touched; ++i) {
        if (builders.at(i).changed() && !builders.at(i).prepare_finish()) return fail();
    }
    next = static_cast<Version*>(alloc_bytes(sizeof(Version), alignof(Version)));
    if (next == nullptr) return fail();
    if (!quiescent_reclaim_) {
        // The record that will carry the replaced version is reserved BEFORE
        // publication, so handing the old version over afterwards cannot fail.
        node = static_cast<RetiredNode*>(alloc_bytes(sizeof(RetiredNode)));
        if (node == nullptr) return fail();
    }

    // ================= Nothing below this line can fail. =======================

    // ---- Phase 4: assemble the new version and take ownership. --------------
    new (next) Version();
    NOVA_COUNT_OBJECT(LiveObject::Version, 1);
    next->sequence = base->sequence + 1;
    std::memcpy(next->segments, base->segments, sizeof(next->segments));
    stats_.directory_bytes_copied += sizeof(next->segments);

    bool replaced[kPartitionCount] = {};
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (builder_of[p] == 0xFFFF) continue;
        SegmentBuilder& b = builders.at(builder_of[p]);
        if (!b.changed()) continue;  // e.g. an erase of an absent key: keep sharing
        stats_.index_bytes_copied += b.bytes_built();
        stats_.segment_growths += b.growths();
        stats_.segment_rebuilds += b.rebuilds();
        Segment* seg = b.finish();
        if (seg != nullptr) {
            // Pin every chunk the new segment references BEFORE it can be seen.
            for (std::uint32_t k = 0; k < seg->pin_count; ++k) arena_.pin(seg->pins[k].chunk_id);
            seg->owners.store(1, std::memory_order_seq_cst);
            stats_.index_bytes_held += segment_bytes(seg);
            ++stats_.segments_built;
        } else {
            ++stats_.segments_dropped;
        }
        next->segments[p] = seg;
        replaced[p] = true;
    }
    // Segments carried over unchanged gain one more owning version.
    for (std::uint32_t p = 0; p < kPartitionCount; ++p) {
        if (replaced[p] || next->segments[p] == nullptr) continue;
        next->segments[p]->owners.fetch_add(1, std::memory_order_seq_cst);
        ++stats_.owner_refs_taken;
    }
    stats_.index_bytes_held += sizeof(Version);
    // The new segments hold their pins now; chunks this commit made unreachable
    // (sealed and referenced by nothing) are freed by the arena.
    arena_.end_commit();

    // ---- Phase 5: publish. The linearization point of the whole write. ------
    NOVA_TEST_POINT(TestPoint::WriterBeforePublish);
    // seq_cst exchange: every initialization above is sequenced before it, and
    // any reader load that returns `next` is ordered after it.
    Version* const old = current_.exchange(next, std::memory_order_seq_cst);
    latest_ = next;
    ++stats_.versions_published;
    NOVA_TEST_POINT(TestPoint::WriterAfterPublish);

    // ---- Phase 6: the replaced version. It is still owned here (by `old`) and
    // is handed to the retained list with the pre-allocated node; it is never
    // dropped. B2 will tag it with the retirement epoch at this point.
    if (quiescent_reclaim_) {
        release_version(old);
    } else {
        node->version = old;
        node->next = retired_;
        retired_ = node;
        ++stats_.versions_retained;
    }
    return true;
}

// ---- Ownership release ------------------------------------------------------------

void VersionedStore::release_version(Version* v) noexcept {
    for (Segment* seg : v->segments) {
        if (seg == nullptr) continue;
        // The last owning version takes the segment, and with it the segment's
        // chunk pins, away.
        if (seg->owners.fetch_sub(1, std::memory_order_seq_cst) != 1) continue;
        for (std::uint32_t k = 0; k < seg->pin_count; ++k) arena_.unpin(seg->pins[k].chunk_id);
        stats_.index_bytes_held -= segment_bytes(seg);
        ++stats_.segments_released;
        destroy_segment(seg);
    }
    stats_.index_bytes_held -= sizeof(Version);
    ++stats_.versions_released;
    NOVA_COUNT_OBJECT(LiveObject::Version, -1);
    v->~Version();
    free_bytes(v);
}

void VersionedStore::release_retained_quiescent() noexcept {
    while (retired_ != nullptr) {
        RetiredNode* node = retired_;
        retired_ = node->next;
        release_version(node->version);
        free_bytes(node);
        --stats_.versions_retained;
    }
}

// ---- Introspection -----------------------------------------------------------------

const VersionedStats& VersionedStore::stats() const noexcept {
    stats_.arena = arena_.stats();
    // Commits that changed nothing also unwind the arena, without being failures.
    stats_.arena.rollbacks -= stats_.commits_without_change;
    return stats_;
}

void VersionedStore::for_each_version(void (*fn)(const Version*, void*), void* context) const {
    fn(latest_, context);
    for (const RetiredNode* node = retired_; node != nullptr; node = node->next) {
        fn(node->version, context);
    }
}

const char* VersionedStore::validate() const {
    if (latest_ == nullptr) return "store has no current version";
    if (current_.load(std::memory_order_seq_cst) != latest_) return "current_ differs from the writer's latest version";

    // Every (version, partition) reference that exists.
    std::vector<const Segment*> refs;
    std::uint64_t versions = 0;
    struct Ctx {
        std::vector<const Segment*>* refs;
        std::uint64_t* versions;
    } ctx{&refs, &versions};
    for_each_version(
        [](const Version* v, void* c) {
            Ctx* x = static_cast<Ctx*>(c);
            ++*x->versions;
            for (const Segment* s : v->segments) {
                if (s != nullptr) x->refs->push_back(s);
            }
        },
        &ctx);
    if (versions != stats_.versions_retained + 1) return "retained version count mismatch";
    std::sort(refs.begin(), refs.end());

    std::vector<std::uint32_t> pins(arena_.stats().chunk_ids_issued, 0);
    std::uint64_t held = versions * sizeof(Version);
    for (std::size_t i = 0; i < refs.size();) {
        const Segment* s = refs[i];
        std::size_t j = i;
        while (j < refs.size() && refs[j] == s) ++j;
        // INV: owners == number of versions whose directory points at the segment.
        if (s->owners.load(std::memory_order_seq_cst) != j - i) return "segment owner count mismatch";
        i = j;
        held += segment_bytes(s);

        if (s->capacity == 0 || (s->capacity & (s->capacity - 1)) != 0) return "segment capacity invalid";
        if (s->live == 0) return "published segment has no live key";
        std::uint32_t live = 0, tombs = 0;
        std::vector<ChunkPin> counted;
        for (std::uint32_t k = 0; k < s->capacity; ++k) {
            const Entry& e = s->slots[k];
            if (e.state == SlotState::Tombstone) {
                ++tombs;
                continue;
            }
            if (e.state == SlotState::Empty) continue;
            if (e.state != SlotState::Occupied) return "slot has an invalid state";
            ++live;
            const std::uint64_t h = hash_key(e.key);
            if (segment_find(s, e.key, h) != &e) return "key unreachable or duplicated in its segment";
            if (e.value.length == 0) {
                if (e.value.chunk_id != kNoChunk || e.value.offset != 0) return "empty value does not use the sentinel reference";
                continue;
            }
            const PinnedChunk* c = arena_.chunk(e.value.chunk_id);
            if (c == nullptr) return "ValueRef points at a missing chunk";
            if (c->id != e.value.chunk_id) return "chunk id does not match its directory slot";
            if (std::uint64_t{e.value.offset} + e.value.length > c->used) return "ValueRef exceeds the chunk's written bytes";
            bool found = false;
            for (ChunkPin& cp : counted) {
                if (cp.chunk_id == e.value.chunk_id) {
                    ++cp.refs;
                    found = true;
                    break;
                }
            }
            if (!found) counted.push_back(ChunkPin{e.value.chunk_id, 1});
        }
        if (live != s->live) return "segment live count mismatch";
        if (tombs != s->tombstones) return "segment tombstone count mismatch";
        if ((std::uint64_t{live} + tombs) * kMaxLoadDen > std::uint64_t{s->capacity} * kMaxLoadNum) return "segment exceeds the load bound";
        // INV-05: the pin list is exactly the set of distinct chunks referenced.
        if (counted.size() != s->pin_count) return "segment pin list does not match its entries";
        for (std::uint32_t k = 0; k < s->pin_count; ++k) {
            if (k != 0 && s->pins[k - 1].chunk_id >= s->pins[k].chunk_id) return "segment pin list not sorted";
            bool found = false;
            for (const ChunkPin& cp : counted) {
                if (cp.chunk_id == s->pins[k].chunk_id) {
                    found = cp.refs == s->pins[k].refs;
                    break;
                }
            }
            if (!found) return "segment pin entry does not match its entries";
            ++pins[s->pins[k].chunk_id];
        }
    }
    if (held != stats_.index_bytes_held) return "index bytes held mismatch";

    // Chunks: pins == number of existing segments that list the chunk; an
    // unpinned chunk exists only if it is the writable one.
    std::uint64_t live_chunks = 0;
    for (std::uint32_t id = 0; id < pins.size(); ++id) {
        const PinnedChunk* c = arena_.chunk(id);
        if (c == nullptr) {
            if (pins[id] != 0) return "a segment pins a freed chunk";
            continue;
        }
        ++live_chunks;
        if (c->pins.load(std::memory_order_seq_cst) != pins[id]) return "chunk pin count mismatch";
        if (pins[id] == 0 && c != arena_.current()) return "unpinned sealed chunk not freed";
        if (c->used > c->capacity) return "chunk used beyond its capacity";
    }
    if (live_chunks != arena_.stats().chunks_live) return "live chunk count mismatch";
    return nullptr;
}

}  // namespace nova
