// reclaim_bench.cpp -- Part D.2, workload B: a workload in which reclamation
// FALLS BEHIND, so that the reclaim batch has something to trade off.
//
// With the default point-get workload (concurrent_bench) the cleaner keeps up
// at every batch size: a pass rarely finds more than a handful of versions to
// free, so batch sizes of 256 and 1024 never fill. Here a backlog is built on
// purpose and then released:
//
//   - values are large (--value-bytes, default 4096), so destroying a version
//     frees real memory and costs real work;
//   - one "pinner" thread repeatedly takes a Snapshot, holds it for
//     --pin-hold-ms and releases it, then waits --pin-gap-ms. While a snapshot
//     is held nothing retired after it can be reclaimed, so the retirement
//     queue grows; at each release the cleaner has a full backlog in front of it;
//   - an unthrottled writer overwrites existing keys the whole time;
//   - readers do point gets and record every latency.
//
// Phases:  warm-up  ->  MEASURE (writer + pinner + readers)
//                   ->  CLEANUP (writer stopped, the last snapshot released,
//                       readers still running): the time until the backlog is
//                       empty and the reader latencies during it are recorded
//                       separately.
//
// rcu build only. --reclaim epoch|disabled|stw and --reclaim-batch N as in
// concurrent_bench. With reclamation disabled there is no cleanup to measure
// and a memory guard (--retain-limit-mb) stops the writer.
//
// The run fails if a reader sees a value that does not belong to its key, if
// the writer completed no update, if (epoch mode) a pass freed more versions
// than the batch, or if the backlog did not drain (epoch: to zero; stw: to zero
// or to rest below the comparator's trigger).
#include "kvstore.h"

#include "bench_common.h"
#include "build_info.h"
#include "inspect.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Mirrors kStwBacklogTrigger in src/versioned_store.cpp (the comparator frees
// its retire list only once it is at least this long).
constexpr std::uint64_t kStwTrigger = 1024;

using bench::LatencyHistogram;

enum Phase : int { kWait = 0, kWarmup = 1, kMeasure = 2, kCleanup = 3, kStop = 4 };

struct Header {
    long key;
    std::uint64_t generation;
    std::uint64_t check;
};

std::uint64_t check_of(long key, std::uint64_t generation) {
    return (static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ULL) ^ (generation * 0xC2B2AE3D27D4EB4FULL);
}

void fill_value(std::vector<unsigned char>& v, long key, std::uint64_t generation) {
    const Header h{key, generation, check_of(key, generation)};
    std::memcpy(v.data(), &h, sizeof(h));
    // The rest of the value only has to be bytes; its last byte is checked.
    v[v.size() - 1] = static_cast<unsigned char>(h.check);
}

struct ReaderResult {
    LatencyHistogram measure, cleanup;
    std::uint64_t ops = 0, cleanup_ops = 0, corrupt = 0, missing = 0;
};

}  // namespace

