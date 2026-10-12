// concurrent_bench.cpp -- the project's own point-get benchmark with a writer
// that is ACTIVE while the readers are measured.
//
// Why it exists: the supplied workload_gen.cpp finishes all writes before it
// starts any reader, so it cannot produce the "one background writer
// throughout" measurements Parts A and D ask for. This harness keeps the
// supplied schema, key layout and 80 % hot / 20 % random read mix, and changes
// only what must change:
//   * readers and the writer start together on a barrier and overlap for the
//     whole measured window; the writer overwrites existing trades;
//   * the random 20 % is drawn from keys that exist (--keys hit, default) or
//     with the generator's own formula, which mostly misses (--keys reference);
//   * loading and a warm-up phase are excluded from the measurement;
//   * throughput uses the measured wall-clock window, and every read latency is
//     recorded in a fixed-memory histogram together with the exact maximum.
//
// The store's synchronization policy is whatever library the binary is linked
// with (concurrent_bench_single / _global_lock / _rwlock). The unsynchronized
// `single` variant is only run as ONE thread: either reads only, or with
// --writer inline:N, where the same thread performs one update per N reads.
//
// The run fails (nonzero exit) if a read returns a record that does not belong
// to its key, if the writer was requested but completed no successful update
// inside the measured window, or if no read completed.
#include "kvstore.h"

#include "bench_common.h"
#include "build_info.h"
#include "inspect.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace {

using bench::LatencyHistogram;
using bench::Trade;

enum Phase : int { kWait = 0, kWarmup = 1, kMeasure = 2, kStop = 3 };

struct Config {
    int readers = 8;
    double duration_s = 3.0;
    double warmup_s = 0.5;
    bool writer_thread = true;
    long writer_rate = 0;       // updates per second; 0 = as fast as possible
    long inline_every = 0;      // single variant: one update per this many reads
    bool keys_hit = true;
    int m = 1000, n = 100, hot_every = 100;
    std::uint64_t seed = 1;
    bool pin = true;
    std::vector<int> reader_cpus;
    int writer_cpu = -1;
};

struct alignas(64) ReaderResult {
    LatencyHistogram hist;
    std::uint64_t ops = 0, hits = 0, misses = 0, corrupt = 0, inline_writes = 0;
    bool pinned = true;
};

struct WriterResult {
    LatencyHistogram hist;
    std::uint64_t ok = 0, failed = 0;
    std::uint64_t ok_total = 0;  // including warm-up
    bool pinned = true;
};

Trade make_trade(long id, int i, int j, std::uint64_t version) {
    Trade t;
    t.trade_id = id;
    std::snprintf(t.symbol, sizeof(t.symbol), "SYM%03d", (i + j) % 50);
    t.price = 100.0 + (i % 97) * 0.37 + static_cast<double>(version % 1000) * 0.01;
    t.ts = static_cast<long>(version);
    return t;
}

long key_of(int i, int j) { return static_cast<long>(i) * 100000 + j; }

// noinline: keeps this loop a distinct frame in profiles.
__attribute__((noinline)) void reader_main(const KVStore& store, KVStore* inline_store, const Config& cfg,
                 const std::vector<long>& hot_ids, const std::atomic<int>& phase, int index,
                 ReaderResult& out) {
    if (cfg.pin && index < static_cast<int>(cfg.reader_cpus.size())) {
        out.pinned = bench::pin_this_thread(cfg.reader_cpus[static_cast<std::size_t>(index)]);
    }
    bench::Rng rng(cfg.seed * 1000 + static_cast<std::uint64_t>(index) + 1);
    const std::uint64_t m = static_cast<std::uint64_t>(cfg.m), n = static_cast<std::uint64_t>(cfg.n);
    Trade t;
    std::uint64_t since_write = 0, version = 1;

    while (phase.load(std::memory_order_relaxed) == kWait) std::this_thread::yield();

    for (;;) {
        const int ph = phase.load(std::memory_order_relaxed);
        if (ph == kStop) break;

        const std::uint64_t r = rng.next();
        long id;
        if (!hot_ids.empty() && (r % 10) < 8) {
            id = hot_ids[(r >> 8) % hot_ids.size()];
        } else if (cfg.keys_hit) {
            id = key_of(static_cast<int>((r >> 8) % m), static_cast<int>((r >> 40) % n));
        } else {
            id = static_cast<long>((r >> 8) % (m * n));  // the generator's formula
        }

        const std::uint64_t t0 = bench::now_ns();
        const bool found = store.get(id, &t, sizeof(t));
        const std::uint64_t t1 = bench::now_ns();
        bench::do_not_optimize(t);

        if (ph == kMeasure) {
            out.hist.record(t1 - t0);
            ++out.ops;
            if (found) {
                ++out.hits;
                if (t.trade_id != id) ++out.corrupt;
            } else {
                ++out.misses;
            }
        }

        // Single-threaded variant only: this same thread is also the writer.
        if (inline_store != nullptr && ++since_write >= static_cast<std::uint64_t>(cfg.inline_every)) {
            since_write = 0;
            const int i = static_cast<int>(rng.next() % m), j = static_cast<int>(rng.next() % n);
            const Trade nt = make_trade(key_of(i, j), i, j, version++);
            const bool ok = inline_store->put(nt.trade_id, &nt, sizeof(nt));
            if (ph == kMeasure && ok) ++out.inline_writes;
        }
    }
}

