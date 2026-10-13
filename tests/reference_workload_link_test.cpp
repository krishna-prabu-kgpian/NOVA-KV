// reference_workload_link_test.cpp -- proves the UNMODIFIED supplied
// workload_gen.cpp compiles and links against this repository's kvstore.h and
// store library, and that every code path it contains (plain puts, Transaction
// batching, instrument index probes, reader threads) executes to completion.
//
// This is a compatibility check, not a benchmark: the configuration is tiny and
// the numbers it prints are meaningless. It also compiles the driver shape
// printed at the end of Appendix A.2.
#include "workload_gen.h"

#include "build_info.h"
#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

WorkloadConfig tiny_config() {
    WorkloadConfig cfg;
    cfg.m = 20;
    cfg.n = 10;         // 200 trades
    cfg.hot_every = 10; // 20 hot ids
    cfg.num_readers = 2;
    cfg.duration_sec = 1;
    return cfg;
}

// Same layout as the Trade struct private to workload_gen.cpp.
struct Trade {
    long trade_id;
    char symbol[8];
    double price;
    long ts;
};

void run(const std::string& label, const WorkloadConfig& cfg, bool with_instruments) {
    KVStore trades;
    KVStore instruments;
    WorkloadResults res;
    workload_run(trades, with_instruments ? &instruments : nullptr, cfg, res);
    print_results(label, res);
    NOVA_CHECK(res.total_ops > 0);
    NOVA_CHECK(res.rps > 0);
    NOVA_CHECK(res.p50_us <= res.p99_us);
    NOVA_CHECK(res.p99_us <= res.p999_us);

    // The generator's load_instruments() calls create_index() and then inserts
    // 50 instruments. On the rcu build (the one with a secondary index, since
    // milestone C2) each symbol must then be found through the index; the
    // lock-based baselines have no index and find nothing.
    if (with_instruments && cfg.use_join) {
        const bool has_index = std::strcmp(nova_kv_sync_mode_name(), "rcu") == 0;
        int found = 0;
        for (int sidx = 0; sidx < 50; sidx++) {
            char symbol[8] = {};  // zero-padded, as the generator's Instrument{} is
            std::snprintf(symbol, sizeof(symbol), "SYM%03d", sidx);
            long keys[4] = {-1, -1, -1, -1};
            const int n = instruments.index_get(symbol, sizeof(symbol), keys, 4);
            if (n == 1 && keys[0] == sidx) ++found;
        }
        NOVA_CHECK(found == (has_index ? 50 : 0));

        // What the GENERATOR's own probe finds (supplied/workload_gen.cpp,
        // run_reader): it reads a Trade and passes all 8 bytes of Trade::symbol
        // to instruments->index_get(). make_trade() writes only 7 of those bytes
        // ("SYM0xx" and the terminator); the 8th is whatever the stack held and
        // is stored with the trade. The index compares 8 bytes, and every
        // instrument's 8th byte is 0. So the generator's probe matches a trade
        // exactly when that trade's stored 8th symbol byte happens to be 0.
        // That equivalence is asserted; the hit RATE is a property of the
        // supplied program's indeterminate byte and is only reported.
        if (has_index) {
            long trades_seen = 0, eighth_byte_zero = 0, probe_hits = 0, inconsistent = 0;
            for (int i = 0; i < cfg.m; i++) {
                for (int j = 0; j < cfg.n; j++) {
                    const long id = static_cast<long>(i) * 100000 + j;
                    Trade t{};
                    if (!trades.get(id, &t, sizeof(t))) continue;
                    ++trades_seen;
                    long match_ids[8];
                    const int n = instruments.index_get(t.symbol, sizeof(t.symbol), match_ids, 8);
                    const bool zero = t.symbol[7] == 0;
                    if (zero) ++eighth_byte_zero;
                    if (n > 0) ++probe_hits;
                    if ((n == 1) != zero || n > 1) ++inconsistent;
                }
            }
            NOVA_CHECK(trades_seen == static_cast<long>(cfg.m) * cfg.n);
            NOVA_CHECK(inconsistent == 0);
            std::printf("  generator index probe: %ld of %ld stored trades have a zero 8th symbol byte; "
                        "the generator's 8-byte probe matches %ld of them (byte-exact index)\n",
                        eighth_byte_zero, trades_seen, probe_hits);
        }
    }

    // The generator's writer must have left every trade in the store, readable
    // and intact: with plain puts (Part A path) and, since milestone C1, with
    // Transactions of any size (the generator ignores commit()'s result, so
    // this is where a non-working Transaction would show). The instrument
    // index probe (use_join) is Part C.2 and is only link-checked here.
    {
        int wrong = 0;
        for (int i = 0; i < cfg.m; i++) {
            for (int j = 0; j < cfg.n; j++) {
                const long id = static_cast<long>(i) * 100000 + j;
                Trade t{};
                char symbol[8];
                std::snprintf(symbol, sizeof(symbol), "SYM%03d", (i + j) % 50);
                if (!trades.get(id, &t, sizeof(t)) || t.trade_id != id ||
                    std::strcmp(t.symbol, symbol) != 0) {
                    ++wrong;
                }
            }
        }
        NOVA_CHECK(wrong == 0);
        Trade t{};
        NOVA_CHECK(!trades.get(static_cast<long>(cfg.m) * 100000, &t, sizeof(t)));
    }
}

}  // namespace

int main() {
    // Appendix A.2 defaults, as printed in the PDF.
    {
        WorkloadConfig d;
        NOVA_CHECK(d.m == 1000 && d.n == 100);
        NOVA_CHECK(d.hot_every == 100);
        NOVA_CHECK(d.num_readers == 8);
        NOVA_CHECK(d.duration_sec == 5);
        NOVA_CHECK(!d.use_txn);
        NOVA_CHECK(d.txn_size == 1);
        NOVA_CHECK(!d.use_join);
    }

    run("link/put", tiny_config(), false);

    WorkloadConfig txn_join = tiny_config();
    txn_join.use_txn = true;
    txn_join.txn_size = 4;
    txn_join.use_join = true;
    run("link/txn+join", txn_join, true);

    // Transactions of several sizes, including one that does not divide the
    // number of trades (the generator's final commit carries the remainder) and
    // one that does (its final commit is empty).
    for (int size : {1, 7, 16, 200}) {
        WorkloadConfig txn = tiny_config();
        txn.use_txn = true;
        txn.txn_size = size;
        run("link/txn size " + std::to_string(size), txn, false);
    }

    return nova_test::finish("reference_workload_link_test");
}
