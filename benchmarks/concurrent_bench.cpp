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
// with (concurrent_bench_single / _global_lock / _rwlock / _rcu). The
// unsynchronized `single` variant is only run as ONE thread: either reads only,
// or with --writer inline:N, where the same thread performs one update per N
// reads.
//
// Part B (rcu): replaced versions are reclaimed by the store's background
// cleaner (epoch-based). --reclaim-batch N sets KVStore::set_reclaim_batch;
// --reclaim disabled|stw selects the two comparator modes. If reclamation does
// not keep up (or is disabled) memory grows; the writer stops, and the run is
// reported INVALID, if the store holds more than --retain-limit-mb.
// Copy-on-write and reclamation counters are printed and, with --cow-csv,
// appended to a separate file; the main CSV schema is unchanged.
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
#include <cstdlib>
#include <optional>
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
    std::uint64_t retain_limit_bytes = 0;  // rcu only; 0 = no limit
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
    bool stopped_at_retain_limit = false;
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
        // Part B without reclamation: never let retained versions exhaust memory.
        if (cfg.retain_limit_bytes != 0 && (version & 0xFF) == 0 &&
            NovaKvInspector::memory_held_bytes(store) > cfg.retain_limit_bytes) {
            out.stopped_at_retain_limit = true;
            return;
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
    const std::string cow_csv_path = args.get("--cow-csv", "");
    cfg.retain_limit_bytes =
        static_cast<std::uint64_t>(args.get_long("--retain-limit-mb", 6000)) * 1024 * 1024;
    const long reclaim_batch = args.get_long("--reclaim-batch", 0);  // 0 = store default
    const std::string reclaim = args.get("--reclaim", "epoch");
    const long work_per_version = args.get_long("--reclaim-work-per-version", 0);
    // --lock-offset N (0, 16, 32 or 48): place the store's lock object at that
    // offset inside a 64-byte cache line. Without it the offset is whatever the
    // heap happens to give, which depends on the lengths of the command-line
    // strings, and the lock-based stores run at two different speeds depending
    // on it (results/part_d_diagnostics/lock_placement_effect.txt).
    const long lock_offset_wanted = args.get_long("--lock-offset", -1);
    if (lock_offset_wanted >= 0 && (lock_offset_wanted > 48 || lock_offset_wanted % 16 != 0)) {
        std::fprintf(stderr, "--lock-offset must be 0, 16, 32 or 48\n");
        return 2;
    }
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
        "reader_cpus,writer_cpu,chunks_live,chunk_bytes_reserved,value_bytes_live,"
        "lock_offset_in_line,lock_bytes,lock_placement\n";
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

    if (reclaim != "epoch" && reclaim != "disabled" && reclaim != "stw") {
        std::fprintf(stderr, "--reclaim must be epoch, disabled or stw\n");
        return 2;
    }
    // Part B options are fixed when the store is constructed. A one-thread run
    // with an inline writer also lets that thread run the reclamation passes.
    NovaKvInspector::set_default_rcu_options(reclaim == "disabled" ? 1 : (reclaim == "stw" ? 2 : 0),
                                             cfg.inline_every > 0 ? 2 : 0);

    // ---- Load (not measured): same keys and hot set as the supplied generator.
    // The store is constructed, and if its lock is not where --lock-offset asks,
    // destroyed and constructed again after a small allocation that shifts the
    // heap. Only the position of the store changes; the store itself does not.
    std::optional<KVStore> store_slot;
    for (int attempt = 0;; ++attempt) {
        store_slot.emplace();
        if (lock_offset_wanted < 0) break;
        const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(NovaKvInspector::lock_address(*store_slot));
        if (static_cast<long>(at % 64) == lock_offset_wanted) break;
        store_slot.reset();
        if (attempt >= 512) {
            std::fprintf(stderr, "could not place the lock at offset %ld of a cache line\n", lock_offset_wanted);
            return 1;
        }
        // Kept until exit on purpose: it is what moves the next store.
        void* pad = std::malloc(static_cast<std::size_t>(16 + 16 * (attempt % 61)));
        if (pad == nullptr) return 1;
        static_cast<volatile char*>(pad)[0] = 0;
    }
    KVStore& store = *store_slot;
    if (reclaim_batch > 0) store.set_reclaim_batch(static_cast<int>(reclaim_batch));
    if (work_per_version > 0) {
        NovaKvInspector::set_reclaim_work_per_version(store, static_cast<unsigned>(work_per_version));
    }
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

    // Part B: let reclamation catch up with the load before measuring, so the
    // run starts from a store that holds only its current state (no reader
    // thread exists yet, so nothing holds the epoch back).
    const bool versioned = NovaKvInspector::is_versioned();
    if (!versioned) cfg.retain_limit_bytes = 0;
    if (versioned && reclaim == "epoch") NovaKvInspector::settle(store);
    const NovaKvStats stats_after_load = NovaKvInspector::stats(store);
    const std::uint64_t memory_after_load = NovaKvInspector::memory_held_bytes(store);

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
    // Statistics first (they describe the state the run ended in), then, for
    // Part B, let reclamation finish (all reader threads have exited, so the
    // epoch can advance) and validate the settled store.
    const NovaKvStats st = NovaKvInspector::stats(store);
    const std::uint64_t memory_at_end = NovaKvInspector::memory_held_bytes(store);
    const std::uint64_t backlog_at_end = NovaKvInspector::backlog(store);
    // settle() stops the background cleaner, which records its CPU time, runs
    // the remaining passes on this thread, and restarts it.
    if (versioned && reclaim != "disabled") NovaKvInspector::settle(store);
    const std::uint64_t memory_settled = NovaKvInspector::memory_held_bytes(store);
    const std::uint64_t cleaner_cpu_ns =
        NovaKvInspector::stats(store).cleaner_cpu_ns - stats_after_load.cleaner_cpu_ns;
    const char* invariant_error = NovaKvInspector::validate(store);
    const std::size_t pinned_readers =
        cfg.pin ? std::min(cfg.reader_cpus.size(), static_cast<std::size_t>(cfg.readers)) : 0;
    const std::string cpus = join_ints(cfg.reader_cpus, pinned_readers);

    std::printf("%-12s scenario=%s readers=%d writer=%s keys=%s seconds=%.3f\n", variant.c_str(),
                scenario.c_str(), cfg.readers, writer_mode.c_str(), keys.c_str(), seconds);
    const std::uintptr_t lock_addr =
        reinterpret_cast<std::uintptr_t>(NovaKvInspector::lock_address(store));
    // The lock object may straddle two 64-byte cache lines.
    std::printf("  store lock object at 0x%llx (offset %u in its cache line, %s), %zu bytes (cache lines 0x%llx..0x%llx)\n",
                static_cast<unsigned long long>(lock_addr), static_cast<unsigned>(lock_addr % 64),
                lock_offset_wanted >= 0 ? "placed by --lock-offset" : "not controlled", NovaKvInspector::lock_size(),
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

    // ---- Part B: copy-on-write cost of the writes made since the load.
    const std::uint64_t writes_all = cfg.writer_thread ? wres.ok_total : inline_writes;
    if (versioned) {
        const std::uint64_t published = st.versions_published - stats_after_load.versions_published;
        const double per = published == 0 ? 0.0 : 1.0 / static_cast<double>(published);
        const std::uint64_t index_copied = st.index_bytes_copied - stats_after_load.index_bytes_copied;
        const std::uint64_t dir_copied = st.directory_bytes_copied - stats_after_load.directory_bytes_copied;
        const std::uint64_t owner_refs = st.owner_refs_taken - stats_after_load.owner_refs_taken;
        const std::uint64_t pins = st.chunk_refs_taken - stats_after_load.chunk_refs_taken;
        const std::uint64_t built = st.segments_built - stats_after_load.segments_built;
        const std::uint64_t value_bytes = st.value_bytes_appended - stats_after_load.value_bytes_appended;
        const std::uint64_t reclaimed = st.versions_reclaimed - stats_after_load.versions_reclaimed;
        const std::uint64_t advances = st.epoch_advances - stats_after_load.epoch_advances;
        const std::uint64_t attempts = st.epoch_advance_attempts - stats_after_load.epoch_advance_attempts;
        const std::uint64_t blocked = st.epoch_advances_blocked - stats_after_load.epoch_advances_blocked;
        const std::uint64_t passes = st.reclaim_passes - stats_after_load.reclaim_passes;
        const std::uint64_t ids_reused = st.chunk_ids_reused - stats_after_load.chunk_ids_reused;
        std::printf("  rcu   : publications=%llu (writes incl. warm-up=%llu) commits_failed=%llu reclaim=%s batch=%d\n",
                    static_cast<unsigned long long>(published),
                    static_cast<unsigned long long>(writes_all),
                    static_cast<unsigned long long>(st.commits_failed), reclaim.c_str(), st.reclaim_batch);
        std::printf("  rcu per publication: segments_cloned=%.2f index_bytes_copied=%.0f "
                    "directory_bytes_copied=%.0f owner_refs=%.1f chunk_refs=%.1f value_bytes=%.1f\n",
                    static_cast<double>(built) * per, static_cast<double>(index_copied) * per,
                    static_cast<double>(dir_copied) * per, static_cast<double>(owner_refs) * per,
                    static_cast<double>(pins) * per, static_cast<double>(value_bytes) * per);
        std::printf("  rcu reclamation: reclaimed=%llu backlog_at_end=%llu backlog_max=%llu passes=%llu "
                    "epoch_advances=%llu/%llu attempts (%llu blocked by a reader) max_versions_in_pass=%llu "
                    "interrupted=%llu chunk_ids_reused=%llu reader_slots=%llu validation_retries=%llu "
                    "cleaner_cpu=%.3fs (%.1f%% of one CPU over warm-up + window)\n",
                    static_cast<unsigned long long>(reclaimed),
                    static_cast<unsigned long long>(backlog_at_end),
                    static_cast<unsigned long long>(st.backlog_max),
                    static_cast<unsigned long long>(passes), static_cast<unsigned long long>(advances),
                    static_cast<unsigned long long>(attempts), static_cast<unsigned long long>(blocked),
                    static_cast<unsigned long long>(st.reclaim_max_versions_in_pass),
                    static_cast<unsigned long long>(st.reclaim_interrupted_versions),
                    static_cast<unsigned long long>(ids_reused),
                    static_cast<unsigned long long>(st.reader_slots_owned),
                    static_cast<unsigned long long>(st.reader_validation_retries),
                    static_cast<double>(cleaner_cpu_ns) / 1e9,
                    100.0 * static_cast<double>(cleaner_cpu_ns) / 1e9 / (seconds + cfg.warmup_s));
        std::printf("  rcu memory held: after_load=%.1f MiB at_end=%.1f MiB settled=%.1f MiB "
                    "(versions + segments + chunks)\n",
                    static_cast<double>(memory_after_load) / 1048576.0,
                    static_cast<double>(memory_at_end) / 1048576.0,
                    static_cast<double>(memory_settled) / 1048576.0);
        if (!cow_csv_path.empty()) {
            std::FILE* probe = std::fopen(cow_csv_path.c_str(), "r");
            const bool exists = probe != nullptr;
            if (probe != nullptr) std::fclose(probe);
            std::FILE* f = std::fopen(cow_csv_path.c_str(), "a");
            if (f == nullptr) {
                std::fprintf(stderr, "cannot open %s\n", cow_csv_path.c_str());
                return 1;
            }
            if (!exists) {
                std::fputs("run_id,git_sha,variant,scenario,readers,writer_mode,reclaim_mode,reclaim_batch,"
                           "seconds_actual,writes_incl_warmup,publications,commits_failed,"
                           "segments_cloned,index_bytes_copied,directory_bytes_copied,owner_refs_taken,"
                           "chunk_refs_taken,value_bytes_appended,versions_reclaimed,backlog_at_end,"
                           "backlog_max,reclaim_passes,epoch_advances,epoch_advance_attempts,"
                           "epoch_advances_blocked,reclaim_max_versions_in_pass,"
                           "reclaim_interrupted_versions,chunk_ids_reused,reader_slots_owned,"
                           "reader_validation_retries,stw_pauses,memory_after_load_bytes,"
                           "memory_at_end_bytes,memory_settled_bytes,stopped_at_retain_limit,cleaner_cpu_ns\n",
                           f);
            }
            std::fprintf(f, "%s,%s,%s,%s,%d,%s,%s,%d,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%llu\n",
                         run_id.c_str(), git_sha.c_str(), variant.c_str(), scenario.c_str(), cfg.readers,
                         writer_mode.c_str(), reclaim.c_str(), st.reclaim_batch, seconds,
                         static_cast<unsigned long long>(writes_all),
                         static_cast<unsigned long long>(published),
                         static_cast<unsigned long long>(st.commits_failed),
                         static_cast<unsigned long long>(built),
                         static_cast<unsigned long long>(index_copied),
                         static_cast<unsigned long long>(dir_copied),
                         static_cast<unsigned long long>(owner_refs),
                         static_cast<unsigned long long>(pins),
                         static_cast<unsigned long long>(value_bytes),
                         static_cast<unsigned long long>(reclaimed),
                         static_cast<unsigned long long>(backlog_at_end),
                         static_cast<unsigned long long>(st.backlog_max),
                         static_cast<unsigned long long>(passes),
                         static_cast<unsigned long long>(advances),
                         static_cast<unsigned long long>(attempts),
                         static_cast<unsigned long long>(blocked),
                         static_cast<unsigned long long>(st.reclaim_max_versions_in_pass),
                         static_cast<unsigned long long>(st.reclaim_interrupted_versions),
                         static_cast<unsigned long long>(ids_reused),
                         static_cast<unsigned long long>(st.reader_slots_owned),
                         static_cast<unsigned long long>(st.reader_validation_retries),
                         static_cast<unsigned long long>(st.stw_pauses),
                         static_cast<unsigned long long>(memory_after_load),
                         static_cast<unsigned long long>(memory_at_end),
                         static_cast<unsigned long long>(memory_settled),
                         wres.stopped_at_retain_limit ? 1 : 0,
                         static_cast<unsigned long long>(cleaner_cpu_ns));
            std::fclose(f);
        }
    }

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
            "%llu,%llu,%llu,%llu,%.1f,%.1f,%.1f,%llu,%llu,%llu,%d,%s,%d,%llu,%llu,%llu,%u,%zu,%s\n",
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
            static_cast<unsigned long long>(st.value_bytes_live), static_cast<unsigned>(lock_addr % 64),
            NovaKvInspector::lock_size(), lock_offset_wanted >= 0 ? "controlled" : "uncontrolled");
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
    if (wres.stopped_at_retain_limit) {
        std::fprintf(stderr,
                     "INVALID: the writer stopped before the end of the window because the store "
                     "held more than --retain-limit-mb (reclamation disabled or not keeping up)\n");
        rc = 1;
    }
    if (versioned && st.commits_failed != 0) {
        std::fprintf(stderr, "INVALID: %llu commits failed\n",
                     static_cast<unsigned long long>(st.commits_failed));
        rc = 1;
    }
    return rc;
}