// noinline: keeps this loop a distinct frame in profiles.
__attribute__((noinline)) void writer_main(KVStore& store, const Config& cfg, const std::atomic<int>& phase,
                 WriterResult& out) {
    if (cfg.pin && cfg.writer_cpu >= 0) out.pinned = bench::pin_this_thread(cfg.writer_cpu);
    bench::Rng rng(cfg.seed * 7777 + 99);
    const std::uint64_t m = static_cast<std::uint64_t>(cfg.m), n = static_cast<std::uint64_t>(cfg.n);
    const std::uint64_t interval_ns =
        cfg.writer_rate > 0 ? 1000000000ULL / static_cast<std::uint64_t>(cfg.writer_rate) : 0;
    std::uint64_t version = 1;

    while (phase.load(std::memory_order_relaxed) == kWait) std::this_thread::yield();
    std::uint64_t next_due = bench::now_ns();

    for (;;) {
        const int ph = phase.load(std::memory_order_relaxed);
        if (ph == kStop) break;
        if (interval_ns != 0) {
            // Paced writer: wait for the next slot without sleeping past it.
            while (bench::now_ns() < next_due) {
                if (phase.load(std::memory_order_relaxed) == kStop) return;
                std::this_thread::yield();
            }
            next_due += interval_ns;
        }
        const std::uint64_t r = rng.next();
        const int i = static_cast<int>((r >> 8) % m), j = static_cast<int>((r >> 40) % n);
        const Trade nt = make_trade(key_of(i, j), i, j, version++);  // overwrite an existing trade

        const std::uint64_t t0 = bench::now_ns();
        const bool ok = store.put(nt.trade_id, &nt, sizeof(nt));
        const std::uint64_t t1 = bench::now_ns();

        if (ok) ++out.ok_total;
        if (ph == kMeasure) {
            out.hist.record(t1 - t0);
            if (ok) ++out.ok;
            else ++out.failed;
        }
    }
}

