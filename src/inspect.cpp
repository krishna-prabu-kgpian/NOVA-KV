#include "inspect.h"

#include "kvstore_impl.h"

#include <vector>

#if NOVA_KV_SYNC_MODE == NOVA_KV_SYNC_RCU

// ---- Part B (rcu) -------------------------------------------------------------------

using nova::Entry;
using nova::Segment;
using nova::SlotState;
using nova::Version;

NovaKvStats NovaKvInspector::stats(const KVStore& store) {
    const nova::VersionedStore& vs = store.impl_->store;
    const nova::VersionedStats& v = vs.stats();
    NovaKvStats s;
    // Index figures describe the CURRENT version.
    const Version* cur = vs.current_version();
    for (const Segment* seg : cur->segments) {
        if (seg == nullptr) continue;
        s.live_keys += seg->live;
        s.tombstones += seg->tombstones;
        s.slot_capacity += seg->capacity;
        s.segments_allocated += 1;
        for (std::uint32_t i = 0; i < seg->capacity; ++i) {
            if (seg->slots[i].state == SlotState::Occupied) s.value_bytes_live += seg->slots[i].value.length;
        }
    }
    // Newest retained version, if any: how much of the directory is shared.
    struct Ctx {
        const Version* current;
        const Version* previous;
    } ctx{cur, nullptr};
    vs.for_each_version(
        [](const Version* x, void* c) {
            Ctx* k = static_cast<Ctx*>(c);
            if (x != k->current && k->previous == nullptr) k->previous = x;
        },
        &ctx);
    if (ctx.previous != nullptr) {
        for (std::uint32_t p = 0; p < nova::kPartitionCount; ++p) {
            if (cur->segments[p] != nullptr && cur->segments[p] == ctx.previous->segments[p]) {
                ++s.segments_shared_with_previous;
            }
        }
    }
    s.segment_growths = v.segment_growths;
    s.segment_rebuilds = v.segment_rebuilds;
    s.chunks_live = v.arena.chunks_live;
    s.chunk_ids_issued = v.arena.chunk_ids_issued;
    s.chunks_freed = v.arena.chunks_freed;
    s.chunk_ids_reused = v.arena.ids_reused;
    s.chunk_bytes_reserved = v.arena.bytes_reserved;
    s.versions_published = v.versions_published;
    s.versions_retained = v.versions_retained;
    s.versions_released = v.versions_released;
    s.commits_failed = v.commits_failed;
    s.segments_built = v.segments_built;
    s.segments_released = v.segments_released;
    s.index_bytes_copied = v.index_bytes_copied;
    s.directory_bytes_copied = v.directory_bytes_copied;
    s.owner_refs_taken = v.owner_refs_taken;
    s.chunk_pins_taken = v.arena.pins_taken;
    s.value_bytes_appended = v.arena.bytes_appended;
    s.index_bytes_held = v.index_bytes_held;
    return s;
}

const void* NovaKvInspector::lock_address(const KVStore& store) {
    return store.impl_->store.root_address();
}

std::size_t NovaKvInspector::lock_size() { return sizeof(std::atomic<Version*>); }

const char* NovaKvInspector::validate(const KVStore& store) {
    const nova::VersionedStore& vs = store.impl_->store;
    const char* err = vs.validate();
    if (err != nullptr) return err;
    // Each key of the current version lives in its own partition.
    const Version* cur = vs.current_version();
    for (std::uint32_t p = 0; p < nova::kPartitionCount; ++p) {
        const Segment* seg = cur->segments[p];
        if (seg == nullptr) continue;
        for (std::uint32_t i = 0; i < seg->capacity; ++i) {
            const Entry& e = seg->slots[i];
            if (e.state == SlotState::Occupied &&
                nova::partition_of(nova::hash_key(e.key)) != p) {
                return "key stored in the wrong partition";
            }
        }
    }
    return nullptr;
}

bool NovaKvInspector::is_versioned() { return true; }

bool NovaKvInspector::commit_batch(KVStore& store, const NovaBatchOp* ops, std::size_t n) {
    std::vector<nova::BatchOp> batch(n);
    for (std::size_t i = 0; i < n; ++i) {
        batch[i] = nova::BatchOp{ops[i].is_put ? nova::BatchOp::Kind::Put : nova::BatchOp::Kind::Erase,
                                 ops[i].key, ops[i].value, ops[i].length};
    }
    return store.impl_->store.commit(batch.data(), n);
}

const void* NovaKvInspector::current_version(const KVStore& store) {
    return store.impl_->store.current_version();
}

std::uint64_t NovaKvInspector::version_sequence(const void* version) {
    return static_cast<const Version*>(version)->sequence;
}

const void* NovaKvInspector::version_segment(const void* version, unsigned partition) {
    return static_cast<const Version*>(version)->segments[partition];
}

bool NovaKvInspector::get_at(const KVStore& store, const void* version, long key, void* value_out,
                             std::size_t value_out_len) {
    return store.impl_->store.get_at(static_cast<const Version*>(version), key, value_out,
                                     value_out_len);
}

void NovaKvInspector::set_quiescent_reclaim(KVStore& store, bool enabled) {
    store.impl_->store.set_quiescent_reclaim(enabled);
}

void NovaKvInspector::set_default_quiescent_reclaim(bool enabled) {
    nova::set_default_quiescent_reclaim(enabled);
}

void NovaKvInspector::release_retained_quiescent(KVStore& store) {
    store.impl_->store.release_retained_quiescent();
}

std::uint64_t NovaKvInspector::memory_held_bytes(const KVStore& store) {
    return store.impl_->store.memory_held_bytes();
}

#else

// ---- Part A (lock-based variants) ---------------------------------------------------

