// join_bench.cpp -- preliminary Part C.2 measurements on the rcu build: index
// creation, index lookups, index maintenance, indexed transactions, and the
// index-nested-loop join with the writer paused and active.
//
// This is NOT the Part D campaign: it exists to give milestone C2 first
// numbers under recorded conditions. Every experiment validates what it did
// (wrong results make the run fail) and appends one CSV row per measurement.
//
//   join_bench --experiment create|lookup|maintain|txn|join
//              [--trades N] [--instruments N] [--duration SEC] [--threads N]
//              [--writer off|on] [--txn-size N] [--index on|off]
//              [--cpus LIST] [--writer-cpu N] [--csv FILE] [--run-id ID]
//
// Tables are the assignment's: Trades (32-byte records, 50 symbols by default)
// and Instruments (20-byte records, one per symbol), as in the supplied
// workload. The join is benchmarks/join.h: one snapshot per store.
#include "join.h"

#include "build_info.h"
#include "inspect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

namespace {

using Clock = std::chrono::steady_clock;
using nova_join::Instrument;
using nova_join::Trade;

double seconds_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

struct Args {
    std::string experiment = "join";
    long trades = 100000;
    int instruments = 50;
    double duration = 2.0;
    int threads = 1;
    bool writer = false;
    int txn_size = 1;
    bool index = true;
    std::vector<int> cpus;
    int writer_cpu = -1;
    std::string csv;
    std::string run_id = "manual";
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--experiment") a.experiment = val();
        else if (k == "--trades") a.trades = std::atol(val());
        else if (k == "--instruments") a.instruments = std::atoi(val());
        else if (k == "--duration") a.duration = std::atof(val());
        else if (k == "--threads") a.threads = std::atoi(val());
        else if (k == "--writer") a.writer = std::string(val()) == "on";
        else if (k == "--txn-size") a.txn_size = std::atoi(val());
        else if (k == "--index") a.index = std::string(val()) == "on";
        else if (k == "--writer-cpu") a.writer_cpu = std::atoi(val());
        else if (k == "--csv") a.csv = val();
        else if (k == "--run-id") a.run_id = val();
        else if (k == "--cpus") {
            std::string list = val();
            std::size_t pos = 0;
            while (pos < list.size()) {
                const std::size_t comma = list.find(',', pos);
                a.cpus.push_back(std::atoi(list.substr(pos, comma - pos).c_str()));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", k.c_str());
            return false;
        }
    }
    return a.trades > 0 && a.instruments > 0 && a.threads > 0 && a.txn_size > 0 && a.duration > 0;
}

void pin_to(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int cpu_for(const Args& a, int thread) {
    return a.cpus.empty() ? -1 : a.cpus[static_cast<std::size_t>(thread) % a.cpus.size()];
}

void symbol_of(int s, char (&out)[8]) {
    std::memset(out, 0, 8);
    std::snprintf(out, 8, "SYM%03d", s % 1000);
}

Trade make_trade(long id, int symbol, long generation) {
    Trade t;
    std::memset(&t, 0, sizeof(t));
    t.trade_id = id;
    symbol_of(symbol, t.symbol);
    t.price = static_cast<double>(generation);
    t.ts = id * 31 + generation;
    return t;
}

Instrument make_instrument(int s, int generation) {
    Instrument ins;
    std::memset(&ins, 0, sizeof(ins));
    symbol_of(s, ins.symbol);
    std::snprintf(ins.venue, sizeof(ins.venue), "NSE");
    ins.tick_size = generation;
    return ins;
}

// Loads `n` trades (keys 0..n-1, symbol = key % instruments) in transactions of 256.
bool load_trades(KVStore& trades, long n, int instruments) {
    bool ok = true;
    for (long base = 0; base < n; base += 256) {
        Transaction txn(trades);
        for (long id = base; id < base + 256 && id < n; ++id) {
            const Trade t = make_trade(id, static_cast<int>(id % instruments), 1);
            txn.put(id, &t, sizeof(t));
        }
        ok = txn.commit() && ok;
    }
    return ok;
}

bool load_instruments(KVStore& instruments, int n) {
    bool ok = instruments.create_index(offsetof(Instrument, symbol), sizeof(Instrument::symbol));
    for (int s = 0; s < n; ++s) {
        const Instrument ins = make_instrument(s, 1);
        ok = instruments.put(s, &ins, sizeof(ins)) && ok;
    }
    return ok;
}

struct Percentiles {
    double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

Percentiles percentiles(std::vector<double>& v) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(static_cast<double>(v.size()) * q))]; };
    p.p50 = at(0.50);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = v.back();
    return p;
}

