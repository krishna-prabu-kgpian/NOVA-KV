// join_test.cpp -- Part C.2: the index-nested-loop join (benchmarks/join.h) on
// the rcu build, through the public API.
//
// Every join result is compared, row for row and byte for byte, with ORACLES
// that do not use the store's index:
//   - a model oracle: the two reference models (ordered maps of key -> bytes)
//     as they were when the join's two snapshots were taken, joined by brute
//     force;
//   - a snapshot oracle: both snapshots enumerated with keys()/get() and joined
//     by a plain nested loop over all pairs.
//
// SNAPSHOT SEMANTICS UNDER TEST: ONE SNAPSHOT PER STORE (decision D-060).
// Asserted: each table is read from one unchanging version for the whole join.
// NOT asserted, because it is not provided: that the two versions belong to
// one instant. The concurrent test counts joins in which they demonstrably do
// not, and prints the count.
#include "join.h"

#include "inspect.h"
#include "test_util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using nova_join::Instrument;
using nova_join::JoinBuffers;
using nova_join::JoinSnapshots;
using nova_join::JoinSpec;
using nova_join::JoinStats;
using nova_join::Trade;
using Bytes = std::vector<unsigned char>;
using Model = std::map<long, Bytes>;

const JoinSpec kSpec = nova_join::trade_instrument_spec();

// ---- Self-validating records ------------------------------------------------------------
// A trade's price and timestamp, and an instrument's venue, are functions of
// the record's key, symbol and generation, so a record assembled from two
// writes, or paired with the wrong key, is detectable from its own bytes.
void set_symbol(char (&dst)[8], const char* text) {
    std::memset(dst, 0, 8);
    for (int i = 0; i < 8 && text[i] != '\0'; ++i) dst[i] = text[i];
}

long trade_check(long id, const char* symbol, std::uint64_t gen) {
    std::uint64_t x = static_cast<std::uint64_t>(id) * 1315423911u + gen * 2654435761u;
    for (int i = 0; i < 8; ++i) x = x * 31 + static_cast<unsigned char>(symbol[i]);
    return static_cast<long>(x & 0x7FFFFFFFFFFFFFFFULL);
}

Bytes trade_row(long id, const char* symbol, std::uint64_t gen) {
    Trade t;
    std::memset(&t, 0, sizeof(t));
    t.trade_id = id;
    set_symbol(t.symbol, symbol);
    t.price = static_cast<double>(gen);
    t.ts = trade_check(id, t.symbol, gen);
    Bytes v(sizeof(t));
    std::memcpy(v.data(), &t, sizeof(t));
    return v;
}

Bytes instrument_row(long key, const char* symbol, std::uint64_t gen) {
    Instrument ins;
    std::memset(&ins, 0, sizeof(ins));
    set_symbol(ins.symbol, symbol);
    std::snprintf(ins.venue, sizeof(ins.venue), "V%06lu", static_cast<unsigned long>((static_cast<std::uint64_t>(key) * 7 + gen) % 1000000));
    ins.tick_size = static_cast<int>(gen);
    Bytes v(sizeof(ins));
    std::memcpy(v.data(), &ins, sizeof(ins));
    return v;
}

bool trade_ok(long key, const unsigned char* row) {
    Trade t;
    std::memcpy(&t, row, sizeof(t));
    const std::uint64_t gen = static_cast<std::uint64_t>(t.price);
    return t.trade_id == key && t.ts == trade_check(key, t.symbol, gen);
}

bool instrument_ok(long key, const unsigned char* row) {
    Instrument ins;
    std::memcpy(&ins, row, sizeof(ins));
    char want[8];
    std::snprintf(want, sizeof(want), "V%06lu",
                  static_cast<unsigned long>((static_cast<std::uint64_t>(key) * 7 + static_cast<std::uint64_t>(ins.tick_size)) % 1000000));
    return std::memcmp(ins.venue, want, 8) == 0;
}

bool put(KVStore& s, Model& m, long key, const Bytes& v) {
    m[key] = v;
    return s.put(key, v.data(), v.size());
}