std::string join_ints(const std::vector<int>& v, std::size_t count) {
    std::string s;
    for (std::size_t i = 0; i < count && i < v.size(); ++i) {
        if (i != 0) s += ' ';
        s += std::to_string(v[i]);
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    bench::Args args(argc, argv);
    Config cfg;
    cfg.readers = static_cast<int>(args.get_long("--readers", cfg.readers));
    cfg.duration_s = args.get_double("--duration", cfg.duration_s);
    cfg.warmup_s = args.get_double("--warmup", cfg.warmup_s);
    const std::string writer = args.get("--writer", "on");
    cfg.writer_rate = args.get_long("--writer-rate", 0);
    const std::string keys = args.get("--keys", "hit");
    cfg.m = static_cast<int>(args.get_long("--m", cfg.m));
    cfg.n = static_cast<int>(args.get_long("--n", cfg.n));
    cfg.hot_every = static_cast<int>(args.get_long("--hot-every", cfg.hot_every));
    cfg.seed = static_cast<std::uint64_t>(args.get_long("--seed", 1));
    cfg.pin = args.get("--pin", "on") == "on";
    cfg.reader_cpus = bench::parse_cpu_list(args.get("--reader-cpus", ""));
    cfg.writer_cpu = static_cast<int>(args.get_long("--writer-cpu", -1));
    const std::string run_id = args.get("--run-id", "adhoc");
    const std::string git_sha = args.get("--git-sha", "unknown");
    const std::string scenario = args.get("--scenario", "adhoc");
    const std::string csv_path = args.get("--csv", "");
    const std::string hist_path = args.get("--hist", "");
    const bool csv_header_only = args.has_flag("--csv-header");
    if (!args.first_unused().empty()) {
        std::fprintf(stderr, "unknown argument: %s\n", args.first_unused().c_str());
        return 2;
    }

    const char* header =
        "run_id,git_sha,variant,scenario,readers,writer_mode,writer_rate_target,key_mode,"
        "record_count,warmup_s,seconds_actual,read_ops,read_hits,read_misses,read_corrupt,"
        "read_rps,write_ok,write_failed,write_rps,p50_ns,p99_ns,p999_ns,max_ns,mean_ns,"
        "reader_min_rps,reader_max_rps,write_p50_ns,write_p99_ns,write_max_ns,pinned,"
        "reader_cpus,writer_cpu,chunks_live,chunk_bytes_reserved,value_bytes_live\n";
    if (csv_header_only) {
        std::fputs(header, stdout);
        return 0;
    }

    const std::string variant = nova_kv_sync_mode_name();
    const bool is_single = variant == "single";
    cfg.keys_hit = keys == "hit";
    if (keys != "hit" && keys != "reference") {
        std::fprintf(stderr, "--keys must be 'hit' or 'reference'\n");
        return 2;
    }
    std::string writer_mode;
    if (writer == "on") {
        cfg.writer_thread = true;
        writer_mode = cfg.writer_rate > 0 ? "thread_paced" : "thread_max";
    } else if (writer == "off") {
        cfg.writer_thread = false;
        writer_mode = "none";
    } else if (writer.rfind("inline:", 0) == 0) {
        cfg.writer_thread = false;
        cfg.inline_every = std::atol(writer.c_str() + 7);
        writer_mode = "inline_same_thread";
        if (cfg.inline_every < 1) {
            std::fprintf(stderr, "--writer inline:N needs N >= 1\n");
            return 2;
        }
    } else {
        std::fprintf(stderr, "--writer must be on, off or inline:N\n");
        return 2;
    }
    if (is_single && (cfg.readers != 1 || cfg.writer_thread)) {
        std::fprintf(stderr,
                     "the 'single' store variant has no synchronization: only --readers 1 with "
                     "--writer off or --writer inline:N is a valid run\n");
        return 2;
    }
    if (cfg.inline_every > 0 && cfg.readers != 1) {
        std::fprintf(stderr, "--writer inline:N requires --readers 1\n");
        return 2;
    }
    if (cfg.readers < 1 || cfg.duration_s <= 0 || cfg.m < 1 || cfg.n < 1 || cfg.hot_every < 1) {
        std::fprintf(stderr, "invalid --readers/--duration/--m/--n/--hot-every\n");
        return 2;
    }

    // ---- Load (not measured): same keys and hot set as the supplied generator.
    KVStore store;
    std::vector<long> hot_ids;
    long count = 0;
    for (int i = 0; i < cfg.m; i++) {
        for (int j = 0; j < cfg.n; j++) {
            const Trade t = make_trade(key_of(i, j), i, j, 0);
            if (!store.put(t.trade_id, &t, sizeof(t))) {
                std::fprintf(stderr, "load failed at key %ld\n", t.trade_id);
                return 1;
            }
            if (++count % cfg.hot_every == 0) hot_ids.push_back(t.trade_id);
        }
    }

    // ---- Threads start on a barrier, warm up, then are measured together.
    std::atomic<int> phase{kWait};
    std::vector<ReaderResult> results(static_cast<std::size_t>(cfg.readers));
    WriterResult wres;
    std::vector<std::thread> threads;
    for (int r = 0; r < cfg.readers; ++r) {
        threads.emplace_back(reader_main, std::cref(store),
                             cfg.inline_every > 0 ? &store : nullptr, std::cref(cfg),
                             std::cref(hot_ids), std::cref(phase), r,
                             std::ref(results[static_cast<std::size_t>(r)]));
    }
    std::thread writer_thread;
    if (cfg.writer_thread) {
        writer_thread = std::thread(writer_main, std::ref(store), std::cref(cfg), std::cref(phase),
                                    std::ref(wres));
    }

    phase.store(kWarmup);
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.warmup_s));
    const std::uint64_t t_start = bench::now_ns();
    phase.store(kMeasure);
    std::this_thread::sleep_for(std::chrono::duration<double>(cfg.duration_s));
    phase.store(kStop);
    const std::uint64_t t_end = bench::now_ns();
    for (auto& th : threads) th.join();
    if (writer_thread.joinable()) writer_thread.join();

    // ---- Aggregate (after every thread has stopped).
    const double seconds = static_cast<double>(t_end - t_start) / 1e9;
    LatencyHistogram all;
    std::uint64_t ops = 0, hits = 0, misses = 0, corrupt = 0, inline_writes = 0;
    double min_rps = 0, max_rps = 0;
    bool pinned = wres.pinned;
    for (std::size_t r = 0; r < results.size(); ++r) {
        const ReaderResult& x = results[r];
        all.merge(x.hist);
        ops += x.ops;
        hits += x.hits;
        misses += x.misses;
        corrupt += x.corrupt;
        inline_writes += x.inline_writes;
        pinned = pinned && x.pinned;
        const double rps = static_cast<double>(x.ops) / seconds;
        if (r == 0 || rps < min_rps) min_rps = rps;
        if (r == 0 || rps > max_rps) max_rps = rps;
    }
    const std::uint64_t write_ok = cfg.writer_thread ? wres.ok : inline_writes;
    const std::uint64_t write_failed = wres.failed;
    const double read_rps = static_cast<double>(ops) / seconds;
    const double write_rps = static_cast<double>(write_ok) / seconds;
    const NovaKvStats st = NovaKvInspector::stats(store);
    const char* invariant_error = NovaKvInspector::validate(store);
    const std::size_t pinned_readers =
        cfg.pin ? std::min(cfg.reader_cpus.size(), static_cast<std::size_t>(cfg.readers)) : 0;
    const std::string cpus = join_ints(cfg.reader_cpus, pinned_readers);

    std::printf("%-12s scenario=%s readers=%d writer=%s keys=%s seconds=%.3f\n", variant.c_str(),
                scenario.c_str(), cfg.readers, writer_mode.c_str(), keys.c_str(), seconds);
    const std::uintptr_t lock_addr =
        reinterpret_cast<std::uintptr_t>(NovaKvInspector::lock_address(store));
    // The lock object may straddle two 64-byte cache lines.
    std::printf("  store lock object at 0x%llx, %zu bytes (cache lines 0x%llx..0x%llx)\n",
                static_cast<unsigned long long>(lock_addr), NovaKvInspector::lock_size(),
                static_cast<unsigned long long>(lock_addr & ~std::uintptr_t{63}),
                static_cast<unsigned long long>((lock_addr + NovaKvInspector::lock_size() - 1) &
                                                ~std::uintptr_t{63}));
    std::printf("  reads : ops=%llu rps=%.0f hits=%llu misses=%llu corrupt=%llu\n",
                static_cast<unsigned long long>(ops), read_rps,
                static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
                static_cast<unsigned long long>(corrupt));
    std::printf("  read latency ns: p50=%llu p99=%llu p999=%llu max=%llu mean=%.1f\n",
                static_cast<unsigned long long>(all.percentile(0.50)),
                static_cast<unsigned long long>(all.percentile(0.99)),
                static_cast<unsigned long long>(all.percentile(0.999)),
                static_cast<unsigned long long>(all.max()), all.mean());
    std::printf("  writes: ok=%llu failed=%llu rps=%.0f p50=%llu p99=%llu max=%llu ns\n",
                static_cast<unsigned long long>(write_ok),
                static_cast<unsigned long long>(write_failed), write_rps,
                static_cast<unsigned long long>(wres.hist.percentile(0.50)),
                static_cast<unsigned long long>(wres.hist.percentile(0.99)),
                static_cast<unsigned long long>(wres.hist.max()));

    if (!csv_path.empty()) {
        std::FILE* probe = std::fopen(csv_path.c_str(), "r");
        const bool exists = probe != nullptr;
        if (probe != nullptr) std::fclose(probe);
        std::FILE* f = std::fopen(csv_path.c_str(), "a");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", csv_path.c_str());
            return 1;
        }
        if (!exists) std::fputs(header, f);
        std::fprintf(
            f,
            "%s,%s,%s,%s,%d,%s,%ld,%s,%d,%.3f,%.6f,%llu,%llu,%llu,%llu,%.1f,%llu,%llu,%.1f,"
            "%llu,%llu,%llu,%llu,%.1f,%.1f,%.1f,%llu,%llu,%llu,%d,%s,%d,%llu,%llu,%llu\n",
            run_id.c_str(), git_sha.c_str(), variant.c_str(), scenario.c_str(), cfg.readers,
            writer_mode.c_str(), cfg.writer_rate, keys.c_str(), cfg.m * cfg.n, cfg.warmup_s,
            seconds, static_cast<unsigned long long>(ops), static_cast<unsigned long long>(hits),
            static_cast<unsigned long long>(misses), static_cast<unsigned long long>(corrupt),
            read_rps, static_cast<unsigned long long>(write_ok),
            static_cast<unsigned long long>(write_failed), write_rps,
            static_cast<unsigned long long>(all.percentile(0.50)),
            static_cast<unsigned long long>(all.percentile(0.99)),
            static_cast<unsigned long long>(all.percentile(0.999)),
            static_cast<unsigned long long>(all.max()), all.mean(), min_rps, max_rps,
            static_cast<unsigned long long>(wres.hist.percentile(0.50)),
            static_cast<unsigned long long>(wres.hist.percentile(0.99)),
            static_cast<unsigned long long>(wres.hist.max()), (cfg.pin && pinned) ? 1 : 0,
            cpus.c_str(), cfg.pin ? cfg.writer_cpu : -1,
            static_cast<unsigned long long>(st.chunks_live),
            static_cast<unsigned long long>(st.chunk_bytes_reserved),
            static_cast<unsigned long long>(st.value_bytes_live));
        std::fclose(f);
    }
    if (!hist_path.empty()) {
        std::FILE* f = std::fopen(hist_path.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", hist_path.c_str());
            return 1;
        }
        std::fprintf(f, "# run_id=%s variant=%s scenario=%s readers=%d writer=%s seconds=%.6f\n",
                     run_id.c_str(), variant.c_str(), scenario.c_str(), cfg.readers,
                     writer_mode.c_str(), seconds);
        std::fprintf(f, "# reader threads: ");
        for (const ReaderResult& x : results) {
            std::fprintf(f, "%llu ", static_cast<unsigned long long>(x.ops));
        }
        std::fprintf(f, "ops\nseries,bucket_lower_ns,count\n");
        all.dump(f, "read");
        wres.hist.dump(f, "write");
        std::fclose(f);
    }

    // ---- Validity of the run itself.
    int rc = 0;
    if (corrupt != 0) {
        std::fprintf(stderr, "INVALID: %llu reads returned a record for another key\n",
                     static_cast<unsigned long long>(corrupt));
        rc = 1;
    }
    if (ops == 0) {
        std::fprintf(stderr, "INVALID: no read completed in the measured window\n");
        rc = 1;
    }
    if (writer_mode != "none" && write_ok == 0) {
        std::fprintf(stderr, "INVALID: writer requested but no update succeeded in the window\n");
        rc = 1;
    }
    if (write_failed != 0) {
        std::fprintf(stderr, "INVALID: %llu writer updates failed\n",
                     static_cast<unsigned long long>(write_failed));
        rc = 1;
    }
    if (cfg.keys_hit && misses != 0) {
        std::fprintf(stderr, "INVALID: %llu misses although every requested key exists\n",
                     static_cast<unsigned long long>(misses));
        rc = 1;
    }
    if (invariant_error != nullptr) {
        std::fprintf(stderr, "INVALID: store invariant violated after the run: %s\n",
                     invariant_error);
        rc = 1;
    }
    if (st.live_keys != static_cast<std::uint64_t>(cfg.m) * static_cast<std::uint64_t>(cfg.n)) {
        std::fprintf(stderr, "INVALID: key count changed during the run\n");
        rc = 1;
    }
    if (cfg.pin && !pinned) {
        std::fprintf(stderr, "INVALID: a thread could not be pinned to its CPU\n");
        rc = 1;
    }
    return rc;
}