// ---- Output -------------------------------------------------------------------------------
struct Row {
    std::string metric;
    double value;
    std::string unit;
    std::string detail;
};

std::vector<Row> g_rows;
bool g_valid = true;

void report(const std::string& metric, double value, const std::string& unit, const std::string& detail = "") {
    g_rows.push_back(Row{metric, value, unit, detail});
    std::printf("  %-34s %14.3f %-10s %s\n", metric.c_str(), value, unit.c_str(), detail.c_str());
}

void require(bool condition, const char* what) {
    if (condition) return;
    g_valid = false;
    std::fprintf(stderr, "VALIDATION FAILED: %s\n", what);
}

void write_csv(const Args& a) {
    if (a.csv.empty()) return;
    std::FILE* probe = std::fopen(a.csv.c_str(), "r");
    const bool exists = probe != nullptr;
    if (probe != nullptr) std::fclose(probe);
    std::FILE* f = std::fopen(a.csv.c_str(), "a");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", a.csv.c_str());
        g_valid = false;
        return;
    }
    if (!exists) {
        std::fputs("run_id,variant,experiment,trades,instruments,threads,writer,txn_size,index,duration_s,metric,value,unit,detail,valid\n", f);
    }
    for (const Row& r : g_rows) {
        std::fprintf(f, "%s,%s,%s,%ld,%d,%d,%s,%d,%s,%.3f,%s,%.6f,%s,%s,%d\n", a.run_id.c_str(), nova_kv_sync_mode_name(),
                     a.experiment.c_str(), a.trades, a.instruments, a.threads, a.writer ? "on" : "off", a.txn_size,
                     a.index ? "on" : "off", a.duration, r.metric.c_str(), r.value, r.unit.c_str(), r.detail.c_str(),
                     g_valid ? 1 : 0);
    }
    std::fclose(f);
}

// ---- create: backfill time ---------------------------------------------------------------------
void experiment_create(const Args& a) {
    {   // Trades indexed by symbol: few distinct values, long posting lists.
        KVStore trades;
        require(load_trades(trades, a.trades, a.instruments), "load");
        NovaKvInspector::settle(trades);
        const std::uint64_t before = NovaKvInspector::memory_held_bytes(trades);
        const auto t0 = Clock::now();
        const bool ok = trades.create_index(offsetof(Trade, symbol), sizeof(Trade::symbol));
        const double s = seconds_since(t0);
        require(ok, "create_index(symbol)");
        NovaKvInspector::settle(trades);
        const NovaKvStats st = NovaKvInspector::stats(trades);
        require(st.index_pairs == static_cast<std::uint64_t>(a.trades), "every trade indexed");
        report("backfill_ms", s * 1e3, "ms", "trades by symbol");
        report("backfill_ns_per_row", s * 1e9 / static_cast<double>(a.trades), "ns/row", "trades by symbol");
        report("index_bytes", static_cast<double>(NovaKvInspector::memory_held_bytes(trades) - before), "bytes", "trades by symbol");
    }
    {   // Trades indexed by trade_id bytes: every value distinct.
        KVStore trades;
        require(load_trades(trades, a.trades, a.instruments), "load");
        NovaKvInspector::settle(trades);
        const std::uint64_t before = NovaKvInspector::memory_held_bytes(trades);
        const auto t0 = Clock::now();
        const bool ok = trades.create_index(offsetof(Trade, trade_id), sizeof(Trade::trade_id));
        const double s = seconds_since(t0);
        require(ok, "create_index(trade_id)");
        NovaKvInspector::settle(trades);
        long key = -1;
        const long probe = a.trades / 2;
        require(trades.index_get(&probe, sizeof(probe), &key, 1) == 1 && key == probe, "unique lookup");
        report("backfill_ms", s * 1e3, "ms", "trades by trade_id (all distinct)");
        report("backfill_ns_per_row", s * 1e9 / static_cast<double>(a.trades), "ns/row", "trades by trade_id (all distinct)");
        report("index_bytes", static_cast<double>(NovaKvInspector::memory_held_bytes(trades) - before), "bytes", "trades by trade_id (all distinct)");
    }
}