void check_valid(KVStore& s) {
    const char* err = NovaKvInspector::validate(s);
    if (err != nullptr) std::fprintf(stderr, "invariant violated: %s\n", err);
    NOVA_CHECK(err == nullptr);
}

// ---- Join results and oracles --------------------------------------------------------------
using Joined = std::tuple<long, Bytes, long, Bytes>;  // outer key, outer row, inner key, inner row

struct Result {
    std::vector<Joined> rows;  // sorted
    JoinStats stats;
    bool rows_self_valid = true;
};

template <class During>
Result run_join(const KVStore& trades, const KVStore& instruments, const JoinSnapshots& snaps, During&& during) {
    Result r;
    JoinBuffers buf;
    r.stats = nova_join::index_nested_loop_join(
        trades, instruments, snaps, kSpec, buf,
        [&](long tk, const unsigned char* trow, long ik, const unsigned char* irow) {
            r.rows.emplace_back(tk, Bytes(trow, trow + sizeof(Trade)), ik, Bytes(irow, irow + sizeof(Instrument)));
            r.rows_self_valid = r.rows_self_valid && trade_ok(tk, trow) && instrument_ok(ik, irow) &&
                                std::memcmp(trow + offsetof(Trade, symbol), irow, 8) == 0;
            during(r.rows.size());
        });
    std::sort(r.rows.begin(), r.rows.end());
    NOVA_CHECK(r.stats.joined_rows == r.rows.size());
    NOVA_CHECK(r.stats.matched_outer + r.stats.unmatched_outer + r.stats.outer_skipped == r.stats.outer_rows);
    return r;
}

Result run_join(const KVStore& trades, const KVStore& instruments, const JoinSnapshots& snaps) {
    return run_join(trades, instruments, snaps, [](std::size_t) {});
}

