#include "inspect.h"

#include "kvstore_impl.h"

#include <vector>

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