using nova::Chunk;
using nova::Entry;
using nova::HashSegment;
using nova::SlotState;

NovaKvStats NovaKvInspector::stats(const KVStore& store) {
    const KVStore::Impl& impl = *store.impl_;
    NovaKvStats s;
    for (const HashSegment& seg : impl.segments) {
        s.live_keys += seg.live();
        s.tombstones += seg.tombstones();
        s.slot_capacity += seg.capacity();
        s.segments_allocated += seg.capacity() != 0 ? 1 : 0;
        s.segment_growths += seg.growths();
        s.segment_rebuilds += seg.rebuilds();
    }
    const nova::ArenaStats& a = impl.arena.stats();
    s.chunks_live = a.chunks_live;
    s.chunk_ids_issued = a.chunk_ids_issued;
    s.chunks_freed = a.chunks_freed;
    s.chunk_ids_reused = a.ids_reused;
    s.chunk_bytes_reserved = a.bytes_reserved;
    s.value_bytes_live = a.value_bytes_live;
    return s;
}

const void* NovaKvInspector::lock_address(const KVStore& store) {
    return static_cast<const void*>(&store.impl_->lock);
}

std::size_t NovaKvInspector::lock_size() { return sizeof(nova::StoreLock); }

const char* NovaKvInspector::validate(const KVStore& store) {
    const KVStore::Impl& impl = *store.impl_;
    const nova::Arena& arena = impl.arena;
    const std::uint64_t ids = arena.stats().chunk_ids_issued;
    std::vector<std::uint32_t> refs(ids, 0);
    std::uint64_t value_bytes = 0;

    for (std::uint32_t p = 0; p < nova::kPartitionCount; ++p) {
        const HashSegment& seg = impl.segments[p];
        const std::uint32_t cap = seg.capacity();
        if (cap == 0) {
            if (seg.live() != 0 || seg.tombstones() != 0) return "counts on unallocated segment";
            continue;
        }
        if ((cap & (cap - 1)) != 0) return "segment capacity is not a power of two";
        std::uint32_t live = 0, tombs = 0;
        for (std::uint32_t i = 0; i < cap; ++i) {
            const Entry& e = seg.slots()[i];
            if (e.state == SlotState::Tombstone) {
                ++tombs;
                continue;
            }
            if (e.state == SlotState::Empty) continue;
            if (e.state != SlotState::Occupied) return "slot has an invalid state";
            ++live;
            const std::uint64_t h = nova::hash_key(e.key);
            if (nova::partition_of(h) != p) return "key stored in the wrong partition";
            // find() returns the first match on the probe chain, so this also
            // proves the key is reachable and not stored twice.
            if (seg.find(e.key, h) != &e) return "key unreachable or duplicated in its segment";

            if (e.value.length == 0) {
                if (e.value.chunk_id != nova::kNoChunk || e.value.offset != 0) {
                    return "empty value does not use the sentinel reference";
                }
                continue;
            }
            if (e.value.length > nova::kMaxValueBytes) return "value length above the limit";
            const Chunk* c = arena.chunk(e.value.chunk_id);
            if (c == nullptr) return "ValueRef points at a missing chunk";
            if (c->id != e.value.chunk_id) return "chunk id does not match its directory slot";
            if (std::uint64_t{e.value.offset} + e.value.length > c->used) {
                return "ValueRef exceeds the chunk's written bytes";
            }
            ++refs[e.value.chunk_id];
            value_bytes += e.value.length;
        }
        if (live != seg.live()) return "segment live count mismatch";
        if (tombs != seg.tombstones()) return "segment tombstone count mismatch";
        if (std::uint64_t{live + tombs} * nova::kMaxLoadDen > std::uint64_t{cap} * nova::kMaxLoadNum) {
            return "segment exceeds the load bound";
        }
    }

    std::uint64_t live_chunks = 0, reserved = 0;
    for (std::uint32_t id = 0; id < ids; ++id) {
        const Chunk* c = arena.chunk(id);
        if (c == nullptr) {
            if (refs[id] != 0) return "entries reference a freed chunk";
            continue;
        }
        ++live_chunks;
        reserved += c->capacity;
        if (c->live_refs != refs[id]) return "chunk reference count mismatch";
        if (c->used > c->capacity) return "chunk used beyond its capacity";
        if (c->live_refs == 0 && c != arena.current()) return "unreferenced sealed chunk not freed";
        if (c->live_refs == 0 && c->used != 0) return "unreferenced writable chunk not reset";
        if (c == arena.current() && c->sealed) return "writable chunk is sealed";
    }
    if (live_chunks != arena.stats().chunks_live) return "live chunk count mismatch";
    if (reserved != arena.stats().bytes_reserved) return "reserved byte count mismatch";
    if (value_bytes != arena.stats().value_bytes_live) return "live value byte count mismatch";
    return nullptr;
}

bool NovaKvInspector::is_versioned() { return false; }
bool NovaKvInspector::commit_batch(KVStore&, const NovaBatchOp*, std::size_t) { return false; }
const void* NovaKvInspector::current_version(const KVStore&) { return nullptr; }
std::uint64_t NovaKvInspector::version_sequence(const void*) { return 0; }
const void* NovaKvInspector::version_segment(const void*, unsigned) { return nullptr; }
bool NovaKvInspector::get_at(const KVStore&, const void*, long, void*, std::size_t) { return false; }
void NovaKvInspector::set_quiescent_reclaim(KVStore&, bool) {}
void NovaKvInspector::set_default_quiescent_reclaim(bool) {}
void NovaKvInspector::release_retained_quiescent(KVStore&) {}
std::uint64_t NovaKvInspector::memory_held_bytes(const KVStore&) { return 0; }

#endif  // NOVA_KV_SYNC_MODE
