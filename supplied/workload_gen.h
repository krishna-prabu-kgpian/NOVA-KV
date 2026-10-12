// workload_gen.h
#pragma once
#include "kvstore.h"
#include <string>

struct WorkloadConfig {
    int m = 1000, n = 100; // m * n trades inserted by the writer
    int hot_every = 100; // 1 in N trades is re-read repeatedly by readers
    int num_readers = 8; // concurrent reader threads
    int duration_sec = 5; // how long readers run once loading is done
    bool use_txn = false; // false: put() per trade, true: batch into Transaction
    int txn_size = 1;     // trades per transaction, used only if use_txn
    bool use_join = false; // false: point gets only,
                           // true: also probe the instrument index
};

struct WorkloadResults {
    long total_ops = 0;
    double rps = 0;
    double p50_us = 0, p99_us = 0, p999_us = 0;
};

void workload_run(KVStore& trades, KVStore* instruments,
                 const WorkloadConfig& cfg, WorkloadResults& out);
void print_results(const std::string& label, const WorkloadResults& r);