int main(int argc, char** argv) {
    bench::Args args(argc, argv);
    args.finalize();
    const int readers = static_cast<int>(args.get_long("--readers", 8));
    const double duration_s = args.get_double("--duration", 3.0);
    const double warmup_s = args.get_double("--warmup", 0.5);
    const long keys = args.get_long("--keys", 2000);
    const std::size_t value_bytes = static_cast<std::size_t>(args.get_long("--value-bytes", 4096));
    const std::string reclaim = args.get("--reclaim", "epoch");
    const long reclaim_batch = args.get_long("--reclaim-batch", 0);
    const long pin_hold_ms = args.get_long("--pin-hold-ms", 20);
    const long pin_gap_ms = args.get_long("--pin-gap-ms", 30);
    const std::vector<int> reader_cpus = bench::parse_cpu_list(args.get("--reader-cpus", ""));
    const int writer_cpu = static_cast<int>(args.get_long("--writer-cpu", -1));
    const int pinner_cpu = static_cast<int>(args.get_long("--pinner-cpu", -1));
    const std::uint64_t retain_limit =
        static_cast<std::uint64_t>(args.get_long("--retain-limit-mb", 6000)) * 1024 * 1024;
    const std::uint64_t seed = static_cast<std::uint64_t>(args.get_long("--seed", 1));
    const std::string run_id = args.get("--run-id", "adhoc");
    const std::string git_sha = args.get("--git-sha", "unknown");
    const std::string scenario = args.get("--scenario", "backlog");
    const std::string csv_path = args.get("--csv", "");
    const std::string hist_path = args.get("--hist", "");
    if (!args.first_unused().empty()) {
        std::fprintf(stderr, "unknown argument: %s\n", args.first_unused().c_str());
        return 2;
    }
    if (!NovaKvInspector::is_versioned()) {
        std::fprintf(stderr, "reclaim_bench needs the rcu build\n");
        return 2;
    }
    if (reclaim != "epoch" && reclaim != "disabled" && reclaim != "stw") {
        std::fprintf(stderr, "--reclaim must be epoch, disabled or stw\n");
        return 2;
    }
    if (readers < 1 || keys < 1 || value_bytes < sizeof(Header) + 1 || duration_s <= 0) {
        std::fprintf(stderr, "invalid parameters\n");
        return 2;
    }

    NovaKvInspector::set_default_rcu_options(reclaim == "disabled" ? 1 : (reclaim == "stw" ? 2 : 0), 0);
    KVStore store;
    if (reclaim_batch > 0) store.set_reclaim_batch(static_cast<int>(reclaim_batch));
    {
        std::vector<unsigned char> v(value_bytes, 0x5A);
        for (long k = 0; k < keys; ++k) {
            fill_value(v, k, 1);
            if (!store.put(k, v.data(), v.size())) {
                std::fprintf(stderr, "load failed\n");
                return 1;
            }
        }
    }
    if (reclaim != "disabled") NovaKvInspector::settle(store);
    const NovaKvStats st0 = NovaKvInspector::stats(store);
    const std::uint64_t held_idle = NovaKvInspector::memory_held_bytes(store);

    std::atomic<int> phase{kWait};
    std::atomic<bool> pinner_stop{false};
    std::vector<ReaderResult> results(static_cast<std::size_t>(readers));
    std::vector<std::thread> threads;
    for (int t = 0; t < readers; ++t) {
        threads.emplace_back([&, t]() {
            if (t < static_cast<int>(reader_cpus.size())) bench::pin_this_thread(reader_cpus[static_cast<std::size_t>(t)]);
            ReaderResult& out = results[static_cast<std::size_t>(t)];
            bench::Rng rng(seed * 1000 + static_cast<std::uint64_t>(t) + 1);
            std::vector<unsigned char> buf(value_bytes);
            while (phase.load(std::memory_order_relaxed) == kWait) std::this_thread::yield();
            for (;;) {
                const int ph = phase.load(std::memory_order_relaxed);
                if (ph == kStop) break;
                const long key = static_cast<long>(rng.next() % static_cast<std::uint64_t>(keys));
                const std::uint64_t t0 = bench::now_ns();
                const bool found = store.get(key, buf.data(), buf.size());
                const std::uint64_t t1 = bench::now_ns();
                if (ph == kWarmup) continue;
                if (!found) {
                    ++out.missing;
                    continue;
                }
                Header h;
                std::memcpy(&h, buf.data(), sizeof(h));
                if (h.key != key || h.check != check_of(key, h.generation) ||
                    buf[value_bytes - 1] != static_cast<unsigned char>(h.check)) {
                    ++out.corrupt;
                }
                if (ph == kMeasure) {
                    out.measure.record(t1 - t0);
                    ++out.ops;
                } else {
                    out.cleanup.record(t1 - t0);
                    ++out.cleanup_ops;
                }
            }
        });
    }

    // The writer: unthrottled overwrites during warm-up and MEASURE.
    std::uint64_t writes_ok = 0, writes_failed = 0, held_max = 0, backlog_max = 0;
    bool stopped_at_limit = false;
    std::thread writer([&]() {
        if (writer_cpu >= 0) bench::pin_this_thread(writer_cpu);
        bench::Rng rng(seed * 7777 + 99);
        std::vector<unsigned char> v(value_bytes, 0x5A);
        std::uint64_t generation = 2, n = 0;
        while (phase.load(std::memory_order_relaxed) == kWait) std::this_thread::yield();
        for (;;) {
            const int ph = phase.load(std::memory_order_relaxed);
            if (ph != kWarmup && ph != kMeasure) break;
            const long key = static_cast<long>(rng.next() % static_cast<std::uint64_t>(keys));
            fill_value(v, key, generation++);
            const bool ok = store.put(key, v.data(), v.size());
            if (ph == kMeasure) {
                if (ok) ++writes_ok;
                else ++writes_failed;
            }
            if ((++n & 63) == 0) {
                const std::uint64_t held = NovaKvInspector::memory_held_bytes(store);
                const std::uint64_t backlog = NovaKvInspector::backlog(store);
                if (held > held_max) held_max = held;
                if (backlog > backlog_max) backlog_max = backlog;
                if (held > retain_limit) {
                    stopped_at_limit = true;
                    break;
                }
            }
        }
    });

    // The pinner: Snapshot held for pin_hold_ms, released, pin_gap_ms pause.
    std::uint64_t pin_cycles = 0;
    std::thread pinner([&]() {
        if (pinner_cpu >= 0) bench::pin_this_thread(pinner_cpu);
        while (phase.load(std::memory_order_relaxed) == kWait) std::this_thread::yield();
        while (!pinner_stop.load(std::memory_order_relaxed)) {
            {
                KVStore::Snapshot snap = store.acquire_snapshot();
                const std::uint64_t until = bench::now_ns() + static_cast<std::uint64_t>(pin_hold_ms) * 1000000ULL;
                while (bench::now_ns() < until && !pinner_stop.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            ++pin_cycles;
            const std::uint64_t until = bench::now_ns() + static_cast<std::uint64_t>(pin_gap_ms) * 1000000ULL;
            while (bench::now_ns() < until && !pinner_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });

    phase.store(kWarmup);
    std::this_thread::sleep_for(std::chrono::duration<double>(warmup_s));
    const std::uint64_t measure_start = bench::now_ns();
    phase.store(kMeasure);
    // A final snapshot, taken pin_hold_ms before the end of MEASURE, makes sure
    // the cleanup phase starts with a backlog whatever the pinner's phase is.
    const double hold_s = static_cast<double>(pin_hold_ms) / 1000.0;
    std::this_thread::sleep_for(std::chrono::duration<double>(duration_s > hold_s ? duration_s - hold_s : 0.0));
    std::uint64_t backlog_at_release = 0, drain_ns = 0;
    bool drained = false;
    double seconds = 0;
    {
        KVStore::Snapshot final_pin = store.acquire_snapshot();
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s > hold_s ? hold_s : duration_s));
        seconds = static_cast<double>(bench::now_ns() - measure_start) / 1e9;
        phase.store(kCleanup);          // the writer stops; readers go on
        writer.join();
        pinner_stop.store(true);
        pinner.join();
        backlog_at_release = NovaKvInspector::backlog(store);
    }                                   // final_pin released here: the cleaner may now free the backlog
    const std::uint64_t release_ns = bench::now_ns();
    // The stop-the-world comparator sweeps only when the retire list has reached
    // its trigger (kStwBacklogTrigger in versioned_store.cpp), so a shorter list
    // stays: that is its resting state, not a failure to drain. The wait ends
    // early once the list is below the trigger and has stopped changing.
    const bool stw = reclaim == "stw";
    bool stw_at_rest = false;
    if (reclaim != "disabled") {
        const std::uint64_t deadline = release_ns + 20ULL * 1000000000ULL;
        std::uint64_t last = NovaKvInspector::backlog(store), last_change_ns = release_ns;
        while (last != 0 && bench::now_ns() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            const std::uint64_t now_backlog = NovaKvInspector::backlog(store);
            const std::uint64_t now = bench::now_ns();
            if (now_backlog != last) {
                last = now_backlog;
                last_change_ns = now;
            } else if (stw && last < kStwTrigger && now - last_change_ns > 50ULL * 1000000ULL) {
                stw_at_rest = true;
                break;
            }
        }
        drained = NovaKvInspector::backlog(store) == 0;
        drain_ns = bench::now_ns() - release_ns;
    }
    phase.store(kStop);
    for (auto& t : threads) t.join();

    const std::uint64_t held_end = NovaKvInspector::memory_held_bytes(store);
    if (reclaim != "disabled") NovaKvInspector::settle(store);   // also records the cleaner's CPU time
    const NovaKvStats st = NovaKvInspector::stats(store);

    LatencyHistogram measure, cleanup;
    std::uint64_t ops = 0, cleanup_ops = 0, corrupt = 0, missing = 0;
    for (const ReaderResult& r : results) {
        measure.merge(r.measure);
        cleanup.merge(r.cleanup);
        ops += r.ops;
        cleanup_ops += r.cleanup_ops;
        corrupt += r.corrupt;
        missing += r.missing;
    }
    const std::uint64_t reclaimed = st.versions_reclaimed - st0.versions_reclaimed;
    const std::uint64_t passes = st.reclaim_passes - st0.reclaim_passes;
    const double cleaner_cpu_s = static_cast<double>(st.cleaner_cpu_ns) / 1e9;

    std::printf("reclaim_bench variant=%s reclaim=%s batch=%d readers=%d keys=%ld value_bytes=%zu pin=%ldms/%ldms\n",
                nova_kv_sync_mode_name(), reclaim.c_str(), st.reclaim_batch, readers, keys, value_bytes, pin_hold_ms,
                pin_gap_ms);
    std::printf("  measure: %.3f s  reads=%llu rps=%.0f p50=%llu ns p99=%llu ns p999=%llu ns max=%llu ns\n", seconds,
                static_cast<unsigned long long>(ops), static_cast<double>(ops) / seconds,
                static_cast<unsigned long long>(measure.percentile(0.50)),
                static_cast<unsigned long long>(measure.percentile(0.99)),
                static_cast<unsigned long long>(measure.percentile(0.999)),
                static_cast<unsigned long long>(measure.max()));
    std::printf("  writer : ok=%llu failed=%llu wps=%.0f stopped_at_limit=%d  pin_cycles=%llu\n",
                static_cast<unsigned long long>(writes_ok), static_cast<unsigned long long>(writes_failed),
                static_cast<double>(writes_ok) / seconds, stopped_at_limit ? 1 : 0,
                static_cast<unsigned long long>(pin_cycles));
    std::printf("  backlog: max=%llu at_release=%llu  held idle=%.1f MiB max=%.1f MiB at_end=%.1f MiB\n",
                static_cast<unsigned long long>(backlog_max), static_cast<unsigned long long>(backlog_at_release),
                static_cast<double>(held_idle) / 1048576.0, static_cast<double>(held_max) / 1048576.0,
                static_cast<double>(held_end) / 1048576.0);
    std::printf("  cleanup: drained=%d in %.3f ms  reads=%llu max=%llu ns p999=%llu ns\n", drained ? 1 : 0,
                static_cast<double>(drain_ns) / 1e6, static_cast<unsigned long long>(cleanup_ops),
                static_cast<unsigned long long>(cleanup.max()),
                static_cast<unsigned long long>(cleanup.percentile(0.999)));
    if (stw_at_rest) {
        std::printf("  cleanup: the retire list stayed below the stop-the-world trigger (%llu); nothing more is freed\n",
                    static_cast<unsigned long long>(kStwTrigger));
    }
    std::printf("  reclaim: versions=%llu passes=%llu max_per_pass=%llu stw_pauses=%llu cleaner_cpu=%.3f s\n",
                static_cast<unsigned long long>(reclaimed), static_cast<unsigned long long>(passes),
                static_cast<unsigned long long>(st.reclaim_max_versions_in_pass),
                static_cast<unsigned long long>(st.stw_pauses), cleaner_cpu_s);

    bool valid = true;
    auto require = [&](bool condition, const char* what) {
        if (condition) return;
        valid = false;
        std::fprintf(stderr, "INVALID: %s\n", what);
    };
    require(corrupt == 0, "a reader saw a value that does not belong to its key");
    require(missing == 0, "a reader missed a key that exists");
    require(writes_ok > 0, "the writer completed no update in the measured window");
    require(ops > 0, "no read completed");
    if (reclaim == "epoch") {
        require(st.reclaim_max_versions_in_pass <= static_cast<std::uint64_t>(st.reclaim_batch),
                "a reclamation pass freed more versions than the batch");
    }
    if (reclaim != "disabled") {
        require(drained || stw_at_rest, "the backlog did not drain after the last snapshot was released");
    }
    require(NovaKvInspector::validate(store) == nullptr, "store invariants");

    if (!hist_path.empty()) {
        std::FILE* f = std::fopen(hist_path.c_str(), "w");
        if (f != nullptr) {
            std::fputs("series,latency_ns_lower_bound,count\n", f);
            measure.dump(f, "read_measure");
            cleanup.dump(f, "read_cleanup");
            std::fclose(f);
        } else {
            require(false, "cannot write the histogram file");
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
        if (!exists) {
            std::fputs("run_id,git_sha,variant,scenario,readers,reclaim_mode,reclaim_batch,keys,value_bytes,"
                       "pin_hold_ms,pin_gap_ms,seconds_actual,read_ops,read_rps,p50_ns,p99_ns,p999_ns,max_ns,"
                       "write_ok,write_failed,write_rps,pin_cycles,backlog_max,backlog_at_release,"
                       "held_idle_bytes,held_max_bytes,held_end_bytes,drained,drain_ms,cleanup_read_ops,"
                       "cleanup_p999_ns,cleanup_max_ns,versions_reclaimed,reclaim_passes,"
                       "reclaim_max_versions_in_pass,stw_pauses,cleaner_cpu_s,stopped_at_retain_limit,valid\n",
                       f);
        }
        std::fprintf(f,
                     "%s,%s,%s,%s,%d,%s,%d,%ld,%zu,%ld,%ld,%.6f,%llu,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%llu,"
                     "%llu,%llu,%llu,%llu,%llu,%d,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%d,%d\n",
                     run_id.c_str(), git_sha.c_str(), nova_kv_sync_mode_name(), scenario.c_str(), readers,
                     reclaim.c_str(), st.reclaim_batch, keys, value_bytes, pin_hold_ms, pin_gap_ms, seconds,
                     static_cast<unsigned long long>(ops), static_cast<double>(ops) / seconds,
                     static_cast<unsigned long long>(measure.percentile(0.50)),
                     static_cast<unsigned long long>(measure.percentile(0.99)),
                     static_cast<unsigned long long>(measure.percentile(0.999)),
                     static_cast<unsigned long long>(measure.max()), static_cast<unsigned long long>(writes_ok),
                     static_cast<unsigned long long>(writes_failed), static_cast<double>(writes_ok) / seconds,
                     static_cast<unsigned long long>(pin_cycles), static_cast<unsigned long long>(backlog_max),
                     static_cast<unsigned long long>(backlog_at_release), static_cast<unsigned long long>(held_idle),
                     static_cast<unsigned long long>(held_max), static_cast<unsigned long long>(held_end),
                     drained ? 1 : 0, static_cast<double>(drain_ns) / 1e6,
                     static_cast<unsigned long long>(cleanup_ops),
                     static_cast<unsigned long long>(cleanup.percentile(0.999)),
                     static_cast<unsigned long long>(cleanup.max()), static_cast<unsigned long long>(reclaimed),
                     static_cast<unsigned long long>(passes),
                     static_cast<unsigned long long>(st.reclaim_max_versions_in_pass),
                     static_cast<unsigned long long>(st.stw_pauses), cleaner_cpu_s, stopped_at_limit ? 1 : 0,
                     valid ? 1 : 0);
        std::fclose(f);
    }
    std::printf("valid=%d\n", valid ? 1 : 0);
    return valid ? 0 : 1;
}
