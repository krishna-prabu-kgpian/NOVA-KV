// workload_gen.cpp
//
// Reference implementation of the workload driver described in Appendix A
// of the NOVA-KV assignment. It only calls the KVStore / Transaction API
// from kvstore.h, so it links unchanged against every store you build in
// Parts A, B, and C.
//
// Build with -std=c++17 or later.

#include "workload_gen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

struct Trade      { long trade_id; char symbol[8]; double price; long ts; };
struct Instrument { char symbol[8]; char venue[8]; int tick_size; };

Trade make_trade(int i, int j) {
    Trade t;
    t.trade_id = static_cast<long>(i) * 100000 + j;
    std::snprintf(t.symbol, sizeof(t.symbol), "SYM%03d", (i + j) % 50);
    t.price = 100.0 + (i % 97) * 0.37;
    t.ts    = std::chrono::steady_clock::now().time_since_epoch().count();
    return t;
}

// Loads m*n trades, collecting every hot_every-th trade id as "hot".
void run_writer(KVStore& trades, const WorkloadConfig& cfg, std::vector<long>& hot_ids) {
    long count = 0;
    std::optional<Transaction> txn;
    if (cfg.use_txn) txn.emplace(trades);

    for (int i = 0; i < cfg.m; i++) {
        for (int j = 0; j < cfg.n; j++) {
            Trade t = make_trade(i, j);
            count++;

            if (cfg.use_txn) {
                txn->put(t.trade_id, &t, sizeof(t));
                if (count % cfg.txn_size == 0) {
                    txn->commit();
                    txn.emplace(trades);
                }
            } else {
                trades.put(t.trade_id, &t, sizeof(t));
            }
            if (count % cfg.hot_every == 0) hot_ids.push_back(t.trade_id);
        }
    }
    if (cfg.use_txn) txn->commit();
}

// Point-gets a mix of hot and random ids until told to stop, recording each
// call's latency into this thread's own vector, never a shared one.
void run_reader(KVStore& trades, KVStore* instruments, const WorkloadConfig& cfg,
                 const std::vector<long>& hot_ids, const std::atomic<bool>& stop,
                 std::vector<double>& latencies_us) {
    // Local, not shared: std::rand() has a single global state that glibc
    // serializes internally, so calling it from every reader thread turns
    // it into a hidden lock on the hot path. A private generator per
    // thread removes that bottleneck entirely.
    std::mt19937 rng(std::random_device{}());
    Trade t;
    while (!stop.load(std::memory_order_relaxed)) {
        long id = (!hot_ids.empty() && rng() % 10 < 8)
                    ? hot_ids[rng() % hot_ids.size()]
                    : static_cast<long>(rng() % (cfg.m * cfg.n));

        auto start = std::chrono::steady_clock::now();
        bool found = trades.get(id, &t, sizeof(t));
        if (found && cfg.use_join) {
            long match_ids[8];
            instruments->index_get(t.symbol, sizeof(t.symbol), match_ids, 8);
        }
        std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
        latencies_us.push_back(elapsed.count());
    }
}

void load_instruments(KVStore& instruments) {
    instruments.create_index(offsetof(Instrument, symbol), sizeof(Instrument::symbol));
    for (int s = 0; s < 50; s++) {
        Instrument ins{};
        std::snprintf(ins.symbol, sizeof(ins.symbol), "SYM%03d", s);
        std::snprintf(ins.venue, sizeof(ins.venue), "NSE");
        ins.tick_size = 1;
        instruments.put(s, &ins, sizeof(ins));
    }
}

}  // namespace

void workload_run(KVStore& trades, KVStore* instruments,
                   const WorkloadConfig& cfg, WorkloadResults& out) {
    std::vector<long> hot_ids;
    run_writer(trades, cfg, hot_ids);

    if (instruments && cfg.use_join) load_instruments(*instruments);

    std::atomic<bool> stop{false};
    std::vector<std::vector<double>> per_thread(cfg.num_readers);
    std::vector<std::thread> threads;
    for (int i = 0; i < cfg.num_readers; i++) {
        threads.emplace_back(run_reader, std::ref(trades), instruments, std::cref(cfg),
                              std::cref(hot_ids), std::cref(stop), std::ref(per_thread[i]));
    }

    std::this_thread::sleep_for(std::chrono::seconds(cfg.duration_sec));
    stop = true;
    for (auto& th : threads) th.join();

    // Merge and sort only after every reader has stopped, so this
    // bookkeeping never competes with the benchmark itself.
    std::vector<double> all;
    for (auto& v : per_thread) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    if (all.empty()) {
        out.total_ops = 0;
        out.rps = 0;
        out.p50_us = out.p99_us = out.p999_us = 0;
        return;
    }

    auto percentile = [&](double p) {
        size_t idx = std::min(all.size() - 1, static_cast<size_t>(all.size() * p));
        return all[idx];
    };

    out.total_ops = static_cast<long>(all.size());
    out.rps       = static_cast<double>(all.size()) / cfg.duration_sec;
    out.p50_us    = percentile(0.50);
    out.p99_us    = percentile(0.99);
    out.p999_us   = percentile(0.999);
}

void print_results(const std::string& label, const WorkloadResults& r) {
    std::printf("%-14s ops=%-9ld rps=%-10.0f p50=%-7.1fus p99=%-7.1fus p999=%-7.1fus\n",
                label.c_str(), r.total_ops, r.rps, r.p50_us, r.p99_us, r.p999_us);
}
