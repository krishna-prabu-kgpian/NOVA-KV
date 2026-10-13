// driver_common.h -- shared by the Part A drivers. Each driver is the program
// shape printed in the assignment's Appendix A.2: a store, a WorkloadConfig,
// workload_run(), print_results(). The store's synchronization policy comes
// from the library the driver is linked with, not from the driver source.
//
// Usage: <driver> [num_readers] [duration_sec] [m] [n] [mode]
// (m * n trades are loaded; defaults are the generator's 1000 and 100)
//
//   mode   put     (default) the generator's defaults: single puts, point gets
//          txn:N   cfg.use_txn with cfg.txn_size = N
//          join    cfg.use_join: the generator also creates the Instruments
//                  store and probes its index after every successful get
//                  (an index PROBE, not a join; see docs/secondary_index.md)
//
// NOTE: the supplied workload generator loads all trades before it starts any
// reader, so these runs are "reference / preload-only": no writer is active
// while the readers are measured.
//
// After the generator has finished, the driver reports two facts about what
// the generator's readers were doing, obtained from the store and not from the
// generator (which is unmodified and reports neither):
//   - how many of the keys its "random" reads draw from actually exist (the
//     generator draws rng() % (m * n), while trade ids are i * 100000 + j, so
//     almost all of those reads miss);
//   - in join mode on a store with an index, how many stored trades the
//     generator's own 8-byte symbol probe matches (decision D-065).
#pragma once

#include "build_info.h"
#include "inspect.h"
#include "workload_gen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// `fixed_readers` > 0 pins the reader count regardless of the command line.
inline int run_driver(const char* expected_variant, int fixed_readers, int argc,
                             char** argv) {
    if (std::strcmp(nova_kv_sync_mode_name(), expected_variant) != 0) {
        std::fprintf(stderr, "%s driver linked against the '%s' store variant\n",
                     expected_variant, nova_kv_sync_mode_name());
        return 2;
    }

    WorkloadConfig cfg;
    if (argc > 1) cfg.num_readers = std::atoi(argv[1]);
    if (argc > 2) cfg.duration_sec = std::atoi(argv[2]);
    if (argc > 3) cfg.m = std::atoi(argv[3]);
    if (argc > 4) cfg.n = std::atoi(argv[4]);
    if (fixed_readers > 0) {
        if (argc > 1 && cfg.num_readers != fixed_readers) {
            std::fprintf(stderr, "%s: reader count is fixed at %d; ignoring %s\n",
                         expected_variant, fixed_readers, argv[1]);
        }
        cfg.num_readers = fixed_readers;
    }
    if (cfg.num_readers < 1 || cfg.duration_sec < 1 || cfg.m < 1 || cfg.n < 1) {
        std::fprintf(stderr, "usage: %s [num_readers >= 1] [duration_sec >= 1] [m >= 1] [n >= 1]\n",
                     argv[0]);
        return 2;
    }

    const char* mode = argc > 5 ? argv[5] : "put";
    KVStore instruments;
    KVStore* instruments_arg = nullptr;
    if (std::strncmp(mode, "txn:", 4) == 0 && std::atoi(mode + 4) >= 1) {
        cfg.use_txn = true;
        cfg.txn_size = std::atoi(mode + 4);
    } else if (std::strcmp(mode, "join") == 0) {
        cfg.use_join = true;
        instruments_arg = &instruments;
    } else if (std::strcmp(mode, "put") != 0) {
        std::fprintf(stderr, "%s: mode must be put, txn:N or join\n", argv[0]);
        return 2;
    }

    KVStore trades;
    WorkloadResults res;
    workload_run(trades, instruments_arg, cfg, res);
    print_results(expected_variant, res);

    // ---- What the generator's reads were (measured from the store) ---------
    // 80 % of its reads go to "hot" ids, which exist. 20 % go to rng() % (m*n).
    struct Row {
        long trade_id;
        char symbol[8];
        double price;
        long ts;
    } row;
    const long space = static_cast<long>(cfg.m) * cfg.n;
    long present = 0;
    for (long id = 0; id < space; ++id) present += trades.get(id, &row, sizeof(row)) ? 1 : 0;
    const double random_hit = static_cast<double>(present) / static_cast<double>(space);
    std::printf("reference_reads: mode=%s random_key_space=%ld present_in_space=%ld random_hit_rate=%.6f "
                "expected_overall_hit_rate=%.6f (80%% hot ids + 20%% random)\n",
                mode, space, present, random_hit, 0.8 + 0.2 * random_hit);
    // Where the store's lock object lies in its cache line: the lock-based
    // stores run at two speeds depending on it (decision D-072). Not controlled
    // here: the generator constructs nothing and its arguments are fixed.
    std::printf("reference_lock: bytes=%zu offset_in_line=%u (not controlled)\n", NovaKvInspector::lock_size(),
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(NovaKvInspector::lock_address(trades)) % 64));
    if (cfg.use_join) {
        long stored = 0, eighth_zero = 0, probe_hits = 0;
        for (int i = 0; i < cfg.m; ++i) {
            for (int j = 0; j < cfg.n; ++j) {
                if (!trades.get(static_cast<long>(i) * 100000 + j, &row, sizeof(row))) continue;
                ++stored;
                eighth_zero += row.symbol[7] == 0 ? 1 : 0;
                long match_ids[8];
                probe_hits += instruments.index_get(row.symbol, sizeof(row.symbol), match_ids, 8) > 0 ? 1 : 0;
            }
        }
        std::printf("generator_index_probe: stored_trades=%ld eighth_symbol_byte_zero=%ld probe_hits=%ld "
                    "probe_hit_rate=%.6f\n",
                    stored, eighth_zero, probe_hits,
                    stored == 0 ? 0.0 : static_cast<double>(probe_hits) / static_cast<double>(stored));
    }
    return 0;
}