// The join of two tables given as models, by brute force. Only records of
// exactly the configured sizes take part (join.h: exact_row_sizes).
std::vector<Joined> oracle(const Model& trades, const Model& instruments) {
    std::vector<Joined> out;
    for (const auto& t : trades) {
        if (t.second.size() != sizeof(Trade)) continue;
        for (const auto& i : instruments) {
            if (i.second.size() != sizeof(Instrument)) continue;
            if (std::memcmp(t.second.data() + offsetof(Trade, symbol), i.second.data(), 8) != 0) continue;
            out.emplace_back(t.first, t.second, i.first, i.second);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A table as one snapshot shows it, obtained with keys() and get() only.
Model table_under(const KVStore& s, const KVStore::Snapshot& snap) {
    std::vector<long> keys(1024);
    std::size_t n = s.keys(snap, keys.data(), keys.size());
    while (n == keys.size()) {
        keys.resize(keys.size() * 2);
        n = s.keys(snap, keys.data(), keys.size());
    }
    Model m;
    Bytes buf(256);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t size = 0;
        while (size <= buf.size() && !s.get(snap, keys[i], buf.data(), size)) ++size;
        NOVA_CHECK(size <= buf.size());
        m[keys[i]] = Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(size));
    }
    return m;
}

std::vector<Joined> snapshot_oracle(const KVStore& trades, const KVStore& instruments, const JoinSnapshots& snaps) {
    return oracle(table_under(trades, snaps.outer), table_under(instruments, snaps.inner));
}

// One join with fresh snapshots, checked against both oracles.
Result checked_join(const KVStore& trades, const KVStore& instruments, const Model& tm, const Model& im) {
    const JoinSnapshots snaps = nova_join::acquire_join_snapshots(trades, instruments);
    Result r = run_join(trades, instruments, snaps);
    NOVA_CHECK(r.rows == oracle(tm, im));
    NOVA_CHECK(r.rows == snapshot_oracle(trades, instruments, snaps));
    NOVA_CHECK(r.rows_self_valid);
    return r;
}

struct Tables {
    KVStore trades, instruments;
    Model tm, im;
    Tables() { NOVA_CHECK(instruments.create_index(offsetof(Instrument, symbol), sizeof(Instrument::symbol))); }
    void trade(long id, const char* sym, std::uint64_t gen = 1) { NOVA_CHECK(put(trades, tm, id, trade_row(id, sym, gen))); }
    void instrument(long key, const char* sym, std::uint64_t gen = 1) {
        NOVA_CHECK(put(instruments, im, key, instrument_row(key, sym, gen)));
    }
    Result join() { return checked_join(trades, instruments, tm, im); }
};

std::string sym(int i) {
    char b[16];
    std::snprintf(b, sizeof(b), "SYM%03d", i);
    return b;
}

// ---- Shapes of the two tables --------------------------------------------------------------
void test_empty_tables() {
    {
        Tables t;                                   // both empty
        const Result r = t.join();
        NOVA_CHECK(r.rows.empty() && r.stats.outer_rows == 0 && r.stats.probes == 0);
    }
    {
        Tables t;                                   // empty Trades
        for (int i = 0; i < 10; ++i) t.instrument(i, sym(i).c_str());
        const Result r = t.join();
        NOVA_CHECK(r.rows.empty() && r.stats.outer_rows == 0);
    }
    {
        Tables t;                                   // empty Instruments
        for (int i = 0; i < 10; ++i) t.trade(i, sym(i).c_str());
        const Result r = t.join();
        NOVA_CHECK(r.rows.empty() && r.stats.outer_rows == 10 && r.stats.unmatched_outer == 10 && r.stats.probes == 10);
    }
    {
        KVStore trades, instruments;                // Instruments WITHOUT an index: nothing can match
        Model tm, im;
        NOVA_CHECK(put(trades, tm, 1, trade_row(1, "SYM001", 1)));
        NOVA_CHECK(put(instruments, im, 1, instrument_row(1, "SYM001", 1)));
        const JoinSnapshots snaps = nova_join::acquire_join_snapshots(trades, instruments);
        const Result r = run_join(trades, instruments, snaps);
        NOVA_CHECK(r.rows.empty() && r.stats.unmatched_outer == 1);
    }
}

void test_one_to_one_one_to_many_and_missing() {
    Tables t;
    // The benchmark's shape: 50 instruments with distinct symbols; many trades per symbol.
    for (int i = 0; i < 50; ++i) t.instrument(i, sym(i).c_str());
    for (long id = 0; id < 500; ++id) t.trade(id * 100000 + id % 7, sym(static_cast<int>(id % 50)).c_str());
    Result r = t.join();
    NOVA_CHECK(r.rows.size() == 500 && r.stats.matched_outer == 500 && r.stats.unmatched_outer == 0);

    // Missing matches: trades whose symbol no instrument has.
    for (long id = 9000; id < 9040; ++id) t.trade(id, "NOSUCH");
    r = t.join();
    NOVA_CHECK(r.rows.size() == 500 && r.stats.unmatched_outer == 40);

    // One-to-many: several instruments share a symbol (duplicate values in the index).
    t.instrument(100, sym(3).c_str(), 2);
    t.instrument(101, sym(3).c_str(), 3);
    t.instrument(102, "NOSUCH", 4);
    t.instrument(103, "NOSUCH", 5);
    r = t.join();
    // 10 trades of SYM003 now match 3 instruments each; 40 NOSUCH trades match 2 each.
    NOVA_CHECK(r.rows.size() == 490 + 10 * 3 + 40 * 2);
    NOVA_CHECK(r.stats.unmatched_outer == 0);

    // Many matches for one probe: more than the join's initial match buffer.
    for (long k = 1000; k < 1150; ++k) t.instrument(k, "CROWDED", static_cast<std::uint64_t>(k));
    t.trade(77777, "CROWDED");
    r = t.join();
    std::size_t crowded = 0;
    for (const Joined& j : r.rows) crowded += std::get<0>(j) == 77777 ? 1u : 0u;
    NOVA_CHECK(crowded == 150);

    // More outer rows than the join's initial key buffer.
    for (long id = 20000; id < 23000; ++id) t.trade(id, sym(static_cast<int>(id % 50)).c_str());
    r = t.join();
    NOVA_CHECK(r.stats.outer_rows == t.tm.size() && r.stats.outer_rows > 3000);
}

// Records that are not exactly a Trade / an Instrument are skipped and counted.
void test_records_of_the_wrong_size() {
    Tables t;
    t.instrument(1, "AAA");
    t.trade(1, "AAA");
    Bytes short_trade = trade_row(2, "AAA", 1);
    short_trade.resize(sizeof(Trade) - 1);
    NOVA_CHECK(put(t.trades, t.tm, 2, short_trade));
    Bytes long_trade = trade_row(3, "AAA", 1);
    long_trade.resize(sizeof(Trade) + 5, 0x33);
    NOVA_CHECK(put(t.trades, t.tm, 3, long_trade));
    NOVA_CHECK(put(t.trades, t.tm, 4, Bytes()));
    Bytes long_ins = instrument_row(2, "AAA", 1);
    long_ins.resize(sizeof(Instrument) + 4, 0x44);       // indexed (long enough), but not an Instrument record
    NOVA_CHECK(put(t.instruments, t.im, 2, long_ins));
    const Result r = t.join();
    NOVA_CHECK(r.rows.size() == 1);
    NOVA_CHECK(r.stats.outer_skipped == 3 && r.stats.inner_skipped == 1);
}

// ---- Changes between joins: overwrites and deletions in both tables --------------------------
void test_updates_and_deletes_between_joins() {
    Tables t;
    for (int i = 0; i < 20; ++i) t.instrument(i, sym(i).c_str());
    for (long id = 0; id < 200; ++id) t.trade(id, sym(static_cast<int>(id % 20)).c_str());
    NOVA_CHECK(t.join().rows.size() == 200);

    for (long id = 0; id < 200; id += 3) t.trade(id, sym(static_cast<int>((id + 1) % 20)).c_str(), 2);   // trade overwrites: new symbol
    t.join();
    for (long id = 0; id < 200; id += 5) t.trade(id, sym(static_cast<int>(id % 20)).c_str(), 3);         // same symbol, new payload
    t.join();

    for (int i = 0; i < 20; i += 2) t.instrument(i, sym(i + 100).c_str(), 4);   // instrument overwrites: symbol changes
    Result r = t.join();
    NOVA_CHECK(r.stats.unmatched_outer > 0);                                    // trades of the renamed symbols lost their match
    for (int i = 1; i < 20; i += 2) t.instrument(i, sym(i).c_str(), 5);         // same symbol, new payload
    t.join();

    for (long id = 0; id < 200; id += 4) {                                      // trade deletions
        NOVA_CHECK(t.trades.erase(id));
        t.tm.erase(id);
    }
    t.join();
    for (int i = 1; i < 20; i += 4) {                                           // instrument deletions
        NOVA_CHECK(t.instruments.erase(i));
        t.im.erase(i);
    }
    r = t.join();
    for (const Joined& j : r.rows) NOVA_CHECK(t.im.count(std::get<2>(j)) == 1); // no deleted instrument is ever joined

    for (int i = 1; i < 20; i += 4) t.instrument(i, sym(i).c_str(), 6);         // deleted keys come back
    {
        Transaction txn(t.instruments);                                         // and a transaction on the inner table
        for (int i = 0; i < 20; i += 2) {
            const Bytes v = instrument_row(i, sym(i).c_str(), 7);
            txn.put(i, v.data(), v.size());
            t.im[i] = v;
        }
        NOVA_CHECK(txn.commit());
    }
    NOVA_CHECK(t.join().stats.unmatched_outer == 0);
    NOVA_CHECK(NovaKvInspector::settle(t.trades) && NovaKvInspector::settle(t.instruments));
    check_valid(t.trades);
    check_valid(t.instruments);
}

// ---- The join's two snapshots: exactly two, held to the end, old as they may be ----------------
// The writer changes BOTH tables in the middle of the join (from the join's own
// emit callback, so the interleaving is exact): rows already emitted and rows
// still to come must all be from the state at acquisition.
void test_writes_during_the_join_are_not_seen() {
    Tables t;
    for (int i = 0; i < 30; ++i) t.instrument(i, sym(i).c_str());
    for (long id = 0; id < 300; ++id) t.trade(id, sym(static_cast<int>(id % 30)).c_str());
    const Model tm0 = t.tm, im0 = t.im;
    const std::vector<Joined> want = oracle(tm0, im0);

    const JoinSnapshots snaps = nova_join::acquire_join_snapshots(t.trades, t.instruments);
    const std::uint64_t slots_t = NovaKvInspector::stats(t.trades).reader_slots_owned;
    const std::uint64_t slots_i = NovaKvInspector::stats(t.instruments).reader_slots_owned;
    bool wrote = false;
    const Result r = run_join(t.trades, t.instruments, snaps, [&](std::size_t emitted) {
        // The join itself holds no other protection than the two handles, at any point.
        NOVA_CHECK(NovaKvInspector::stats(t.trades).reader_slots_owned == slots_t);
        NOVA_CHECK(NovaKvInspector::stats(t.instruments).reader_slots_owned == slots_i);
        if (emitted != 100 || wrote) return;
        wrote = true;
        // Index updates, overwrites, deletions and inserts, in both tables, mid-join.
        for (int i = 0; i < 30; ++i) t.instrument(i, sym((i + 7) % 30).c_str(), 9);     // every symbol moves
        for (int i = 0; i < 30; i += 3) {
            NOVA_CHECK(t.instruments.erase(i));
            t.im.erase(i);
        }
        for (long id = 0; id < 300; id += 2) t.trade(id, "CHANGED", 9);
        for (long id = 1; id < 300; id += 10) {
            NOVA_CHECK(t.trades.erase(id));
            t.tm.erase(id);
        }
        for (long id = 1000; id < 1100; ++id) t.trade(id, sym(static_cast<int>(id % 30)).c_str(), 9);
        t.instrument(500, "CHANGED", 9);
    });
    NOVA_CHECK(wrote);
    NOVA_CHECK(r.rows == want);                                    // exactly the state at acquisition
    NOVA_CHECK(r.rows == snapshot_oracle(t.trades, t.instruments, snaps));
    NOVA_CHECK(r.rows_self_valid);
    // A join started now sees the new state.
    const Result later = t.join();
    NOVA_CHECK(later.rows != want);
}

// The snapshots are taken, then thousands of commits and reclamation attempts
// happen in both stores, and only then does the join run.
void test_long_lived_join_snapshots_and_reclamation() {
    NovaKvInspector::set_default_rcu_options(0, 1);          // manual passes
    {
        Tables t;
        for (int i = 0; i < 40; ++i) t.instrument(i, sym(i).c_str());
        for (long id = 0; id < 400; ++id) t.trade(id, sym(static_cast<int>(id % 40)).c_str());
        NovaKvInspector::settle(t.trades);
        NovaKvInspector::settle(t.instruments);
        const std::vector<Joined> want = oracle(t.tm, t.im);

        JoinSnapshots snaps = nova_join::acquire_join_snapshots(t.trades, t.instruments);
        const NovaKvStats t0 = NovaKvInspector::stats(t.trades), i0 = NovaKvInspector::stats(t.instruments);
        for (std::uint64_t g = 2; g < 3002; ++g) {
            const long id = static_cast<long>(g % 400);
            t.trade(id, sym(static_cast<int>((g * 7) % 40)).c_str(), g);
            // (g / 40 makes an instrument's symbol differ from round to round, so
            // every one of these writes changes the index.)
            t.instrument(static_cast<long>(g % 40), sym(static_cast<int>((g * 3 + g / 40) % 40)).c_str(), g);
            if (g % 500 == 0) {
                NovaKvInspector::settle(t.trades);
                NovaKvInspector::settle(t.instruments);
            }
        }
        NovaKvInspector::settle(t.trades);
        NovaKvInspector::settle(t.instruments);
        const NovaKvStats t1 = NovaKvInspector::stats(t.trades), i1 = NovaKvInspector::stats(t.instruments);
        // Nothing either snapshot can reach was reclaimed: versions, index
        // segments, chunks, chunk IDs.
        NOVA_CHECK(t1.versions_reclaimed == t0.versions_reclaimed && i1.versions_reclaimed == i0.versions_reclaimed);
        NOVA_CHECK(i1.index_segments_released == i0.index_segments_released);
        NOVA_CHECK(t1.chunks_freed == t0.chunks_freed && i1.chunks_freed == i0.chunks_freed);
        NOVA_CHECK(t1.chunk_ids_reused == t0.chunk_ids_reused && i1.chunk_ids_reused == i0.chunk_ids_reused);
        NOVA_CHECK(NovaKvInspector::backlog(t.trades) >= 3000 && NovaKvInspector::backlog(t.instruments) >= 3000);

        // The join runs now, 3000 commits per store later, with reclamation
        // being attempted during it as well.
        const Result r = run_join(t.trades, t.instruments, snaps, [&](std::size_t emitted) {
            if (emitted % 100 == 0) {
                NovaKvInspector::settle(t.trades);
                NovaKvInspector::settle(t.instruments);
            }
        });
        NOVA_CHECK(r.rows == want);
        NOVA_CHECK(r.rows == snapshot_oracle(t.trades, t.instruments, snaps));
        NOVA_CHECK(r.rows_self_valid && r.rows.size() == 400);

        // After the join has finished and released its two handles, everything
        // is reclaimed, and only then are chunk IDs reused.
        {
            JoinSnapshots done = std::move(snaps);
        }
        NOVA_CHECK(NovaKvInspector::settle(t.trades) && NovaKvInspector::settle(t.instruments));
        const NovaKvStats t2 = NovaKvInspector::stats(t.trades), i2 = NovaKvInspector::stats(t.instruments);
        NOVA_CHECK(t2.versions_reclaimed == t2.versions_retired && i2.versions_reclaimed == i2.versions_retired);
        NOVA_CHECK(NovaKvInspector::backlog(t.trades) == 0 && NovaKvInspector::backlog(t.instruments) == 0);
        NOVA_CHECK(i2.index_segments_released > i1.index_segments_released + 1000);
        NOVA_CHECK(t2.reader_slots_owned == 0 && i2.reader_slots_owned == 0);
        // 3000 Trade writes are 96 KB: the old chunks are free now; make the
        // writer need new ones and see the IDs come back.
        for (std::uint64_t g = 0; g < 4000; ++g) t.trade(static_cast<long>(g % 400), sym(1).c_str(), 5000 + g);
        NOVA_CHECK(NovaKvInspector::settle(t.trades));
        NOVA_CHECK(NovaKvInspector::stats(t.trades).chunks_freed > t1.chunks_freed);
        for (std::uint64_t g = 0; g < 4000; ++g) t.trade(static_cast<long>(g % 400), sym(2).c_str(), 9000 + g);
        NOVA_CHECK(NovaKvInspector::stats(t.trades).chunk_ids_reused > t1.chunk_ids_reused);
        t.join();
        check_valid(t.trades);
        check_valid(t.instruments);
        std::printf("  long-lived join snapshots: 3000 commits per store before the join ran; "
                    "0 versions, 0 index segments, 0 chunks reclaimed meanwhile; %llu index segments released afterwards\n",
                    static_cast<unsigned long long>(i2.index_segments_released - i1.index_segments_released));
    }
    NovaKvInspector::set_default_rcu_options(0, 2);
}

// ---- A writer running concurrently with the joins ---------------------------------------------
// The writer rewrites the WHOLE Instruments table in one transaction per
// generation (every instrument's tick_size = generation, and symbols rotate, so
// the index changes every time), and the whole Trades table likewise (price =
// generation). Under one snapshot per store, a join must therefore see:
//   - all instruments of ONE generation, and an index consistent with them;
//   - all trades of ONE generation;
// but not necessarily the SAME generation in both tables.
void test_join_with_concurrent_writer() {
    NovaKvInspector::set_default_rcu_options(0, 0);          // background cleaner
    {
        KVStore trades, instruments;
        NOVA_CHECK(instruments.create_index(offsetof(Instrument, symbol), sizeof(Instrument::symbol)));
        const int kInstruments = 24, kTrades = 120;
        auto write_generation = [&](std::uint64_t g) {
            bool ok = true;
            {
                Transaction txn(instruments);
                for (int i = 0; i < kInstruments; ++i) {
                    const Bytes v = instrument_row(i, sym(static_cast<int>((static_cast<std::uint64_t>(i) + g) % kInstruments)).c_str(), g);
                    txn.put(i, v.data(), v.size());
                }
                ok = txn.commit() && ok;
            }
            {
                Transaction txn(trades);
                for (long id = 0; id < kTrades; ++id) {
                    const Bytes v = trade_row(id, sym(static_cast<int>((static_cast<std::uint64_t>(id) * 5 + g) % kInstruments)).c_str(), g);
                    txn.put(id, v.data(), v.size());
                }
                ok = txn.commit() && ok;
            }
            return ok;
        };
        NOVA_CHECK(write_generation(1));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> generations{1};
        bool writes_ok = true;
        std::thread writer([&]() {
            for (std::uint64_t g = 2; !stop.load(); ++g) {
                writes_ok = write_generation(g) && writes_ok;
                generations.store(g);
            }
        });

        std::uint64_t joins = 0, wrong = 0, mixed_inner = 0, mixed_outer = 0, different_instants = 0, rows = 0;
        std::uint64_t last_inner = 0, last_outer = 0, backwards = 0;
        while (joins < 400 || generations.load() < 200) {
            const JoinSnapshots snaps = nova_join::acquire_join_snapshots(trades, instruments);
            const Result r = run_join(trades, instruments, snaps);
            // Complete and correct for the two pinned versions (index-free oracle).
            if (r.rows != snapshot_oracle(trades, instruments, snaps)) ++wrong;
            if (!r.rows_self_valid) ++wrong;
            if (r.rows.size() != static_cast<std::size_t>(kTrades)) ++wrong;   // every trade's symbol exists exactly once
            // Each table internally consistent: one generation per table.
            std::uint64_t inner_gen = 0, outer_gen = 0;
            bool first = true;
            for (const Joined& j : r.rows) {
                Trade t;
                Instrument ins;
                std::memcpy(&t, std::get<1>(j).data(), sizeof(t));
                std::memcpy(&ins, std::get<3>(j).data(), sizeof(ins));
                const std::uint64_t tg = static_cast<std::uint64_t>(t.price), ig = static_cast<std::uint64_t>(ins.tick_size);
                if (first) {
                    outer_gen = tg;
                    inner_gen = ig;
                    first = false;
                }
                if (tg != outer_gen) ++mixed_outer;
                if (ig != inner_gen) ++mixed_inner;
            }
            if (inner_gen < last_inner || outer_gen < last_outer) ++backwards;
            last_inner = inner_gen;
            last_outer = outer_gen;
            // NOT an error: the two snapshots are two instants.
            if (inner_gen != outer_gen) ++different_instants;
            rows += r.rows.size();
            ++joins;
        }
        stop.store(true);
        writer.join();
        NOVA_CHECK(writes_ok);
        NOVA_CHECK(wrong == 0);
        NOVA_CHECK(mixed_inner == 0);    // Instruments: one version per join
        NOVA_CHECK(mixed_outer == 0);    // Trades: one version per join
        NOVA_CHECK(backwards == 0);
        NOVA_CHECK(NovaKvInspector::settle(trades) && NovaKvInspector::settle(instruments));
        NOVA_CHECK(NovaKvInspector::stats(trades).reader_slots_owned == 0);
        check_valid(trades);
        check_valid(instruments);
        std::printf("  concurrent writer: %llu joins over %llu generations, %llu joined rows, 0 wrong, 0 mixed versions "
                    "within a table; joins whose two tables were of different generations: %llu (allowed: one snapshot per store)\n",
                    static_cast<unsigned long long>(joins), static_cast<unsigned long long>(generations.load()),
                    static_cast<unsigned long long>(rows), static_cast<unsigned long long>(different_instants));
    }
    NovaKvInspector::set_default_rcu_options(0, 2);
}

}  // namespace

int main() {
    if (!NovaKvInspector::is_versioned()) return nova_test::finish("join_test");  // needs snapshots and the index
    NovaKvInspector::set_default_rcu_options(0, 2);  // single-threaded parts: inline reclamation passes

    test_empty_tables();
    test_one_to_one_one_to_many_and_missing();
    test_records_of_the_wrong_size();
    test_updates_and_deletes_between_joins();
    test_writes_during_the_join_are_not_seen();
    test_long_lived_join_snapshots_and_reclamation();
    test_join_with_concurrent_writer();
    return nova_test::finish("join_test");
}