// ---- lookup: index_get throughput on the Instruments table -------------------------------------
void experiment_lookup(const Args& a) {
    KVStore instruments;
    require(load_instruments(instruments, a.instruments), "load");
    NovaKvInspector::settle(instruments);
    std::atomic<bool> go{false}, stop{false};
    std::atomic<std::uint64_t> total{0}, wrong{0}, writes{0};
    std::vector<std::vector<double>> samples(static_cast<std::size_t>(a.threads));
    std::vector<std::thread> readers;
    for (int t = 0; t < a.threads; ++t) {
        readers.emplace_back([&, t]() {
            pin_to(cpu_for(a, t));
            std::uint64_t x = 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(t + 1), n = 0, bad = 0;
            std::vector<double>& lat = samples[static_cast<std::size_t>(t)];
            lat.reserve(1 << 20);
            char sym[8];
            long keys[8];
            while (!go.load()) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                const int s = static_cast<int>(x % static_cast<std::uint64_t>(a.instruments));
                symbol_of(s, sym);
                if ((n & 63) == 0 && lat.size() < lat.capacity()) {      // 1 call in 64 is timed
                    const auto t0 = Clock::now();
                    const int m = instruments.index_get(sym, 8, keys, 8);
                    lat.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
                    if (m != 1 || keys[0] != s) ++bad;
                } else {
                    const int m = instruments.index_get(sym, 8, keys, 8);
                    if (m != 1 || keys[0] != s) ++bad;
                }
                ++n;
            }
            total.fetch_add(n);
            wrong.fetch_add(bad);
        });
    }
    std::thread writer;
    if (a.writer) {
        // Rewrites instruments (payload only: every symbol keeps its key) as fast as it can.
        writer = std::thread([&]() {
            pin_to(a.writer_cpu);
            std::uint64_t n = 0;
            int g = 2;
            while (!go.load()) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                const int s = static_cast<int>(n % static_cast<std::uint64_t>(a.instruments));
                const Instrument ins = make_instrument(s, g++);
                if (instruments.put(s, &ins, sizeof(ins))) ++n;
            }
            writes.store(n);
        });
    }
    const auto t0 = Clock::now();
    go.store(true);
    std::this_thread::sleep_for(std::chrono::duration<double>(a.duration));
    stop.store(true);
    for (auto& r : readers) r.join();
    const double s = seconds_since(t0);
    if (writer.joinable()) writer.join();
    require(wrong.load() == 0, "every lookup returned exactly its instrument");
    std::vector<double> all;
    for (auto& v : samples) all.insert(all.end(), v.begin(), v.end());
    const Percentiles p = percentiles(all);
    report("index_get_mops", static_cast<double>(total.load()) / s / 1e6, "M/s");
    report("index_get_p50_us", p.p50, "us");
    report("index_get_p99_us", p.p99, "us");
    report("index_get_p999_us", p.p999, "us");
    report("index_get_max_us", p.max, "us");
    if (a.writer) report("writer_puts_per_s", static_cast<double>(writes.load()) / s, "1/s");
}

// ---- maintain: what the index costs a single-key writer -------------------------------------------
void experiment_maintain(const Args& a) {
    for (int mode = 0; mode < 3; ++mode) {
        // 0: no index. 1: index, the overwrite keeps the symbol. 2: index, the overwrite changes it.
        KVStore trades;
        require(load_trades(trades, a.trades, a.instruments), "load");
        if (mode != 0) require(trades.create_index(offsetof(Trade, symbol), sizeof(Trade::symbol)), "create_index");
        NovaKvInspector::settle(trades);
        const NovaKvStats st0 = NovaKvInspector::stats(trades);
        pin_to(a.writer_cpu);
        std::uint64_t n = 0;
        long generation = 2;
        const auto t0 = Clock::now();
        while (seconds_since(t0) < a.duration) {
            for (int burst = 0; burst < 256; ++burst) {
                const long id = static_cast<long>((n * 7919) % static_cast<std::uint64_t>(a.trades));
                const int symbol = static_cast<int>((mode == 2 ? id + generation : id) % a.instruments);
                const Trade t = make_trade(id, symbol, generation);
                if (!trades.put(id, &t, sizeof(t))) g_valid = false;
                ++n;
            }
            ++generation;
        }
        const double s = seconds_since(t0);
        NovaKvInspector::settle(trades);
        const NovaKvStats st1 = NovaKvInspector::stats(trades);
        require(NovaKvInspector::validate(trades) == nullptr, "store valid after maintenance");
        const char* label = mode == 0 ? "no index" : (mode == 1 ? "index, field unchanged" : "index, field changes");
        report("put_per_s", static_cast<double>(n) / s, "1/s", label);
        report("index_segments_per_put", static_cast<double>(st1.index_segments_built - st0.index_segments_built) / static_cast<double>(n), "1/put", label);
        report("index_bytes_per_put", static_cast<double>(st1.index_segment_bytes_built - st0.index_segment_bytes_built) / static_cast<double>(n), "bytes/put", label);
    }
}

