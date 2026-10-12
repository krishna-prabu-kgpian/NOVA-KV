// driver_common.h -- shared by the Part A drivers. Each driver is the program
// shape printed in the assignment's Appendix A.2: a store, a WorkloadConfig,
// workload_run(), print_results(). The store's synchronization policy comes
// from the library the driver is linked with, not from the driver source.
//
// Usage: <driver> [num_readers] [duration_sec] [m] [n]
// (m * n trades are loaded; defaults are the generator's 1000 and 100)
//
// NOTE: the supplied workload generator loads all trades before it starts any
// reader, so these runs are "reference / preload-only": no writer is active
// while the readers are measured.
#pragma once

#include "build_info.h"
#include "workload_gen.h"

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

    KVStore trades;
    WorkloadResults res;
    workload_run(trades, nullptr, cfg, res);
    print_results(expected_variant, res);
    return 0;
}
