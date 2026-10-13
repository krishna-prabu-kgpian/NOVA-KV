// join.h -- the Part C.2 index-nested-loop join, written against the public
// KVStore API only (kvstore.h). Used by tests/join_test.cpp and
// benchmarks/join_bench.cpp.
//
//   outer table   one KVStore (Trades). Enumerated with keys().
//   inner table   another KVStore (Instruments), with a secondary index on the
//                 join field. Probed with index_get(), rows fetched with get().
//
// SNAPSHOTS: ONE PER STORE (project decision D-060, "Option A").
//
// A Snapshot pins one version of ONE KVStore. The two tables are two
// independently owned stores, so a join holds two handles, both acquired at its
// very start by acquire_join_snapshots() below, the only place in this file
// that acquires a snapshot. Every call on the outer store uses the outer
// handle, every call on the inner store uses the inner handle, and no call
// reads a store's current version.
//
// What that guarantees:
//   - the outer table is read from one immutable version for the whole join;
//   - the inner table is read from one immutable version for the whole join
//     (its rows AND its index: they are parts of the same version);
//   - so each table is internally consistent throughout, whatever the writer
//     publishes meanwhile, and the writer is never blocked.
// What it does NOT guarantee:
//   - that the two versions belong to one instant. Between the two
//     acquisitions a commit to either store can be published. If both tables
//     are being written, the pair is not an atomic cross-store snapshot.
//   - It uses two handles, not the single handle of the assignment's wording
//     ("acquire exactly one Snapshot handle"). This is a documented deviation.
#pragma once

#include "kvstore.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace nova_join {

// Record layouts of the assignment's benchmark (the same as the private structs
// of supplied/workload_gen.cpp).
struct Trade {
    long trade_id;
    char symbol[8];
    double price;
    long ts;
};
struct Instrument {
    char symbol[8];
    char venue[8];
    int tick_size;
};

// ---- The two snapshots of one join ---------------------------------------------------
struct JoinSnapshots {
    KVStore::Snapshot outer;  // pins one version of the outer store
    KVStore::Snapshot inner;  // pins one version of the inner store
};

// Acquires exactly one Snapshot from each store. Call once, at the very start
// of a join, and keep the result alive until the join has finished.
inline JoinSnapshots acquire_join_snapshots(const KVStore& outer, const KVStore& inner) {
    return JoinSnapshots{outer.acquire_snapshot(), inner.acquire_snapshot()};
}

// Both tables hold fixed-size records.
struct JoinSpec {
    std::size_t outer_row_size;      // bytes of an outer record
    std::size_t outer_field_offset;  // where the join field is in it
    std::size_t field_len;           // join field length = the inner index's field length
    std::size_t inner_row_size;      // bytes of an inner record
    // true: a record whose stored length is not exactly the configured size is
    // skipped and counted (one extra lookup per record). false: a shorter
    // record is read zero-padded; a longer one is skipped and counted.
    bool exact_row_sizes = true;
};

inline JoinSpec trade_instrument_spec() {
    return JoinSpec{sizeof(Trade), offsetof(Trade, symbol), sizeof(Trade::symbol), sizeof(Instrument), true};
}

struct JoinStats {
    std::size_t outer_rows = 0;        // outer keys enumerated
    std::size_t outer_skipped = 0;     // outer records of the wrong size
    std::size_t probes = 0;            // index probes
    std::size_t matched_outer = 0;     // outer rows with at least one match
    std::size_t unmatched_outer = 0;   // outer rows with none
    std::size_t joined_rows = 0;       // (outer, inner) pairs emitted
    std::size_t inner_skipped = 0;     // index matches whose record was of the wrong size
};

// Reusable buffers, so that repeated joins do not allocate.
struct JoinBuffers {
    std::vector<long> outer_keys;
    std::vector<long> matches;
    std::vector<unsigned char> outer_row;
    std::vector<unsigned char> inner_row;
};