// ---- txn: commit cost by transaction size, with and without the index ------------------------------
void experiment_txn(const Args& a) {
    KVStore trades;
    require(load_trades(trades, a.trades, a.instruments), "load");
    if (a.index) require(trades.create_index(offsetof(Trade, symbol), sizeof(Trade::symbol)), "create_index");
    NovaKvInspector::settle(trades);
    pin_to(a.writer_cpu);
    std::uint64_t commits = 0, n = 0;
    long generation = 2;
    std::vector<double> lat;
    lat.reserve(1 << 20);
    const auto t0 = Clock::now();
    while (seconds_since(t0) < a.duration) {
        for (int burst = 0; burst < 64; ++burst) {
            const auto c0 = Clock::now();
            Transaction txn(trades);
            for (int i = 0; i < a.txn_size; ++i) {
                const long id = static_cast<long>((n * 7919) % static_cast<std::uint64_t>(a.trades));
                const Trade t = make_trade(id, static_cast<int>((id + generation) % a.instruments), generation);
                txn.put(id, &t, sizeof(t));
                ++n;
            }
            if (!txn.commit()) g_valid = false;
            if (lat.size() < lat.capacity()) lat.push_back(std::chrono::duration<double, std::micro>(Clock::now() - c0).count());
            ++commits;
        }
        ++generation;
    }
    const double s = seconds_since(t0);
    NovaKvInspector::settle(trades);
    require(NovaKvInspector::validate(trades) == nullptr, "store valid after transactions");
    const Percentiles p = percentiles(lat);
    report("commits_per_s", static_cast<double>(commits) / s, "1/s");
    report("rows_per_s", static_cast<double>(n) / s, "1/s");
    report("commit_p50_us", p.p50, "us");
    report("commit_p99_us", p.p99, "us");
    report("commit_max_us", p.max, "us");
}

// ---- join: the index-nested-loop join, writer paused or active -------------------------------------
void experiment_join(const Args& a) {
    KVStore trades, instruments;
    require(load_trades(trades, a.trades, a.instruments), "load trades");
    require(load_instruments(instruments, a.instruments), "load instruments");
    NovaKvInspector::settle(trades);
    NovaKvInspector::settle(instruments);
    const std::uint64_t held0 = NovaKvInspector::memory_held_bytes(trades) + NovaKvInspector::memory_held_bytes(instruments);
    const nova_join::JoinSpec spec = nova_join::trade_instrument_spec();
    std::atomic<bool> go{false}, stop{false};
    std::atomic<std::uint64_t> joins{0}, rows{0}, wrong{0};
    std::atomic<std::uint64_t> trade_writes{0}, instrument_commits{0}, held_max{0}, backlog_max{0};
    std::vector<std::vector<double>> samples(static_cast<std::size_t>(a.threads));
    std::vector<std::thread> joiners;
    for (int t = 0; t < a.threads; ++t) {
        joiners.emplace_back([&, t]() {
            pin_to(cpu_for(a, t));
            nova_join::JoinBuffers buf;
            std::uint64_t n = 0, r = 0, bad = 0;
            while (!go.load()) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                const auto t0 = Clock::now();
                // One snapshot per store, both taken here and nowhere else.
                const nova_join::JoinSnapshots snaps = nova_join::acquire_join_snapshots(trades, instruments);
                std::uint64_t mismatched = 0;
                long inner_generation = -1;
                const nova_join::JoinStats st = nova_join::index_nested_loop_join(
                    trades, instruments, snaps, spec, buf,
                    [&](long, const unsigned char* trow, long, const unsigned char* irow) {
                        // The joined pair really shares the join field, and the
                        // inner table is of one version (one tick_size).
                        if (std::memcmp(trow + offsetof(Trade, symbol), irow, 8) != 0) ++mismatched;
                        Instrument ins;
                        std::memcpy(&ins, irow, sizeof(ins));
                        if (inner_generation < 0) inner_generation = ins.tick_size;
                        if (ins.tick_size != inner_generation) ++mismatched;
                    });
                samples[static_cast<std::size_t>(t)].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
                // Every trade's symbol exists exactly once among the instruments.
                if (mismatched != 0 || st.joined_rows != static_cast<std::size_t>(a.trades) || st.outer_skipped != 0) ++bad;
                r += st.joined_rows;
                ++n;
            }
            joins.fetch_add(n);
            rows.fetch_add(r);
            wrong.fetch_add(bad);
        });
    }
    std::thread writer;
    if (a.writer) {
        // Trades: single puts that change the symbol. Instruments: every 64th
        // step, ONE transaction rewriting the whole table (tick_size = generation),
        // which the join checks for (one tick_size per join).
        writer = std::thread([&]() {
            pin_to(a.writer_cpu);
            std::uint64_t n = 0, commits = 0, held = 0, backlog = 0;
            int generation = 2;
            while (!go.load()) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                const long id = static_cast<long>((n * 7919) % static_cast<std::uint64_t>(a.trades));
                const Trade t = make_trade(id, static_cast<int>((id + generation) % a.instruments), generation);
                if (trades.put(id, &t, sizeof(t))) ++n;
                if (n % 64 == 0) {
                    Transaction txn(instruments);
                    for (int s = 0; s < a.instruments; ++s) {
                        const Instrument ins = make_instrument(s, generation);
                        txn.put(s, &ins, sizeof(ins));
                    }
                    if (txn.commit()) ++commits;
                    ++generation;
                    held = std::max(held, NovaKvInspector::memory_held_bytes(trades) + NovaKvInspector::memory_held_bytes(instruments));
                    backlog = std::max(backlog, NovaKvInspector::backlog(trades) + NovaKvInspector::backlog(instruments));
                }
            }
            trade_writes.store(n);
            instrument_commits.store(commits);
            held_max.store(held);
            backlog_max.store(backlog);
        });
    }
    const auto t0 = Clock::now();
    go.store(true);
    std::this_thread::sleep_for(std::chrono::duration<double>(a.duration));
    stop.store(true);
    for (auto& j : joiners) j.join();
    const double s = seconds_since(t0);
    if (writer.joinable()) writer.join();
    require(wrong.load() == 0, "every join complete, pairs matching, inner table of one version");
    require(joins.load() > 0, "at least one join finished");
    std::vector<double> all;
    for (auto& v : samples) all.insert(all.end(), v.begin(), v.end());
    const Percentiles p = percentiles(all);
    report("joins_per_s", static_cast<double>(joins.load()) / s, "1/s");
    report("joined_rows_per_s", static_cast<double>(rows.load()) / s / 1e6, "M/s");
    report("join_p50_ms", p.p50, "ms");
    report("join_p99_ms", p.p99, "ms");
    report("join_max_ms", p.max, "ms");
    report("joins_measured", static_cast<double>(all.size()), "joins");
    if (a.writer) {
        report("writer_trade_puts_per_s", static_cast<double>(trade_writes.load()) / s, "1/s");
        report("writer_instrument_txns_per_s", static_cast<double>(instrument_commits.load()) / s, "1/s");
        report("memory_held_max_mib", static_cast<double>(held_max.load()) / 1048576.0, "MiB");
        report("backlog_max_versions", static_cast<double>(backlog_max.load()), "versions");
    }
    report("memory_held_idle_mib", static_cast<double>(held0) / 1048576.0, "MiB");
    const bool drained = NovaKvInspector::settle(trades) && NovaKvInspector::settle(instruments);
    require(drained, "backlog drains once the joins have finished");
    const NovaKvStats st = NovaKvInspector::stats(trades), si = NovaKvInspector::stats(instruments);
    report("versions_reclaimed", static_cast<double>(st.versions_reclaimed + si.versions_reclaimed), "versions");
    report("index_segments_reclaimed", static_cast<double>(si.index_segments_released), "segments");
    report("cleaner_cpu_s", static_cast<double>(st.cleaner_cpu_ns + si.cleaner_cpu_ns) / 1e9, "s");
    require(NovaKvInspector::validate(trades) == nullptr && NovaKvInspector::validate(instruments) == nullptr, "stores valid");
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) return 2;
    if (!NovaKvInspector::is_versioned()) {
        std::fprintf(stderr, "join_bench needs the rcu build (snapshots and the secondary index)\n");
        return 2;
    }
    std::printf("join_bench experiment=%s trades=%ld instruments=%d threads=%d writer=%s txn_size=%d index=%s duration=%.1fs\n",
                a.experiment.c_str(), a.trades, a.instruments, a.threads, a.writer ? "on" : "off", a.txn_size,
                a.index ? "on" : "off", a.duration);
    if (a.experiment == "create") experiment_create(a);
    else if (a.experiment == "lookup") experiment_lookup(a);
    else if (a.experiment == "maintain") experiment_maintain(a);
    else if (a.experiment == "txn") experiment_txn(a);
    else if (a.experiment == "join") experiment_join(a);
    else {
        std::fprintf(stderr, "unknown experiment: %s\n", a.experiment.c_str());
        return 2;
    }
    write_csv(a);
    std::printf("valid=%d\n", g_valid ? 1 : 0);
    return g_valid ? 0 : 1;
}