namespace detail {

// Reads the record of `key` under `snap`. False if it is absent or (with
// `exact`) not exactly `size` bytes long.
inline bool fetch(const KVStore& store, const KVStore::Snapshot& snap, long key, unsigned char* buf,
                  std::size_t size, bool exact) {
    std::memset(buf, 0, size);
    if (!store.get(snap, key, buf, size)) return false;  // absent, or longer than `size`
    if (exact && size != 0 && store.get(snap, key, buf, size - 1)) return false;  // shorter than `size`
    return true;
}

}  // namespace detail

// For every record of the outer table: probe the inner table's index with the
// record's join field and, for every match, fetch the complete inner record and
// call emit(outer_key, outer_row, inner_key, inner_row). All four kinds of call
// (keys, outer get, index_get, inner get) are snapshot-scoped, each with the
// handle of its own store. An outer record may match any number of inner
// records, including none.
template <class Emit>
JoinStats index_nested_loop_join(const KVStore& outer, const KVStore& inner, const JoinSnapshots& snaps,
                                 const JoinSpec& spec, JoinBuffers& buf, Emit&& emit) {
    JoinStats st;
    buf.outer_row.resize(spec.outer_row_size + 1);
    buf.inner_row.resize(spec.inner_row_size + 1);

    // 1. The outer table's keys, under the outer snapshot. keys() reports how
    //    many it wrote, not how many there are: a full buffer means "maybe
    //    more", so ask again with a larger one (the order is repeatable).
    if (buf.outer_keys.size() < 1024) buf.outer_keys.resize(1024);
    std::size_t n = outer.keys(snaps.outer, buf.outer_keys.data(), buf.outer_keys.size());
    while (n == buf.outer_keys.size()) {
        buf.outer_keys.resize(buf.outer_keys.size() * 2);
        n = outer.keys(snaps.outer, buf.outer_keys.data(), buf.outer_keys.size());
    }
    st.outer_rows = n;
    if (buf.matches.size() < 16) buf.matches.resize(16);

    for (std::size_t i = 0; i < n; ++i) {
        const long outer_key = buf.outer_keys[i];
        // 2. The complete outer record, under the outer snapshot.
        if (!detail::fetch(outer, snaps.outer, outer_key, buf.outer_row.data(), spec.outer_row_size,
                           spec.exact_row_sizes)) {
            ++st.outer_skipped;
            continue;
        }
        // 3. + 4. Its join field probes the inner index, under the INNER
        //    snapshot. A full buffer may have been truncated: probe again with
        //    a larger one (the result is in ascending key order, so nothing is
        //    lost or duplicated by asking again).
        const unsigned char* field = buf.outer_row.data() + spec.outer_field_offset;
        ++st.probes;
        int m = inner.index_get(snaps.inner, field, spec.field_len, buf.matches.data(),
                                static_cast<int>(buf.matches.size()));
        while (m == static_cast<int>(buf.matches.size())) {
            buf.matches.resize(buf.matches.size() * 2);
            m = inner.index_get(snaps.inner, field, spec.field_len, buf.matches.data(),
                                static_cast<int>(buf.matches.size()));
        }
        std::size_t emitted = 0;
        for (int k = 0; k < m; ++k) {
            const long inner_key = buf.matches[static_cast<std::size_t>(k)];
            // 5. The complete inner record, under the inner snapshot.
            if (!detail::fetch(inner, snaps.inner, inner_key, buf.inner_row.data(), spec.inner_row_size,
                               spec.exact_row_sizes)) {
                ++st.inner_skipped;
                continue;
            }
            // 6. One joined row.
            emit(outer_key, static_cast<const unsigned char*>(buf.outer_row.data()), inner_key,
                 static_cast<const unsigned char*>(buf.inner_row.data()));
            ++emitted;
        }
        st.joined_rows += emitted;
        if (emitted != 0) ++st.matched_outer;
        else ++st.unmatched_outer;
    }
    return st;
}

}  // namespace nova_join
